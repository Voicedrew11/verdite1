using System.Runtime.InteropServices;

namespace RecompOne.Runtime;

/// <summary>
/// 0072. The area's geometry kept on the GPU in world space, so a reflection can
/// draw the scene again from any camera without the game's own code running twice.
///
/// <para>The port fills it: the static map from the map data once an area settles
/// (<see cref="SetStatic"/>), and every frame the camera the frame was drawn with and
/// the models it submitted (<see cref="BeginFrame"/>, <see cref="AddDynamic"/>). The
/// GL core backend draws it with a world-space vertex shader in front of the game's
/// own prim fragment shader, so a reflected texel is decoded, filtered and lit by
/// the same code as a drawn one.</para>
///
/// <para>Everything is drawn at present, for the target being presented, with the
/// camera and the models of the frame that target holds: with two display buffers
/// that is a frame ago, so the frames are kept in a short ring by serial and the
/// target remembers the serial it was drawn under.</para>
/// </summary>
public static class RetainedScene
{
    /// <summary>One corner, as the backend uploads it.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Vertex
    {
        /// <summary>World position, the game's units.</summary>
        public float X, Y, Z;
        /// <summary>The lit colour before the depth cue, 0..255.</summary>
        public float R, G, B;
        /// <summary>As the GP0 packet carries them; texpage bit 15 is untextured.</summary>
        public float Clut, Texpage;
        public float U, V;
        /// <summary>The depth cue this corner is fogged with: DQA, DQB, and the curve
        /// in GteLightMap's numbering (0 none, 1 offset, 2 knee).</summary>
        public float Dqa, Dqb, Curve;
        /// <summary>0060's texture rectangle, u0 v0 u1 v1 a byte each.</summary>
        public uint Rect;
        /// <summary>Bit 31 the rectangle is valid, bit 10 semi-transparent, bits 8-9
        /// the blend mode, the low byte the material.</summary>
        public uint Flags;
    }

    public const uint FlagRect = 0x80000000u, FlagSemi = 0x400u;

    /// <summary>0077. A blended model that stands for something solid (a door): it casts
    /// a shadow with every texel. Any other blended face casts with the texels the GPU
    /// draws opaque, those without the semi-transparency bit.</summary>
    public const uint FlagSolid = 0x800u;

    /// <summary>0077. An effect (a spark, a flame): drawn, but it casts no shadow.</summary>
    public const uint FlagNoShadow = 0x1000u;

    /// <summary>Bits 13-26: the map half a static corner belongs to, plus one
    /// ((tile Z * 80 + tile X) * 2 + upper), 0 for anything else. A reflection draws
    /// only the halves the game's own walk drew in that frame
    /// (<see cref="Frame.Halves"/>).</summary>
    public const int HalfShift = 13;
    public const uint HalfBits = 0x3FFFu;
    public const int HalvesW = 160, HalvesH = 80;

    public static uint HalfFlag(int tx, int tz, int upper) => (uint)((tz * 80 + tx) * 2 + upper + 1) << HalfShift;

    /// <summary>Reflect only the halves the game drew in the frame. Off, every half on
    /// the map is reflected, including those its visibility flood leaves out.</summary>
    public static bool HalfGate = true;

    /// <summary>The port's switch: draw reflections from this scene. The reflection
    /// pass runs for it whether or not the screen march is on.</summary>
    public static bool Enabled
    {
        get => _on;
        set { _on = value; ScreenReflections.Refresh(); }
    }
    static bool _on;

    /// <summary>0077. The port's authored lights want the static map for their shadows,
    /// whether or not reflections are drawn from it.</summary>
    public static bool ShadowsWanted;

    /// <summary>0077. The frame's models cast into the lights' cubemaps too; the port
    /// captures them while this and <see cref="ShadowsWanted"/> hold.</summary>
    public static bool ShadowModels = true;

    public static bool ShadowModelsWanted => ShadowsWanted && ShadowModels;

    /// <summary>Set by the GL core backend once its world program built.</summary>
    public static bool Supported;

    /// <summary>Draw planar reflections from it, and the camera cubemap.</summary>
    public static bool Planar = true, Cube = true;

    /// <summary>A cubemap face's size, in pixels, and the steps the reflection
    /// pass marches it in.</summary>
    public static int CubeSize = 256, CubeSteps = 48;

    /// <summary>Cull the faces a view sees from behind, as the game's assemblers
    /// do: a mirror or a cube face shows only what the game would draw from there.
    /// Off, the underside of every floor above a plane is reflected.</summary>
    public static bool CullBack = true;

    /// <summary>The probe's switch: GPU time per draw, read back with a query.</summary>
    public static bool Probe;

    // ---- the static map ----------------------------------------------------------

    static Vertex[] _static = [];
    static int _staticCount;

    /// <summary>Bumped on every <see cref="SetStatic"/>; the backend re-uploads.</summary>
    public static int StaticGeneration { get; private set; }

    public static ReadOnlySpan<Vertex> Static => _static.AsSpan(0, _staticCount);

    /// <summary>The static map is kept in chunks of <see cref="ChunkTiles"/> tiles a
    /// side, so a view draws only the chunks it can see. Sorted by range (opaque,
    /// then each blend mode) and within a range by chunk; a chunk's triangles in a
    /// range are <see cref="ChunkStart"/>/<see cref="ChunkCount"/> at
    /// <c>range * Chunks + chunk</c>.</summary>
    public const int ChunkTiles = 8, ChunkSide = 80 / ChunkTiles, Chunks = ChunkSide * ChunkSide;
    const float ChunkUnits = ChunkTiles * 2048f;

    public static readonly int[] ChunkStart = new int[5 * Chunks], ChunkCount = new int[5 * Chunks];

    /// <summary>Each chunk's bounds in world space, and whether it holds anything.</summary>
    public static readonly float[] ChunkMin = new float[Chunks * 3], ChunkMax = new float[Chunks * 3];
    public static readonly bool[] ChunkUsed = new bool[Chunks];

    /// <summary>Each chunk's latest fog: the depth-cue quotient (H*65536/z) at and
    /// below which its last corner is black, 0 when a corner never goes black; and
    /// that corner's DQA, DQB and curve, for the probe.</summary>
    public static readonly float[] ChunkFogQ = new float[Chunks];
    public static readonly Vertex[] ChunkFogOf = new Vertex[Chunks];

    /// <summary>Draw ranges in <see cref="Static"/> as a whole: opaque first, then
    /// the semi-transparent triangles by blend mode.</summary>
    public static readonly int[] StaticStart = new int[5], StaticCount = new int[5];

    /// <summary>The static map, replaced whole. Sorted here into ranges and chunks.</summary>
    public static void SetStatic(ReadOnlySpan<Vertex> tris)
    {
        if (_static.Length < tris.Length) _static = new Vertex[tris.Length];
        int n = tris.Length / 3 * 3;
        Array.Clear(ChunkCount);
        Array.Clear(ChunkUsed);
        for (int c = 0; c < Chunks; c++)
        {
            ChunkFogQ[c] = float.MaxValue;
            ChunkMin[c * 3] = ChunkMin[c * 3 + 1] = ChunkMin[c * 3 + 2] = float.MaxValue;
            ChunkMax[c * 3] = ChunkMax[c * 3 + 1] = ChunkMax[c * 3 + 2] = float.MinValue;
        }
        var key = new int[n / 3];
        for (int i = 0, t = 0; i < n; i += 3, t++)
        {
            int c = ChunkOf(tris[i], tris[i + 1], tris[i + 2]);
            key[t] = Range(tris[i].Flags) * Chunks + c;
            ChunkCount[key[t]] += 3;
            ChunkUsed[c] = true;
            for (int k = 0; k < 3; k++)
            {
                ref readonly var v = ref tris[i + k];
                ChunkMin[c * 3] = Math.Min(ChunkMin[c * 3], v.X); ChunkMax[c * 3] = Math.Max(ChunkMax[c * 3], v.X);
                ChunkMin[c * 3 + 1] = Math.Min(ChunkMin[c * 3 + 1], v.Y); ChunkMax[c * 3 + 1] = Math.Max(ChunkMax[c * 3 + 1], v.Y);
                ChunkMin[c * 3 + 2] = Math.Min(ChunkMin[c * 3 + 2], v.Z); ChunkMax[c * 3 + 2] = Math.Max(ChunkMax[c * 3 + 2], v.Z);
                float q = BlackQuotient(v);
                if (q < ChunkFogQ[c]) { ChunkFogQ[c] = q; ChunkFogOf[c] = v; }
            }
        }
        int at = 0;
        for (int k = 0; k < 5 * Chunks; k++) { ChunkStart[k] = at; at += ChunkCount[k]; }
        var put = new int[5 * Chunks];
        Array.Copy(ChunkStart, put, put.Length);
        for (int i = 0, t = 0; i < n; i += 3, t++)
        {
            int k = key[t];
            _static[put[k]] = tris[i]; _static[put[k] + 1] = tris[i + 1]; _static[put[k] + 2] = tris[i + 2];
            put[k] += 3;
        }
        for (int r = 0; r < 5; r++)
        {
            StaticStart[r] = ChunkStart[r * Chunks];
            int sum = 0;
            for (int c = 0; c < Chunks; c++) sum += ChunkCount[r * Chunks + c];
            StaticCount[r] = sum;
        }
        _staticCount = n;
        StaticGeneration++;
    }

    /// <summary>The quotient H*65536/z at and below which a corner's depth cue
    /// leaves nothing (the curves WorldVs fogs with), or 0 when it never does.</summary>
    public static float BlackQuotient(in Vertex v)
    {
        int curve = (int)(v.Curve + 0.5f);
        if (curve == 0 || v.Dqa >= 0f) return 0f;
        float ir0 = curve == 1 ? 2848f : 3232f;
        return Math.Max(0f, (ir0 * 4096f - v.Dqb) / v.Dqa);
    }

    /// <summary>How much of a corner's colour survives the cue at view depth z.</summary>
    public static float FogKeep(in Vertex v, float h, float z)
    {
        int curve = (int)(v.Curve + 0.5f);
        if (curve == 0) return 1f;
        float q = Math.Min(h * 65536f / Math.Max(z, 1f), 131071f);
        float ir0 = Math.Clamp((v.Dqa * q + v.Dqb) / 4096f, 0f, 4096f);
        float w = curve == 1 ? Math.Max(ir0 - 800f, 0f) * 2f : ir0 < 2800f ? ir0 : 3f * ir0 - 5600f;
        return Math.Clamp(1f - w / 4096f, 0f, 1f);
    }

    static int ChunkOf(in Vertex a, in Vertex b, in Vertex c)
    {
        int cx = Math.Clamp((int)((a.X + b.X + c.X) / 3f / ChunkUnits), 0, ChunkSide - 1);
        int cz = Math.Clamp((int)((a.Z + b.Z + c.Z) / 3f / ChunkUnits), 0, ChunkSide - 1);
        return cz * ChunkSide + cx;
    }

    static int Sort(ReadOnlySpan<Vertex> tris, Vertex[] dst, int[] start, int[] count)
    {
        Array.Clear(count);
        int n = tris.Length / 3 * 3;
        for (int i = 0; i < n; i += 3) count[Range(tris[i].Flags)] += 3;
        int at = 0;
        for (int r = 0; r < 5; r++) { start[r] = at; at += count[r]; }
        Span<int> put = stackalloc int[5];
        for (int r = 0; r < 5; r++) put[r] = start[r];
        for (int i = 0; i < n; i += 3)
        {
            int r = Range(tris[i].Flags);
            dst[put[r]] = tris[i]; dst[put[r] + 1] = tris[i + 1]; dst[put[r] + 2] = tris[i + 2];
            put[r] += 3;
        }
        return n;
    }

    static int Range(uint flags) => (flags & FlagSemi) == 0 ? 0 : 1 + (int)((flags >> 8) & 3);

    // ---- the frames --------------------------------------------------------------

    /// <summary>A camera: the GTE's world-to-view rotation at its 4096 scale
    /// divided out, the camera's world position, the view translation the game's
    /// matrix carries, and its projection (H and the centre, in the game's pixels).</summary>
    public struct View
    {
        public float R00, R01, R02, R10, R11, R12, R20, R21, R22;
        public double CamX, CamY, CamZ;
        public float Tx, Ty, Tz;
        public float H, Cx, Cy;
    }

    /// <summary>A plane a planar reflection mirrors in: world Y, Y being down.</summary>
    public const int MaxPlanes = 4;

    public sealed class Frame
    {
        public int Serial = -1;
        public View View;
        public Vertex[] Dynamic = new Vertex[4096];
        public int DynamicCount;
        public readonly int[] DynStart = new int[5], DynCount = new int[5];
        public Vertex[] Sorted = new Vertex[4096];
        public bool SortedValid;
        public readonly float[] Planes = new float[MaxPlanes];
        public int PlaneCount;
        /// <summary>1 for each map half the frame's own tile walk drew, by
        /// <c>(tile Z * 80 + tile X) * 2 + upper</c>.</summary>
        public readonly byte[] Halves = new byte[HalvesW * HalvesH];

        public ReadOnlySpan<Vertex> SortedDynamic()
        {
            if (!SortedValid)
            {
                if (Sorted.Length < DynamicCount) Sorted = new Vertex[Dynamic.Length];
                Sort(Dynamic.AsSpan(0, DynamicCount), Sorted, DynStart, DynCount);
                SortedValid = true;
            }
            return Sorted.AsSpan(0, DynamicCount / 3 * 3);
        }
    }

    const int Ring = 4;
    static readonly Frame[] _frames = [new(), new(), new(), new()];
    static int _serial;

    /// <summary>The serial of the frame being built, which a target drawn now
    /// records.</summary>
    public static int Serial => _serial;

    static Frame Current => _frames[_serial % Ring];

    /// <summary>A new frame, drawn with <paramref name="view"/>. The models the
    /// port adds after this belong to it.</summary>
    public static void BeginFrame(in View view)
    {
        _serial++;
        var f = Current;
        f.Serial = _serial;
        f.View = view;
        f.DynamicCount = 0;
        f.SortedValid = false;
        f.PlaneCount = 0;
        Array.Clear(f.Halves);
    }

    /// <summary>A map half the current frame's walk drew.</summary>
    public static void NoteHalf(int tx, int tz, int upper)
    {
        var f = Current;
        if (f.Serial == _serial && (uint)tx < 80u && (uint)tz < 80u) f.Halves[(tz * 80 + tx) * 2 + upper] = 1;
    }

    /// <summary>One model's triangles, in world space, to the current frame.</summary>
    public static void AddDynamic(ReadOnlySpan<Vertex> tris)
    {
        var f = Current;
        if (f.Serial != _serial) return;
        int n = tris.Length / 3 * 3;
        if (f.DynamicCount + n > f.Dynamic.Length)
            Array.Resize(ref f.Dynamic, Math.Max(f.Dynamic.Length * 2, f.DynamicCount + n));
        tris[..n].CopyTo(f.Dynamic.AsSpan(f.DynamicCount));
        f.DynamicCount += n;
        f.SortedValid = false;
    }

    /// <summary>The planes the current frame mirrors in, nearest-first by the
    /// port's own ranking; at most <see cref="MaxPlanes"/>.</summary>
    public static void SetPlanes(ReadOnlySpan<float> worldY)
    {
        var f = Current;
        int n = Math.Min(worldY.Length, MaxPlanes);
        worldY[..n].CopyTo(f.Planes);
        f.PlaneCount = n;
    }

    public static Frame? Find(int serial)
    {
        if (serial <= 0) return null;
        var f = _frames[serial % Ring];
        return f.Serial == serial ? f : null;
    }

    // ---- what the probe reads ----------------------------------------------------

    /// <summary>Planar and cubemap draws made, triangles submitted, and the GPU
    /// time the last of each took, in nanoseconds (0 while the probe is off).</summary>
    public static long PlanarDraws, CubeDraws, Triangles;
    public static long PlanarGpuNs, CubeGpuNs;

    /// <summary>Static chunks drawn, of those tested, over every view.</summary>
    public static long ChunksDrawn, ChunksTested;

    /// <summary>Presents that had a frame to draw, and those whose target's frame
    /// had fallen out of the ring.</summary>
    public static long Found, Missed;

    /// <summary>The probe's, per planar pass: pixels whose nearest opaque face is a
    /// front face, and those a back face would have covered with culling off.</summary>
    public static long FrontPixels, BackPixels;

    /// <summary>The probe's, per planar pass: pixels a map half the game did not draw
    /// that frame would have taken with the gate off.</summary>
    public static long UndrawnPixels;

    /// <summary>The probe's: mirrored chunks drawn that the old distance cull
    /// (straight-line distance past one fog's black) dropped, and the most of a
    /// colour any of them keeps at its nearest depth.</summary>
    public static long OldCullVisible;
    public static float OldCullKeep;

    public static void ResetCounters()
    {
        PlanarDraws = CubeDraws = Triangles = Found = Missed = ChunksDrawn = ChunksTested = 0;
        FrontPixels = BackPixels = OldCullVisible = UndrawnPixels = 0;
        OldCullKeep = 0f;
    }
}
