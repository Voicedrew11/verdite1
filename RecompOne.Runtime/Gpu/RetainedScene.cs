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
        /// <summary>The colour the game lit this corner from (the GTE's RGBC, low 24
        /// bits), which an authored light and a glow scale as they do the game's own
        /// face; 0 leaves the corner out of both.</summary>
        public uint Rgbc;
        /// <summary>0085. A static map corner lit in the vertex shader from the light
        /// records (<see cref="Records"/>) rather than carrying its colour and cue:
        /// <see cref="LightRecord"/> set, the half's record and quarter turn, and the
        /// three records EvenFog blends it with. R, G and B are then the face's normal,
        /// and Dqa and Dqb the corner's two blend weights (TileWeights' ax and az).
        /// 0 for everything else.</summary>
        public uint Light;
    }

    /// <summary>0085. <see cref="Vertex.Light"/>: bit 31 lit from the records; bits
    /// 0-5 the half's record, 6-7 its quarter turn, 8-13, 14-19 and 20-25 the records
    /// of the neighbours along X, along Z and on the diagonal, 26-28 whether each is
    /// there, 29 the fog blended between them, 30 the light.</summary>
    public const uint LightRecord = 0x80000000u, LightFogBlend = 0x20000000u, LightLitBlend = 0x40000000u;

    public static uint PackLight(int own, int rot, int x, int z, int d, bool hasX, bool hasZ, bool hasD, bool fog, bool light) =>
        LightRecord | (uint)own | (uint)rot << 6 | (uint)x << 8 | (uint)z << 14 | (uint)d << 20
        | (hasX ? 1u << 26 : 0u) | (hasZ ? 1u << 27 : 0u) | (hasD ? 1u << 28 : 0u)
        | (fog ? LightFogBlend : 0u) | (light ? LightLitBlend : 0u);

    // ---- 0085. the light records --------------------------------------------------

    /// <summary>The area's 64 light records as the vertex shader reads them, 52 ints
    /// each: the light matrix at each quarter turn (4 x 9), the colour matrix (9), the
    /// back colour (3, shifted as the GTE takes it), the fog word, its DQA and DQB,
    /// and its curve.</summary>
    public const int RecordInts = 52, RecordCount = 64;
    public const int RecLcm = 36, RecBk = 45, RecWord = 48, RecDqa = 49, RecDqb = 50, RecCurve = 51;
    public static readonly int[] Records = new int[RecordCount * RecordInts];

    /// <summary>Bumped by <see cref="SetRecords"/>; the backend re-uploads.</summary>
    public static int RecordGeneration { get; private set; }

    /// <summary>The probe's: the records' uploads.</summary>
    public static long RecordUploads;

    /// <summary>The records changed: the backend uploads them, and each chunk's fog
    /// bound is taken again from the records its corners use.</summary>
    public static void SetRecords(ReadOnlySpan<int> records)
    {
        records[..Records.Length].CopyTo(Records);
        RecordGeneration++;
        RecordFog();
    }

    // The records each chunk's corners are lit or fogged from, and its bound from
    // the corners that carry their own cue.
    static readonly ulong[] _chunkRecords = new ulong[Chunks];
    static readonly float[] _chunkPlainQ = new float[Chunks];
    static readonly Vertex[] _chunkPlainOf = new Vertex[Chunks];

    /// <summary>A chunk lit from the records goes black no nearer than the nearest of
    /// its records does at the knee's end (3232), which bounds any blend of them.</summary>
    static void RecordFog()
    {
        for (int c = 0; c < Chunks; c++)
        {
            ChunkFogQ[c] = _chunkPlainQ[c];
            ChunkFogOf[c] = _chunkPlainOf[c];
            ulong m = _chunkRecords[c];
            if (m == 0) continue;
            float best = float.MaxValue;
            int of = 0;
            for (int r = 0; r < RecordCount; r++)
            {
                if ((m >> r & 1ul) == 0) continue;
                int at = r * RecordInts;
                float dqa = Records[at + RecDqa], dqb = Records[at + RecDqb];
                float q = Records[at + RecCurve] == 0 || Records[at + RecCurve] == LinearDepthCue.Curve || dqa >= 0f
                    ? 0f : Math.Max(0f, (3232f * 4096f - dqb) / dqa);
                if (q < best) { best = q; of = r; }
            }
            if (best >= ChunkFogQ[c]) continue;
            ChunkFogQ[c] = best;
            ChunkFogOf[c] = new Vertex { Dqa = Records[of * RecordInts + RecDqa], Dqb = Records[of * RecordInts + RecDqb], Curve = 2f };
        }
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

    /// <summary>0085. A corner the port's swell may move (WaterSwell's free positions),
    /// and a blended face the surface buffer takes as water.</summary>
    public const uint FlagSwell = 0x8000000u, FlagWater = 0x10000000u;

    /// <summary>0085. The second triangle of a quad, which the game sorts with the first
    /// as one packet.</summary>
    public const uint FlagQuadTail = 0x20000000u;

    /// <summary>0085. A model corner whose colour is the three light dots of its normal
    /// (a gouraud face), lit per pixel with its group's BK and LCM, not a lit colour.</summary>
    public const uint FlagDots = 0x40000000u;

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

    // ---- 0085: the main view -------------------------------------------------------

    /// <summary>0085. The port's switch: the static map's opaque range is drawn into
    /// the frame by the backend, at the head of the ordering table's walk, and the
    /// port stops assembling those faces.</summary>
    public static bool MainView;

    /// <summary>0085. The frame whose map the next table walk draws, set by the port
    /// when its tile walk starts; 0 once drawn.</summary>
    public static int MainSerial;

    /// <summary>0085. The backend's draw, given the GPU's draw offset: true when the
    /// map went into a display target.</summary>
    public static Func<int, int, bool>? MainDrawer;

    /// <summary>0085. Main-view draws made, walks that found no target or no frame,
    /// and static triangles submitted.</summary>
    public static long MainDraws, MainMissed, MainTriangles;
    /// <summary>Map draws (main view and mirror) taken in two passes for 0051's tolerance.</summary>
    public static long MainMapPrepasses;
    /// <summary>The map's triangles the normal pass drew for the surface buffers.</summary>
    public static long MainNormalTriangles;
    /// <summary>0085's probe: read the surface buffer back against the frame's depth
    /// when set, and clear it. Of the pixels with a depth: those whose surface lies
    /// behind it, those whose opaque surface lies in front of it (a face the colour
    /// pass drew no depth for, as a billboard's transparent texels: AO's box round a
    /// sprite), and those with no surface at all.</summary>
    public static bool SurfaceCheck;
    /// <summary>Whether the map drawn on the GPU goes into the normal and surface
    /// buffers; off is the comparison (the map missing from both).</summary>
    public static bool MainSurfaces = true;
    /// <summary>The main view's near plane, in view depth; the game's own clipper keeps
    /// everything in front of 0.</summary>
    public static float MainNear = 16f;
    /// <summary>The main view's per-pixel fog from each pixel's own depth; off is the
    /// screen-affine corner value, which a face clipped at the eye gets wrong.</summary>
    public static bool MainFogFromZ = true;
    public static long SurfaceDepthPixels, SurfaceBehind, SurfaceAhead, SurfaceMissing, SurfaceChecks;
    /// <summary>The probe's readback by surface id: none, opaque, water, overlay, an
    /// authored id, a blended one.</summary>
    public static readonly long[] SurfaceIds = new long[6];

    // ---- 0085. the depth-stage probe ------------------------------------------------

    /// <summary>0085's depth-stage probe, off by default: sample the frame target's
    /// depth at two stages, once every five seconds. Numeric FBO reads only.</summary>
    public static bool DepthStageProbe;

    /// <summary>The probe's stages: after the retained main draw, and the present's source.</summary>
    public const int ProbeMain = 0, ProbePresent = 1, ProbeStages = 2;

    /// <summary>How long the probe waits between runs.</summary>
    public const long ProbePeriodMs = 5000;

    static long _probeAt;
    static bool _probeArmed;

    /// <summary>Whether the after-main stage is due; the runtime owns the cadence.</summary>
    public static bool ProbeMainDue => DepthStageProbe && Environment.TickCount64 >= _probeAt;

    /// <summary>Whether the present stage follows the after-main sample that armed it.</summary>
    public static bool ProbePresentDue => DepthStageProbe && _probeArmed;

    /// <summary>The after-main sample was taken: wait out the period, arm the present stage.</summary>
    public static void ProbeMainTaken() { _probeAt = Environment.TickCount64 + ProbePeriodMs; _probeArmed = true; }

    /// <summary>The present sample was taken.</summary>
    public static void ProbePresentTaken() => _probeArmed = false;

    /// <summary>The probe's runs, samples, depth-carrying samples, and stages with no
    /// target or no depth attachment, by stage.</summary>
    public static readonly long[] ProbeRuns = new long[ProbeStages];
    public static readonly long[] ProbeSamples = new long[ProbeStages];
    public static readonly long[] ProbeDepth = new long[ProbeStages];
    public static readonly long[] ProbeTargetAbsent = new long[ProbeStages], ProbeDepthAbsent = new long[ProbeStages];

    /// <summary>The samples with no surface buffer to read an id from, and the samples
    /// whose buffer is not the sampled frame's (the pass runs at present), by stage.</summary>
    public static readonly long[] ProbeSurfaceAbsent = new long[ProbeStages], ProbeSurfaceStale = new long[ProbeStages];

    /// <summary>The present stage's ids sampled, six a stage in <see cref="SurfaceIds"/>'
    /// order; the after-main stage reads no ids, its buffer being an older frame's.</summary>
    public static readonly long[] ProbeSurfaceIds = new long[ProbeStages * 6];

    /// <summary>The probe's last reading, by stage: target, frame, retained serial, size.</summary>
    public static readonly uint[] ProbeTargetFbo = new uint[ProbeStages];
    public static readonly long[] ProbeFrame = new long[ProbeStages];
    public static readonly int[] ProbeTargetSerial = new int[ProbeStages];
    public static readonly int[] ProbeTargetW = new int[ProbeStages], ProbeTargetH = new int[ProbeStages];

    /// <summary>Present/after-main pairs taken, and whether the two read the same
    /// target and the same retained serial; a pair that did not is still counted.</summary>
    public static long ProbePairRuns, ProbePairFboMatch, ProbePairFboMismatch;
    public static long ProbePairSerialMatch, ProbePairSerialMismatch;

    /// <summary>0085. The port's switch: the map's blended faces (water) are drawn by
    /// the backend too, a slice of view depth at a time where the table's walk would
    /// have drawn their packets (<see cref="WaterDrawer"/>).</summary>
    public static bool MainWater = true;

    /// <summary>0085. Set by the backend when the main view drew and the frame has
    /// water to draw; the table walk then calls <see cref="WaterDrawer"/>.</summary>
    public static bool WaterPending;

    /// <summary>0085. The backend's water draw: the walk has passed view depth
    /// <c>cut</c> and is about to draw what covers the screen box given (the GTE's
    /// pixels); the water walked and not drawn goes in first if it meets that box.
    /// False once no water is left.</summary>
    public delegate bool WaterDraw(float cut, float x0, float y0, float x1, float y1);

    public static WaterDraw? WaterDrawer;

    /// <summary>0085. Water slices drawn, calls that found nothing in their slice,
    /// water triangles submitted, and water triangles noted for the planar plane.</summary>
    public static long MainWaterSlices, MainWaterEmpty, MainWaterTriangles, MainWaterNoted;
    /// <summary>0085. Calls whose water waited, sharing no pixel with the packet next.</summary>
    public static long MainWaterDeferred;
    /// <summary>0085. Runs the blended draws were cut into: one stream's faces, drawn
    /// before anything of another stream comes first.</summary>
    public static long MainWaterRuns;
    /// <summary>0085. Blended triangles sorted far to near for the main view's draws.</summary>
    public static long MainWaterSorted, MainWaterSortTicks;

    /// <summary>0085. Stopwatch ticks in the draw's parts: shadows and the static
    /// upload, the mip entries, the uniforms and the cull, the draw and after.</summary>
    public static readonly long[] MainTicks = new long[4];

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

    /// <summary>The frame's authored lights and glows drawn into the reflections, and
    /// its textures filtered through the mip atlas as the frame's are. Off, each is
    /// left out as it was before either was drawn (the comparisons).</summary>
    public static bool Lit = true, Mips = true;

    /// <summary>The last present's: lights sent to the world draws, whether a glow was
    /// on, and the retained textures with an atlas entry of those that could have one.</summary>
    public static int LitLights, MipsFound, MipsKeys;

    /// <summary>0085. Uploads of the static map's table of atlas entries, one word a texture.</summary>
    public static long MipTableUploads;

    /// <summary>0085. Stopwatch ticks in the atlas lookups and in the decode they queued.</summary>
    public static readonly long[] MipTicks = new long[2];
    public static bool LitGlow;

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

    /// <summary>0085. Each chunk's bounds over its blended faces alone (min above max
    /// for none).</summary>
    public static readonly float[] ChunkBlendMin = new float[Chunks * 3], ChunkBlendMax = new float[Chunks * 3];
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
        Array.Clear(_chunkRecords);
        for (int c = 0; c < Chunks; c++)
        {
            ChunkFogQ[c] = float.MaxValue;
            ChunkMin[c * 3] = ChunkMin[c * 3 + 1] = ChunkMin[c * 3 + 2] = float.MaxValue;
            ChunkMax[c * 3] = ChunkMax[c * 3 + 1] = ChunkMax[c * 3 + 2] = float.MinValue;
            ChunkBlendMin[c * 3] = ChunkBlendMin[c * 3 + 1] = ChunkBlendMin[c * 3 + 2] = float.MaxValue;
            ChunkBlendMax[c * 3] = ChunkBlendMax[c * 3 + 1] = ChunkBlendMax[c * 3 + 2] = float.MinValue;
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
                if ((v.Light & LightRecord) != 0)
                {
                    uint l = v.Light;
                    _chunkRecords[c] |= 1ul << (int)(l & 63u);
                    if ((l & 1u << 26) != 0) _chunkRecords[c] |= 1ul << (int)(l >> 8 & 63u);
                    if ((l & 1u << 27) != 0) _chunkRecords[c] |= 1ul << (int)(l >> 14 & 63u);
                    if ((l & 1u << 28) != 0) _chunkRecords[c] |= 1ul << (int)(l >> 20 & 63u);
                }
                else
                {
                    float q = BlackQuotient(v);
                    if (q < ChunkFogQ[c]) { ChunkFogQ[c] = q; ChunkFogOf[c] = v; }
                }
                if ((v.Flags & FlagSemi) != 0)
                {
                    ChunkBlendMin[c * 3] = Math.Min(ChunkBlendMin[c * 3], v.X); ChunkBlendMax[c * 3] = Math.Max(ChunkBlendMax[c * 3], v.X);
                    ChunkBlendMin[c * 3 + 1] = Math.Min(ChunkBlendMin[c * 3 + 1], v.Y); ChunkBlendMax[c * 3 + 1] = Math.Max(ChunkBlendMax[c * 3 + 1], v.Y);
                    ChunkBlendMin[c * 3 + 2] = Math.Min(ChunkBlendMin[c * 3 + 2], v.Z); ChunkBlendMax[c * 3 + 2] = Math.Max(ChunkBlendMax[c * 3 + 2], v.Z);
                }
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
        Array.Copy(ChunkFogQ, _chunkPlainQ, Chunks);
        Array.Copy(ChunkFogOf, _chunkPlainOf, Chunks);
        RecordFog();
        StaticGeneration++;
    }

    /// <summary>The quotient H*65536/z at and below which a corner's depth cue
    /// leaves nothing (the curves WorldVs fogs with), or 0 when it never does.</summary>
    public static float BlackQuotient(in Vertex v)
    {
        int curve = (int)(v.Curve + 0.5f);
        if (curve == 0 || curve == LinearDepthCue.Curve || v.Dqa >= 0f) return 0f;
        float ir0 = curve == 1 ? 2848f : 3232f;
        return Math.Max(0f, (ir0 * 4096f - v.Dqb) / v.Dqa);
    }

    /// <summary>How much of a corner's colour survives the cue at view depth z.</summary>
    public static float FogKeep(in Vertex v, float h, float z)
    {
        int curve = (int)(v.Curve + 0.5f);
        if (curve == 0) return 1f;
        if (curve == LinearDepthCue.Curve)
            return Math.Clamp(1 - LinearDepthCue.Weight(z, v.Dqa, v.Dqb) / 4096, 0, 1);
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
        /// <summary>Each map half's weight in the reflections, by
        /// <c>(tile Z * 80 + tile X) * 2 + upper</c>: 255 for one the frame's own
        /// tile walk drew, unless the port weighs them (<see cref="CurrentHalves"/>).</summary>
        public readonly byte[] Halves = new byte[HalvesW * HalvesH];
        /// <summary>0085. The halves the frame's own walk drew, 255 each: the main
        /// view's gate, which nothing grows or fades. 0089: the port may add halves
        /// past the walk's reach (<see cref="CurrentMainHalves"/>).</summary>
        public readonly byte[] MainHalves = new byte[HalvesW * HalvesH];
        /// <summary>0089. The main view's <see cref="DistanceFade"/>: its edge and band
        /// in world units from the camera's X and Z, and its depth limit; all 0 is none.</summary>
        public float FadeEdge, FadeBand, FadeDepth;
        /// <summary>0085. The swell this frame was walked with: per wave, its
        /// wavenumber along X and Z, its height and its phase; and whether it is on.</summary>
        public readonly float[] Swell = new float[12];
        public bool SwellOn;
        /// <summary>0085. The opaque faces of the models the frame's object walk took
        /// off the packets, in world space, in runs lit by one BK and LCM.</summary>
        public readonly ModelRuns Models = new();
        /// <summary>0085. The planar walk's mirrored camera, when the mirror is drawn by
        /// the backend this frame (<see cref="MirrorSerial"/>): the halves its walk
        /// visited, 255 each, and the models its replay took off the packets.</summary>
        public View MirrorView;
        public bool MirrorOn;
        public readonly byte[] MirrorHalves = new byte[HalvesW * HalvesH];
        public readonly ModelRuns MirrorModels = new();
        /// <summary>0085. The models drawn from cached meshes, in the main view and the
        /// mirror, and the posed vertices both read (four shorts a vertex).</summary>
        public readonly List<ModelInstance> Instances = new(), MirrorInstances = new();
        public short[] Verts = new short[4096];
        public int VertCount;
        /// <summary>0085. The blended faces of the main view's instances (a model's
        /// translucent faces, an effect, a billboard), each with the key the game would
        /// link it into its table at; the backend sorts them and draws them where the
        /// walk reaches them, among the map's water.</summary>
        public readonly List<BlendFace> BlendFaces = new();
        /// <summary>0085. The same for the mirror's instances, keyed as the mirrored
        /// table would link them.</summary>
        public readonly List<BlendFace> MirrorBlendFaces = new();
        /// <summary>0085. The first-person arm, when it is drawn from its mesh this
        /// frame: at <see cref="ArmSlot"/> of the table's walk, in painter's order.</summary>
        public ModelInstance Arm;
        public bool HasArm;
        /// <summary>Its faces' corners in the store, in the order the walk takes its
        /// packets: far to near by the key the game links each at; and the runs of one
        /// key, as the key and the first corner, which the walk draws at the key's slot.</summary>
        public int[] ArmOrder = new int[1024];
        public int ArmOrderCount;
        public int[] ArmRunKey = new int[64], ArmRunAt = new int[64];
        public int ArmRuns, ArmNext;
        /// <summary>0085. The sky: the objects of kind 0xF0, drawn from their meshes
        /// before the map, in painter's order (<see cref="AddSky"/>).</summary>
        public readonly List<ModelInstance> Sky = new();
        public readonly List<SkyFace> SkyFaces = new();

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
        f.SwellOn = false;
        f.Models.Clear();
        f.MirrorModels.Clear();
        f.Instances.Clear();
        f.MirrorInstances.Clear();
        f.BlendFaces.Clear();
        f.MirrorBlendFaces.Clear();
        f.VertCount = 0;
        f.HasArm = false;
        f.Sky.Clear();
        f.SkyFaces.Clear();
        ArmSerial = 0;
        f.MirrorOn = false;
        MirrorSerial = 0;
        Array.Clear(f.Halves);
        Array.Clear(f.MainHalves);
        f.FadeEdge = f.FadeBand = f.FadeDepth = 0f;
    }

    // ---- 0085: the mirror ----------------------------------------------------------

    /// <summary>0085. The frame whose mirror the next planar capture draws, set by the
    /// port when its mirrored walk begins; 0 once drawn.</summary>
    public static int MirrorSerial;

    /// <summary>0085. The current frame's mirror, drawn from <paramref name="view"/>: the
    /// halves and models the port adds with <c>mirror</c> set belong to it.</summary>
    public static void BeginMirror(in View view)
    {
        var f = Current;
        if (f.Serial != _serial) return;
        f.MirrorView = view;
        f.MirrorOn = true;
        f.MirrorModels.Clear();
        // The main view's instances placed in the world are the mirror's too: the
        // camera is the only thing that differs.
        f.MirrorInstances.Clear();
        f.MirrorBlendFaces.Clear();
        foreach (var m in f.Instances) if (m.Mirrored) f.MirrorInstances.Add(m);
        Array.Clear(f.MirrorHalves);
        MirrorSerial = _serial;
    }

    /// <summary>0085. A map half the mirrored walk left to the backend.</summary>
    public static void NoteMirrorHalf(int tx, int tz, int upper)
    {
        var f = Current;
        if (f.Serial != _serial || !f.MirrorOn || (uint)tx >= 80u || (uint)tz >= 80u) return;
        f.MirrorHalves[(tz * 80 + tx) * 2 + upper] = 255;
    }

    /// <summary>0085. Off, the mirror's map and models are taken off the packets and not
    /// drawn: the probe's way to see what they cover.</summary>
    public static bool MirrorShown = true;

    /// <summary>0085. Mirror draws made, captures that found no planar texture or no
    /// frame, and the static and model triangles drawn.</summary>
    public static long MirrorDraws, MirrorMissed, MirrorTriangles, MirrorModelTriangles, MirrorWaterTriangles;

    /// <summary>0085. The port's switch: the mirror's blended map faces are drawn by the
    /// backend too, after its opaque ones.</summary>
    public static bool MirrorWater;

    /// <summary>A map half the current frame's walk drew, at full weight; with
    /// <paramref name="main"/> false, one the game's packets draw in the main view
    /// (0085), so it is reflected but left out of the main view's gate.</summary>
    public static void NoteHalf(int tx, int tz, int upper, bool main = true)
    {
        var f = Current;
        if (f.Serial != _serial || (uint)tx >= 80u || (uint)tz >= 80u) return;
        f.Halves[(tz * 80 + tx) * 2 + upper] = 255;
        if (main) f.MainHalves[(tz * 80 + tx) * 2 + upper] = 255;
    }

    /// <summary>0085. The swell the current frame's water moves by (see <see cref="Frame.Swell"/>).</summary>
    public static void SetSwell(ReadOnlySpan<float> waves)
    {
        var f = Current;
        if (f.Serial != _serial) return;
        waves[..Math.Min(waves.Length, 12)].CopyTo(f.Swell);
        f.SwellOn = true;
    }

    /// <summary>The current frame's halves, for a port that weighs them itself: 0
    /// is not reflected, 255 fully, and between is dithered (the world program's
    /// <c>vFade</c>). Empty outside a frame.</summary>
    public static Span<byte> CurrentHalves => Current.Serial == _serial ? Current.Halves : Span<byte>.Empty;

    /// <summary>0089. The current frame's main-view gate, for a port that draws halves
    /// past the walk's reach: 255 is drawn, and a half written here is neither
    /// reflected nor noted for the mirror. Empty outside a frame.</summary>
    public static Span<byte> CurrentMainHalves => Current.Serial == _serial ? Current.MainHalves : Span<byte>.Empty;

    /// <summary>0089. The current frame's <see cref="DistanceFade"/>: <paramref name="edge"/>
    /// and <paramref name="band"/> in world units of horizontal distance from the camera
    /// (band 0 none), <paramref name="depth"/> the view depth everything is gone by (0
    /// none).</summary>
    public static void SetFade(float edge, float band, float depth)
    {
        var f = Current;
        if (f.Serial != _serial) return;
        f.FadeEdge = edge; f.FadeBand = band; f.FadeDepth = depth;
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

    /// <summary>0085. A run of <see cref="Frame.Models"/> lit with one back colour and
    /// light colour matrix, as the GTE held them (BK, then LCM row by row).</summary>
    public struct ModelGroup
    {
        public int Start, Count;
        /// <summary>Faces the port could not cull as the game does (those its clipper
        /// took), left to the GPU's facing cull.</summary>
        public bool Cull;
        public float Bk0, Bk1, Bk2, L0, L1, L2, L3, L4, L5, L6, L7, L8;
    }

    /// <summary>0085. A view's models: triangles in world space, in runs of one BK and LCM.</summary>
    public sealed class ModelRuns
    {
        public Vertex[] Tris = new Vertex[1024];
        public int Count;
        public readonly List<ModelGroup> Groups = new();

        public void Clear() { Count = 0; Groups.Clear(); }

        public void Add(ReadOnlySpan<Vertex> tris, ReadOnlySpan<float> light, bool cull)
        {
            int n = tris.Length / 3 * 3;
            if (n == 0) return;
            if (Count + n > Tris.Length) Array.Resize(ref Tris, Math.Max(Tris.Length * 2, Count + n));
            tris[..n].CopyTo(Tris.AsSpan(Count));
            var g = Groups.Count > 0 ? Groups[^1] : default;
            bool same = Groups.Count > 0 && g.Start + g.Count == Count && g.Cull == cull
                        && g.Bk0 == light[0] && g.Bk1 == light[1] && g.Bk2 == light[2]
                        && g.L0 == light[3] && g.L1 == light[4] && g.L2 == light[5] && g.L3 == light[6]
                        && g.L4 == light[7] && g.L5 == light[8] && g.L6 == light[9] && g.L7 == light[10] && g.L8 == light[11];
            if (same) { g.Count += n; Groups[^1] = g; }
            else Groups.Add(new ModelGroup
            {
                Start = Count, Count = n, Cull = cull, Bk0 = light[0], Bk1 = light[1], Bk2 = light[2],
                L0 = light[3], L1 = light[4], L2 = light[5], L3 = light[6], L4 = light[7], L5 = light[8],
                L6 = light[9], L7 = light[10], L8 = light[11],
            });
            Count += n;
        }
    }

    /// <summary>0085. One model's opaque triangles to the current frame's main view,
    /// or its mirror, with the BK and LCM its light dots are lit by
    /// (<paramref name="light"/>, 12).</summary>
    public static void AddMainModel(ReadOnlySpan<Vertex> tris, ReadOnlySpan<float> light, bool cull = false, bool mirror = false)
    {
        var f = Current;
        if (f.Serial != _serial || mirror && !f.MirrorOn) return;
        (mirror ? f.MirrorModels : f.Models).Add(tris, light, cull);
    }

    /// <summary>0085. Models' triangles drawn in the main view, draws, and the models'
    /// triangles the normal pass drew.</summary>
    public static long MainModelTriangles, MainModelGroups, MainModelNormalTriangles;

    // ---- 0085: the models' meshes, kept on the GPU ---------------------------------

    /// <summary>0085. Every cached model mesh's opaque faces, in the model's own space,
    /// as corners (two triangles a quad). A corner reuses <see cref="Vertex"/>: X is its
    /// vertex's index in the model's vertex array, R G B its normal (the GTE's 4096
    /// scale), Dqa Dqb Curve the indices of its face's first three vertices and Rgbc
    /// the fourth's (<see cref="uint.MaxValue"/> for a triangle); the rest as a
    /// packet has it, with <see cref="FlagDots"/> set. Appended to until
    /// <see cref="ClearMeshes"/>; the backend uploads what it has not.</summary>
    public static Vertex[] MeshCorners = new Vertex[16384];
    public static int MeshCornerCount;

    /// <summary>Bumped when the store is emptied, so the backend uploads it again.</summary>
    public static int MeshGeneration { get; private set; }

    /// <summary>A mesh's corners to the store; the index of its first.</summary>
    public static int AddMesh(ReadOnlySpan<Vertex> corners)
    {
        if (MeshCornerCount + corners.Length > MeshCorners.Length)
            Array.Resize(ref MeshCorners, Math.Max(MeshCorners.Length * 2, MeshCornerCount + corners.Length));
        int at = MeshCornerCount;
        corners.CopyTo(MeshCorners.AsSpan(at));
        MeshCornerCount += corners.Length;
        return at;
    }

    public static void ClearMeshes()
    {
        MeshCornerCount = 0;
        PoseTexels = 0;
        MeshGeneration++;
    }

    /// <summary>0085. The models' vertices kept on the GPU, four shorts a texel (x, y, z
    /// and a pad, as the game keeps them): a rigid model's vertices, one texel each; an
    /// MO pose's keyframe and its delta to the segment's target, two texels a vertex,
    /// which <c>ModelGlsl</c> blends by an instance's weight as the game's decoder
    /// does. Appended to until <see cref="ClearMeshes"/>; the backend uploads what it
    /// has not.</summary>
    public static short[] PoseStore = new short[65536];
    public static int PoseTexels;

    /// <summary>Texels to the pose store; the index of the first.</summary>
    public static int AddPose(ReadOnlySpan<short> texels)
    {
        int n = texels.Length / 4;
        if ((PoseTexels + n) * 4 > PoseStore.Length)
            Array.Resize(ref PoseStore, Math.Max(PoseStore.Length * 2, (PoseTexels + n) * 4));
        texels[..(n * 4)].CopyTo(PoseStore.AsSpan(PoseTexels * 4));
        int at = PoseTexels;
        PoseTexels += n;
        return at;
    }

    /// <summary>0085. One model drawn from a cached mesh: its corners in
    /// <see cref="MeshCorners"/>, its posed vertices in the frame's
    /// <see cref="Frame.Verts"/>, placed in the world by a rotation and a translation,
    /// and lit and fogged as its submit set the GTE up. <see cref="Far"/> is the mean
    /// table depth at and past which the game drops a face (8192 less the bias).</summary>
    public struct ModelInstance
    {
        public int MeshStart, MeshCount, VertBase;
        /// <summary>The mesh's corners in all, opaque then blended: the textures its
        /// draws need looked up.</summary>
        public int MeshAll;
        public float R00, R01, R02, R10, R11, R12, R20, R21, R22, Tx, Ty, Tz;
        /// <summary>The light matrix, divided by 4096; the back colour; the light colour
        /// matrix row by row.</summary>
        public float Llm0, Llm1, Llm2, Llm3, Llm4, Llm5, Llm6, Llm7, Llm8;
        public float Bk0, Bk1, Bk2, L0, L1, L2, L3, L4, L5, L6, L7, L8;
        public float Dqa, Dqb, Curve, Far;
        /// <summary>The mean table depth below which the game drops a face: a negative
        /// bias wraps the table's unsigned test, so it has a near end too.</summary>
        public float Near;
        public uint Rgbc, Material;
        /// <summary>Where the vertices come from: 0, the frame's <see cref="Frame.Verts"/>
        /// at <see cref="VertBase"/>; otherwise one past the first texel in
        /// <see cref="PoseStore"/>, a rigid model's vertices, or with
        /// <see cref="PoseMorph"/> an MO keyframe and its deltas, blended by
        /// <see cref="PoseWeight"/> (12.12).</summary>
        public int Pose, PoseWeight;
        public bool PoseMorph;
        /// <summary>Drawn in the mirror too, when the frame has one.</summary>
        public bool Mirrored;
        /// <summary>A blended model that stands for something solid (a door): its blended
        /// faces hide what is behind them from the occlusion pass with every texel.</summary>
        public bool Solid;
        /// <summary>The store's generation when it was added; the backend draws none
        /// from an emptied store.</summary>
        public int MeshGen;
        /// <summary>Placed in view space as the GTE places it: its rotation (4.12, row
        /// by row) and translation, integers. The world placement above is kept for
        /// what lights it; the eye's position is taken from these.</summary>
        public bool ViewSpace;
        public int V00, V01, V02, V10, V11, V12, V20, V21, V22, Vtx, Vty, Vtz;
        /// <summary>Assembled as <c>func_8002F918</c> assembles the sky: every face kept by
        /// its facing alone, on whole pixels, at no depth; lit per corner with no depth
        /// cue; an untextured face's colour its own (the corner's <see cref="Vertex.Light"/>
        /// with <see cref="FaceColour"/>).</summary>
        public bool Sky;
        /// <summary>Assembled as <c>func_80030540</c> assembles an object near the camera:
        /// no depth range, a face its near transform refuses left to the GPU's near clip
        /// and facing, and no corner placed at the GTE's saturated projection.</summary>
        public bool Tile;
        /// <summary>Its blended faces drawn by the backend (<see cref="BlendFace"/>) reach the
        /// surface buffer too: a solid one's as the opaque surface it stands for, one with a
        /// material or on the water's texture with that material, as their packets did.</summary>
        public bool BlendSurfaces;
        /// <summary>The forced-blend twin's rate plus one, every face blended at it; 0 for
        /// the faces' own.</summary>
        public int TwinMode;
        /// <summary>0098. How far this model alone is faded out, 0 (drawn in full) to 1
        /// (gone), through the same ordered dither as <see cref="DistanceFade"/> and on top
        /// of it, in the colour pass and the normal pass alike.</summary>
        public float FadeOut;
    }

    /// <summary>0085. A mesh corner's <see cref="Vertex.Light"/>: the low 24 bits are its
    /// face's own colour, which the light scales in place of the instance's RGBC.</summary>
    public const uint FaceColour = 0x40000000u;

    /// <summary>0085. The current frame's posed vertices (x, y, z and a pad, as the
    /// game keeps them), the index of the first.</summary>
    public static int AddModelVertices(ReadOnlySpan<short> verts)
    {
        var f = Current;
        if (f.Serial != _serial) return -1;
        int n = verts.Length / 4 * 4;
        if (f.VertCount * 4 + n > f.Verts.Length) Array.Resize(ref f.Verts, Math.Max(f.Verts.Length * 2, f.VertCount * 4 + n));
        verts[..n].CopyTo(f.Verts.AsSpan(f.VertCount * 4));
        int at = f.VertCount;
        f.VertCount += n / 4;
        return at;
    }

    /// <summary>0085. One blended face of a main-view instance: its corners in the store
    /// (three, or six for a quad), the table slot the game links it at (the mean of its
    /// corners' SZ over four, plus the slot bias), its blend mode and the order it was
    /// built in (the last built goes first within a slot), and the screen box its
    /// instance covers, in the GTE's pixels.</summary>
    public struct BlendFace
    {
        public int Inst, Corner, Corners, Key, Mode, Seq;
        public float X0, Y0, X1, Y1;
    }

    /// <summary>0085. The port's switch: a model's blended faces are drawn by the
    /// backend, where the table's walk would have drawn their packets.</summary>
    public static bool MainBlend = true;

    /// <summary>0085. Off, the blended faces are taken off the packets and not drawn: the
    /// probe's way to see what they cover.</summary>
    public static bool BlendShown = true;

    /// <summary>0085. The probe's: draw only this instance's blended faces (-1 all).</summary>
    public static int BlendOnly = -1;

    /// <summary>0085. The probe's: off, the blended faces skip the depth test.</summary>
    public static bool BlendDepth = true;

    /// <summary>0085. Blended faces noted, sorted for a draw, and drawn.</summary>
    public static long BlendNoted, BlendSorted, BlendDrawn, BlendRuns;

    /// <summary>0085. A blended face of the last instance added to the main view, or to
    /// the mirror.</summary>
    public static void AddBlendFace(int key, int corner, int corners, int mode, float x0, float y0, float x1, float y1,
                                    bool mirror = false)
    {
        var f = Current;
        var list = mirror ? f.MirrorInstances : f.Instances;
        if (f.Serial != _serial || list.Count == 0 || mirror && !f.MirrorOn) return;
        var faces = mirror ? f.MirrorBlendFaces : f.BlendFaces;
        faces.Add(new BlendFace
        {
            Inst = list.Count - 1, Corner = corner, Corners = corners, Key = key, Mode = mode,
            Seq = faces.Count, X0 = x0, Y0 = y0, X1 = x1, Y1 = y1,
        });
        if (mirror) MirrorBlendNoted++;
        else BlendNoted++;
    }

    /// <summary>0085. Instances whose blended faces went into the surface buffer, over
    /// every slice of the normal pass; never reset.</summary>
    public static long BlendNormalInstances;

    /// <summary>0085. The mirror's blended faces noted, and drawn.</summary>
    public static long MirrorBlendNoted, MirrorBlendDrawn;

    /// <summary>0085. A model instance to the current frame's main view, or its mirror.</summary>
    public static void AddInstance(in ModelInstance m, bool mirror = false)
    {
        var f = Current;
        if (f.Serial != _serial || mirror && !f.MirrorOn) return;
        var copy = m;
        copy.MeshGen = MeshGeneration;
        (mirror ? f.MirrorInstances : f.Instances).Add(copy);
    }

    /// <summary>0085. One face of a sky object: its corners in the store (three, or six
    /// for a quad), the table slot the game links it at, its blend mode (-1 opaque) and
    /// the order it was linked in, across the frame's sky.</summary>
    public struct SkyFace
    {
        public int Inst, Corner, Corners, Key, Mode, Seq;
    }

    /// <summary>0085. The objects of kind 0xF0 (<c>func_80032AC4</c>): the sky, centred
    /// on the eye, linked at the far end of the table with no depth record, so drawn in
    /// painter's order before everything else. <paramref name="faces"/> is the instance's
    /// faces in the order the game links them, each its corner, corner count, slot and
    /// blend mode; the backend draws them with the map, before it, far slot first and the
    /// last linked first within a slot.</summary>
    public static void AddSky(in ModelInstance m, ReadOnlySpan<(int Corner, int Corners, int Key, int Mode)> faces)
    {
        var f = Current;
        if (f.Serial != _serial || faces.Length == 0) return;
        var copy = m;
        copy.MeshGen = MeshGeneration;
        f.Sky.Add(copy);
        foreach (var (corner, corners, key, mode) in faces)
            f.SkyFaces.Add(new SkyFace { Inst = f.Sky.Count - 1, Corner = corner, Corners = corners, Key = key, Mode = mode, Seq = f.SkyFaces.Count });
    }

    /// <summary>0085. Off, the sky is taken off the packets and not drawn: the probe's
    /// way to see what it covers.</summary>
    public static bool SkyShown = true;

    /// <summary>0085. Sky objects drawn, their faces, and draws that could not.</summary>
    public static long SkyDrawn, SkyFacesDrawn, SkyMissed;

    /// <summary>0085. The first-person arm (<c>func_80032400</c>), drawn from its mesh:
    /// the game draws it in view space with no depth record, so it keeps painter's
    /// order, in front of whatever the table walked before it and behind whatever
    /// after, face by face. <paramref name="order"/> is its faces' corners in the walk's
    /// order, in runs of one key (<paramref name="runKey"/>, starting at
    /// <paramref name="runAt"/>); the backend draws each run when the walk reaches the
    /// key's slot (0x1FFF less it, counted from the far end as <c>GteDepth.OtSlot</c>
    /// is), with no depth test, and leaves the far plane where it drew. <paramref name="box"/>
    /// is the screen box it covers, in the GTE's pixels, as the packets' are.</summary>
    public static void SetArm(in ModelInstance m, ReadOnlySpan<int> order, ReadOnlySpan<int> runKey, ReadOnlySpan<int> runAt,
                              ReadOnlySpan<float> box)
    {
        ArmX0 = box[0]; ArmY0 = box[1]; ArmX1 = box[2]; ArmY1 = box[3];
        var f = Current;
        if (f.Serial != _serial || runKey.Length == 0) return;
        if (f.ArmOrder.Length < order.Length) f.ArmOrder = new int[order.Length];
        order.CopyTo(f.ArmOrder);
        f.ArmOrderCount = order.Length;
        if (f.ArmRunKey.Length < runKey.Length) { f.ArmRunKey = new int[runKey.Length]; f.ArmRunAt = new int[runKey.Length]; }
        runKey.CopyTo(f.ArmRunKey);
        runAt.CopyTo(f.ArmRunAt);
        f.ArmRuns = runKey.Length;
        f.ArmNext = 0;
        f.Arm = m;
        f.Arm.MeshGen = MeshGeneration;
        f.HasArm = true;
        ArmSerial = _serial;
        ArmSlot = 0x1FFF - runKey[0];
    }

    /// <summary>0085. The frame whose arm the next table walk draws, and the slot of its
    /// next run; 0 once drawn.</summary>
    public static int ArmSerial, ArmSlot;

    /// <summary>0085. The slot the walk has reached, for the backend's arm draw: every
    /// run at or before it is drawn.</summary>
    public static int ArmCut;

    /// <summary>0085. The screen box the arm covers; a packet outside it shares no pixel
    /// with the arm, so the walk need not stop to draw the arm before it.</summary>
    public static float ArmX0, ArmY0, ArmX1, ArmY1;

    public static bool ArmMeets(float x0, float y0, float x1, float y1) =>
        x0 <= ArmX1 && x1 >= ArmX0 && y0 <= ArmY1 && y1 >= ArmY0;

    /// <summary>0085. The backend's arm draw, given the GPU's draw offset: the runs the
    /// walk has reached. True while runs are left.</summary>
    public static Func<int, int, bool>? ArmDrawer;

    /// <summary>0085. Arms drawn, the draw calls they took, and walks that could not
    /// draw the arm they had.</summary>
    public static long ArmDraws, ArmCalls, ArmMissed;

    /// <summary>0085. Instances drawn in the main view and the mirror, their corners,
    /// and the vertices uploaded; never reset.</summary>
    public static long InstancesDrawn, InstanceCorners, MirrorInstancesDrawn, InstanceVertices, PoseTexelsUploaded;

    /// <summary>0085. Off, the models taken off the packets are not drawn either: the
    /// probe's way to see what they cover.</summary>
    public static bool MainModelsShown = true;

    /// <summary>0086. The main view's models mark the target's stencil, and a tested
    /// packet the walk sends after them draws over those pixels against its true depth
    /// instead of 0051's tolerance: they were drawn at slot 1, ahead of every packet,
    /// so the tolerance gave a model's pixels to whatever stood just behind it. Off
    /// unless the game turns it on.</summary>
    public static bool ModelMask;

    /// <summary>0086. Counts what the tolerance would let through over the models'
    /// pixels, with two occlusion queries per tested batch drawn after them (a stall
    /// each); off by default. The counts below are cumulative.</summary>
    public static bool ModelMaskProbe;

    /// <summary>0086. Main views that marked models; tested batches drawn over the
    /// mark; samples of those that pass the tolerant test over model pixels, and of
    /// those, the ones behind the model's true depth, which the mask holds back.</summary>
    public static long MaskFrames, MaskBatches, MaskSamples, MaskBehind;

    /// <summary>0096. How many table slots the walk passes after the slot the main view
    /// is drawn at (1) that the game linked to be drawn before its world: a second table
    /// spliced in there, such as Verdite3's eight-entry front table. A packet in them
    /// with no recovered depth is drawn only where the main view left the far plane
    /// (<see cref="UnderWorld"/>), as painter's order put it behind everything. 0, the
    /// default, is the walk as it was.</summary>
    public static int UnderSlots;

    /// <summary>0103. Draw a retained model's face that has a corner the GTE cannot
    /// project (nearer than H/2, or off the divide's range) in the ordinary projection,
    /// clipped at the GPU's near plane and kept by its plane against the eye, as a
    /// <c>Tile</c> instance's is. Placed at the divide's saturated ends instead, a large
    /// flat face reaching past the eye drew as a long sheared slab; the packets never
    /// drew it, the GPU refusing a triangle more than 1023 pixels across or 511 down.
    /// Not the sky nor a model placed in view space. False, the default, is the draw as
    /// it was.</summary>
    public static bool ModelNearClip;

    /// <summary>0096. Set by the table walk while it is in <see cref="UnderSlots"/> after
    /// a main view that drew; false outside a walk.</summary>
    public static bool UnderWorld;

    /// <summary>0096. Triangles drawn under the world (cumulative).</summary>
    public static long UnderTriangles;

    /// <summary>0096. Counts each batch drawn under the world twice more with occlusion
    /// queries (a stall each): its samples untested, and those that pass, which are the
    /// ones painter's order would have left showing. Off by default; cumulative.</summary>
    public static bool UnderProbe;
    public static long UnderSamples, UnderShown;

    /// <summary>0086, with the probe: samples of the blended ones in front of the model
    /// by less than the tolerance, which still draw (an opaque batch's own depth is
    /// already in the buffer when it is probed). Then, before each model list's
    /// depth, the models' samples against the map: those the tolerant test passes, and
    /// those it fails but a test pulled 0xF0 slots towards the camera
    /// passes -- a model just behind a map face, which the table, linking a tile
    /// 0xF0 slots deeper than its mean, drew over it.</summary>
    public static long MaskAhead, ModelSamples, ModelUnderMap;

    /// <summary>0086. The probe's slacks, in view-depth units, the last 0xF0 table
    /// slots; <see cref="ModelUnderSlack"/> counts, per slack, the models' samples it
    /// passes and the tolerance does not (<see cref="ModelUnderMap"/> is the last).</summary>
    public static readonly float[] MapSlacks = [8, 32, 128, 512, 0xF0 * 4];
    public static readonly long[] ModelUnderSlack = new long[5];

    /// <summary>The tolerance probe: before each colour pass of the main view's map and
    /// models, the samples the tolerant test passes, and of those the ones it passes
    /// only because the tolerance exceeded each of <see cref="ToleranceCaps"/> -- colour
    /// drawn over a nearer surface by more than that many view-depth units. Occlusion
    /// queries (a stall each); off by default; cumulative.</summary>
    public static bool ToleranceProbe;
    public static readonly float[] ToleranceCaps = [0.25f, 1, 4, 16, 64, 512];
    /// <summary>Per pass: 0 the map, 1 the frame's posed models, 2 the instances.</summary>
    public static readonly long[] ToleranceSamples = new long[3];
    /// <summary>Pass * <see cref="ToleranceCaps"/>.Length + cap.</summary>
    public static readonly long[] ToleranceBehind = new long[18];

    /// <summary>The main view's ceiling on 0051's slope term, in the game's pixels: a
    /// fragment of the map or a model is pulled towards the camera by at most the
    /// constant bias plus the world width of this many pixels at its own depth
    /// (z / H each). 0, the default, leaves the term unbounded.</summary>
    public static float DepthCapPixels;

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
