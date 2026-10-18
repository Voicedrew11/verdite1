using System.Runtime.InteropServices;
using RecompOne.Runtime;
using RecompOne.Runtime.Diagnostics;
using RecompOne.Runtime.Memory;

namespace Kf1;

/// <summary>
/// The area's map as one world-space mesh on the GPU (<see cref="RetainedScene.SetStatic"/>),
/// gated to the cells the game's own walk draws. Re-derived from this game's walk
/// (<see cref="MapCell"/>, <see cref="MapAssembler"/>):
///
/// - A cell (col, row) of the 100x100 grid at <c>0x8009A748</c> names map object
///   <c>byte - 1</c> of TMD 0 (<c>0xFF</c> empty; <c>0x44</c>-<c>0x46</c> remapped while
///   <c>s16[0x800A07D2]</c> is set with its low bits under 2), drawn only under 100; a
///   stencil entry of 1 draws object + 100 instead.
/// - Its origin is <c>(2000 col, -100 h, 2000 row)</c>, h from the grid at
///   <c>0x80095900</c>; the grid at <c>0x80069018</c> (1-4) picks one of four
///   orientations, which adds 2000 to z, to x and z, or to x, and supplies the
///   rotation (<c>0x80095760 + 32 k</c>) and the light matrix (<c>0x800957E0 + 32 k</c>).
/// - Only FT3 and FT4 primitives (modes <c>0x24</c>, <c>0x2C</c>) are drawn, as GT3/GT4:
///   each face lit once by NCCS from its normal (camera-independent), each corner
///   then depth-cued by DPCS at twice the vertex's IR0 toward a black far colour.
///   That is the shader's offset curve (1) with DQB raised by 800 x 4096: exactly
///   <c>keep = 1 - 2 IR0 / 4096</c>.
///
/// A cell's variant is a gate slot (<see cref="RetainedScene.HalfFlag"/>), so the GPU
/// draws exactly the cells the walk visited. What the mesh is built from is hashed
/// at each walk; a change rebuilds it, unless a map with the same hash and TMD is
/// kept (KF1_MAPCACHE, the fork's 0111), which is put back instead. Step 3 of
/// docs/RENDERER_PLAN.md; "Each area's map kept" in docs/GPU_RENDERER.md.
/// </summary>
public static class RetainedMap
{
    const uint Cells = 0x8009A748, Heights = 0x80095900, Turns = 0x80069018;
    const uint Orient = 0x80095760, Lights = 0x800957E0, Remap = 0x800A07D2;
    const uint Tmds = 0x80090FA8, Rgbc = 0x80057B60, Cache = 0x800911B0;
    const int Grid = 100, CellSize = 2000;

    // The frame profiler's sections for the map's CPU work (free while profiling is off).
    static readonly int ProfWalk = Profiler.Register("retained map: walk", ProfileGroup.Hook);
    static readonly int ProfBuild = Profiler.Register("retained map: mesh build", ProfileGroup.Hook);

    public static bool Ready { get; private set; }
    public static long Builds, Restores, Corners;

    /// <summary>Built maps kept to put back (KF1_MAPCACHE); 0 keeps none.</summary>
    public const int DefaultKeep = 5;

    // About an area's corners, areas 1-4 (area 5 has half).
    const int Reserve = 1_050_000;

    /// <summary>The cache's size, and the first build's arrays readied off the game
    /// thread: written for the first time, a million corners fault in 136 MB.</summary>
    public static void Configure(string? keep)
    {
        RetainedScene.StaticKeep = int.TryParse(keep, out int n) ? Math.Clamp(n, 0, 16) : DefaultKeep;
        if (GpuWorld.Mode == 0) return;
        RetainedScene.ReserveStatic(Reserve);
        ThreadPool.QueueUserWorkItem(_ =>
        {
            var a = new RetainedScene.Vertex[Reserve];
            for (int i = 0; i < a.Length; i += 64) a[i].Flags = 0;
            Interlocked.CompareExchange(ref _all, a, Array.Empty<RetainedScene.Vertex>());
        });
    }

    // What a kept map needs beside the scene's own copy.
    sealed record Kept(int[] Low, int[] High, int Slots, long Corners, uint Tmd, int Meshes, Source Src, bool Remap);
    public static int Slots;

    // Each cell's gate slot for its own object and for object + 100, -1 for none.
    static readonly int[] SlotLow = new int[Grid * Grid], SlotHigh = new int[Grid * Grid];

    /// <summary>A built cell's gate slot for its object (or object + 100); -1 for none.</summary>
    public static int SlotOf(int cell, bool high) =>
        !Ready || (uint)cell >= Grid * Grid ? -1 : (high ? SlotHigh : SlotLow)[cell];
    static ulong _built, _seen;
    static int _held;
    static long _seenTick;
    static bool _remap;

    struct Face
    {
        public int N, Normal;
        public uint Prim;
        public ushort V0, V1, V2, V3, Uv0, Uv1, Uv2, Uv3, Clut, Tpage;
    }

    sealed class Mesh
    {
        public Face[] Faces = [];
        public uint Vertices;
        public int Count;
    }

    static Mesh?[] _meshes = [];
    static uint _tmd;

    /// <summary>The source state of the last build, as the GTE held it at the walk.</summary>
    struct Source
    {
        public int Dqa, Dqb;
        public uint Rgbc, Fc;
        public int Bk0, Bk1, Bk2;
        public short L0, L1, L2, L3, L4, L5, L6, L7, L8;
    }

    static Source _src;

    static unsafe ulong Hash(PSMemory m, uint address, int bytes, ulong h)
    {
        var span = m.Ram.Slice((int)(address & 0x1FFFFF), bytes);
        var words = MemoryMarshal.Cast<byte, ulong>(span);
        foreach (ulong w in words) h = (h ^ w) * 0x100000001B3UL + 0x9E3779B97F4A7C15UL;
        for (int i = words.Length * 8; i < span.Length; i++) h = (h ^ span[i]) * 0x100000001B3UL;
        return h;
    }

    static Source ReadSource(PSMemory m)
    {
        var s = new Source
        {
            Dqa = (short)Gte.ReadControl(27), Dqb = (int)Gte.ReadControl(28),
            Rgbc = m.ReadU32(Rgbc) & 0xFFFFFF,
            Fc = (uint)((Gte.ReadControl(21) & 0xFFF) | (Gte.ReadControl(22) & 0xFFF) << 12) ^ (Gte.ReadControl(23) & 0xFF) << 24,
            Bk0 = (int)Gte.ReadControl(13), Bk1 = (int)Gte.ReadControl(14), Bk2 = (int)Gte.ReadControl(15),
        };
        uint l01 = Gte.ReadControl(16), l23 = Gte.ReadControl(17), l45 = Gte.ReadControl(18), l67 = Gte.ReadControl(19);
        s.L0 = (short)l01; s.L1 = (short)(l01 >> 16); s.L2 = (short)l23; s.L3 = (short)(l23 >> 16);
        s.L4 = (short)l45; s.L5 = (short)(l45 >> 16); s.L6 = (short)l67; s.L7 = (short)(l67 >> 16);
        s.L8 = (short)Gte.ReadControl(20);
        return s;
    }

    static bool FarBlack => Gte.ReadControl(21) == 0 && Gte.ReadControl(22) == 0 && Gte.ReadControl(23) == 0;

    /// <summary>At the head of the map walk: rebuild if anything the mesh is built
    /// from moved.</summary>
    public static void Walk(PSMemory m)
    {
        int p = Profiler.Begin(ProfWalk);
        try { WalkCore(m); }
        finally { Profiler.End(p); }
    }

    static void WalkCore(PSMemory m)
    {
        uint tmd = m.ReadU32(Tmds);
        if ((tmd & 0x1FFFFF) == 0 || (tmd >> 24) != 0x80) { Ready = false; GpuWorld.Fallback(0x8001E83C, 0, "map-tmd-absent"); return; }
        if (GpuWorld.CensusOn || GpuWorld.Check) DiffGrid(m);
        var src = ReadSource(m);
        short remap = (short)m.ReadU16(Remap);
        bool remapOn = remap != -1 && (remap & 3) < 2;
        ulong h = 0xCBF29CE484222325UL ^ tmd ^ (remapOn ? 1UL << 40 : 0);
        h = Hash(m, Cells, Grid * Grid, h);
        ulong hCells = h;
        h = Hash(m, Heights, Grid * Grid, h);
        h = Hash(m, Turns, Grid * Grid, h);
        h = Hash(m, Orient, 0x100, h);
        ulong hGrid = h;
        var srcBytes = MemoryMarshal.AsBytes(new ReadOnlySpan<Source>(in src));
        foreach (byte b in srcBytes) h = (h ^ b) * 0x100000001B3UL;
        if (Ready && h == _built) return;
        // A changing source (the fog fading at an area's exit, the death's dimming, a
        // step a tick) is left to the packets until it holds for ten walks and, under
        // pacing, two ticks: one build, not one a step.
        if (h != _seen) { _seen = h; _held = 0; _seenTick = FramePacing.Ticks; }
        if (++_held < 10 || FramePacing.Enabled && FramePacing.Ticks - _seenTick < 2)
        { Ready = false; GpuWorld.Fallback(0x8001E83C, 0, "map-source-changing"); return; }
        if (!FarBlack) { Ready = false; GpuWorld.Fallback(0x8001E83C, 0, "map-far-colour"); return; }
        int bp = Profiler.Begin(ProfBuild);
        long t0 = System.Diagnostics.Stopwatch.GetTimestamp();
        ulong key = 0;
        try
        {
            key = RetainedScene.StaticKeep > 0 ? (h ^ TmdHash(m, tmd)) * 0x100000001B3UL : 0;
            if (key != 0 && RetainedScene.RestoreStatic(key, out var tag) && tag is Kept k)
            {
                Restore(k);
                _built = h;
                if (GpuWorld.CensusOn)
                    Console.WriteLine($"[KF1] map restore {Restores}: frame {GpuWorld.Frames}, {Corners} corners, " +
                        $"{System.Diagnostics.Stopwatch.GetElapsedTime(t0).TotalMilliseconds:F1} ms; tmd {tmd:X8} cells {hCells:X16}");
                return;
            }
            Build(m, tmd, src, remapOn);
            if (key != 0 && Ready)
                RetainedScene.KeepStatic(key, new Kept((int[])SlotLow.Clone(), (int[])SlotHigh.Clone(), Slots, Corners,
                    tmd, _meshes.Length, src, remapOn));
        }
        finally { Profiler.End(bp); }
        if (GpuWorld.CensusOn)
            Console.WriteLine($"[KF1] map build {Builds}: frame {GpuWorld.Frames}, {Corners} corners, " +
                $"{System.Diagnostics.Stopwatch.GetElapsedTime(t0).TotalMilliseconds:F1} ms; tmd {tmd:X8} remap {remapOn} " +
                $"cells {hCells:X16} grid {hGrid:X16} dq {src.Dqa}/{src.Dqb} rgbc {src.Rgbc:X6} bk {src.Bk0}/{src.Bk1}/{src.Bk2} " +
                $"l {src.L0}/{src.L4}/{src.L8}");
        _built = h;
    }

    static void Restore(Kept k)
    {
        k.Low.CopyTo(SlotLow);
        k.High.CopyTo(SlotHigh);
        Slots = k.Slots;
        Corners = k.Corners;
        _tmd = k.Tmd;
        _meshes = new Mesh?[k.Meshes];
        _src = k.Src;
        _remap = k.Remap;
        Restores++;
        Ready = true;
    }

    /// <summary>The map TMD's bytes, header to its furthest vertex, normal or primitive:
    /// the areas load theirs at one address, so the pointer alone names none.</summary>
    static ulong TmdHash(PSMemory m, uint tmd)
    {
        uint baseAt = tmd + 0xC, end = baseAt;
        int objects = Math.Clamp((int)m.ReadU32(tmd + 8), 0, 1024);
        for (int o = 0; o < objects; o++)
        {
            uint header = baseAt + (uint)o * 28;
            end = Math.Max(end, header + 28);
            end = Math.Max(end, baseAt + m.ReadU32(header) + Math.Min(m.ReadU32(header + 4), 4096) * 8);
            end = Math.Max(end, baseAt + m.ReadU32(header + 8) + Math.Min(m.ReadU32(header + 12), 4096) * 8);
            uint at = baseAt + m.ReadU32(header + 16);
            uint n = Math.Min(m.ReadU32(header + 20), 8192);
            for (uint i = 0; i < n && (at & 0x1FFFFF) < 0x1FFFFC; i++) at += 4 + ((m.ReadU32(at) >> 6) & 0x3FC);
            end = Math.Max(end, at);
        }
        int bytes = (int)Math.Min(end - tmd, 0x200000 - (tmd & 0x1FFFFF));
        return Hash(m, tmd, Math.Max(bytes, 0), 0xCBF29CE484222325UL);
    }

    // ---- the grid's writes, measured: which cells change between walks ----------

    static readonly uint[] GridAt = [Cells, Heights, Turns];
    static readonly byte[] _grid = new byte[3 * Grid * Grid];
    static bool _gridSeen;
    public static long GridWalks, GridChangedWalks, GridChangedBytes;
    static int _gridLogs;

    /// <summary>Each walk: the three grid arrays against the last walk's copy. A change is
    /// counted and, for the first few, logged with its cell and frame.</summary>
    static void DiffGrid(PSMemory m)
    {
        GridWalks++;
        int changed = 0;
        for (int k = 0; k < GridAt.Length; k++)
        {
            var now = m.Ram.Slice((int)(GridAt[k] & 0x1FFFFF), Grid * Grid);
            var old = _grid.AsSpan(k * Grid * Grid, Grid * Grid);
            for (int i = 0; i < now.Length; i++)
            {
                if (now[i] == old[i]) continue;
                if (_gridSeen)
                {
                    changed++;
                    if (_gridLogs++ < 24)
                        Console.WriteLine($"[KF1] map grid: frame {GpuWorld.Frames} array {k} " +
                            $"cell {i % Grid},{i / Grid} {old[i]:X2} -> {now[i]:X2}");
                }
                old[i] = now[i];
            }
        }
        if (!_gridSeen) { _gridSeen = true; return; }
        if (changed == 0) return;
        GridChangedWalks++;
        GridChangedBytes += changed;
    }

    static Mesh? MeshOf(PSMemory m, int obj)
    {
        if ((uint)obj >= (uint)_meshes.Length) return null;
        if (_meshes[obj] is { } cached) return cached;
        uint header = _tmd + 0xC + (uint)obj * 28;
        uint baseAt = _tmd + 0xC;
        uint verts = baseAt + m.ReadU32(header), nverts = m.ReadU32(header + 4);
        uint prims = baseAt + m.ReadU32(header + 16), nprims = m.ReadU32(header + 20);
        if (nverts > 4096 || nprims > 8192) return null;
        var faces = new List<Face>((int)nprims);
        uint at = prims;
        for (uint i = 0; i < nprims; i++)
        {
            uint word = m.ReadU32(at), body = at + 4;
            uint mode = word >> 24;
            if (mode == 0x24)
                faces.Add(new Face
                {
                    N = 3, Prim = body, Uv0 = m.ReadU16(body), Clut = m.ReadU16(body + 2), Uv1 = m.ReadU16(body + 4),
                    Tpage = m.ReadU16(body + 6), Uv2 = m.ReadU16(body + 8), Normal = m.ReadU16(body + 0xC),
                    V0 = m.ReadU16(body + 0xE), V1 = m.ReadU16(body + 0x10), V2 = m.ReadU16(body + 0x12),
                });
            else if (mode == 0x2C)
                faces.Add(new Face
                {
                    N = 4, Prim = body, Uv0 = m.ReadU16(body), Clut = m.ReadU16(body + 2), Uv1 = m.ReadU16(body + 4),
                    Tpage = m.ReadU16(body + 6), Uv2 = m.ReadU16(body + 8), Uv3 = m.ReadU16(body + 0xC),
                    Normal = m.ReadU16(body + 0x10), V0 = m.ReadU16(body + 0x12), V1 = m.ReadU16(body + 0x14),
                    V2 = m.ReadU16(body + 0x16), V3 = m.ReadU16(body + 0x18),
                });
            at = body + ((word >> 6) & 0x3FC);
        }
        var mesh = new Mesh { Faces = faces.ToArray(), Vertices = verts, Count = (int)nverts };
        _meshes[obj] = mesh;
        return mesh;
    }

    /// <summary>NCCS as the GTE does it (sf 1, lm 1): the face's colour from its
    /// normal under the light matrix, the colour matrix and the back colour.</summary>
    static (int R, int G, int B) Nccs(in Source s, ReadOnlySpan<short> llm, int nx, int ny, int nz, uint rgbc)
    {
        static int Sat(long v) => (int)Math.Clamp(v, 0, 0x7FFF);
        int i1 = Sat(((long)llm[0] * nx + (long)llm[1] * ny + (long)llm[2] * nz) >> 12);
        int i2 = Sat(((long)llm[3] * nx + (long)llm[4] * ny + (long)llm[5] * nz) >> 12);
        int i3 = Sat(((long)llm[6] * nx + (long)llm[7] * ny + (long)llm[8] * nz) >> 12);
        int c1 = Sat((((long)s.Bk0 << 12) + (long)s.L0 * i1 + (long)s.L1 * i2 + (long)s.L2 * i3) >> 12);
        int c2 = Sat((((long)s.Bk1 << 12) + (long)s.L3 * i1 + (long)s.L4 * i2 + (long)s.L5 * i3) >> 12);
        int c3 = Sat((((long)s.Bk2 << 12) + (long)s.L6 * i1 + (long)s.L7 * i2 + (long)s.L8 * i3) >> 12);
        int r = (int)(rgbc & 0xFF), g = (int)(rgbc >> 8 & 0xFF), b = (int)(rgbc >> 16 & 0xFF);
        return (Math.Clamp(((r << 4) * c1 >> 12) >> 4, 0, 255), Math.Clamp(((g << 4) * c2 >> 12) >> 4, 0, 255),
                Math.Clamp(((b << 4) * c3 >> 12) >> 4, 0, 255));
    }

    static RetainedScene.Vertex[] _all = Array.Empty<RetainedScene.Vertex>();

    static void Build(PSMemory m, uint tmd, in Source src, bool remapOn)
    {
        long t0 = System.Diagnostics.Stopwatch.GetTimestamp();
        if (tmd != _tmd || Ready == false)
        {
            _tmd = tmd;
            _meshes = new Mesh?[Math.Clamp((int)m.ReadU32(tmd + 8), 0, 1024)];
        }
        _src = src;
        _remap = remapOn;
        Span<short> llm = stackalloc short[9 * 4];
        Span<short> rot = stackalloc short[9 * 4];
        for (int k = 0; k < 4; k++)
            for (int j = 0; j < 9; j++)
            {
                llm[k * 9 + j] = (short)m.ReadU16(Lights + (uint)k * 32 + (uint)j * 2);
                rot[k * 9 + j] = (short)m.ReadU16(Orient + (uint)k * 32 + (uint)j * 2);
            }
        Array.Fill(SlotLow, -1);
        Array.Fill(SlotHigh, -1);
        int slots = 0, n = 0;
        // Each turn's lit colours of a mesh's faces, worked out once a build.
        var lit = new Dictionary<(int, int), (int, int, int)[]>();
        for (int row = 0; row < Grid; row++)
            for (int col = 0; col < Grid; col++)
            {
                int cell = row * Grid + col;
                int obj = Object(m.ReadU8(Cells + (uint)cell), remapOn);
                if (obj < 0) continue;
                int turn = m.ReadU8(Turns + (uint)cell) - 1;
                if ((uint)turn > 3) { GpuWorld.Fallback(0x8001E5EC, 0, "map-turn-range"); continue; }
                for (int variant = 0; variant < 2; variant++)
                {
                    int o = obj + variant * 100;
                    if (o >= _meshes.Length || MeshOf(m, o) is not { } mesh) continue;
                    if (slots >= RetainedScene.HalvesW * RetainedScene.HalvesH) { GpuWorld.Fallback(0x8001E5EC, 0, "map-slots-full"); continue; }
                    int slot = slots++;
                    (variant == 0 ? SlotLow : SlotHigh)[cell] = slot;
                    if (!lit.TryGetValue((o, turn), out var colours))
                    {
                        colours = new (int, int, int)[mesh.Faces.Length];
                        uint normals = _tmd + 0xC + m.ReadU32(_tmd + 0xC + (uint)o * 28 + 8);
                        for (int f = 0; f < colours.Length; f++)
                        {
                            uint np = normals + (uint)mesh.Faces[f].Normal;
                            colours[f] = Nccs(src, llm.Slice(turn * 9, 9), (short)m.ReadU16(np), (short)m.ReadU16(np + 2),
                                (short)m.ReadU16(np + 4), src.Rgbc);
                        }
                        lit[(o, turn)] = colours;
                    }
                    int need = n + mesh.Faces.Length * 6;
                    if (_all.Length < need) Array.Resize(ref _all, Math.Max(need, _all.Length * 2));
                    n = Place(m, mesh, colours, rot.Slice(turn * 9, 9), col, row, turn, slot, src, n);
                }
            }
        long t1 = System.Diagnostics.Stopwatch.GetTimestamp();
        RetainedScene.SetStatic(_all.AsSpan(0, n));
        Slots = slots;
        Corners = n;
        Builds++;
        Ready = n > 0;
        if (GpuWorld.CensusOn)
            Console.WriteLine($"[KF1] retained map: built {slots} slots, {n} corners in " +
                $"{System.Diagnostics.Stopwatch.GetElapsedTime(t0).TotalMilliseconds:0.0} ms, " +
                $"{System.Diagnostics.Stopwatch.GetElapsedTime(t1).TotalMilliseconds:0.0} of them sorting " +
                $"(dqa {src.Dqa}, dqb {src.Dqb}, rgbc {src.Rgbc:X6})");
    }

    /// <summary>The object a cell's byte names, as <see cref="MapCell"/> takes it; -1 none.</summary>
    static int Object(byte b, bool remapOn)
    {
        if (b == 0xFF) return -1;
        int o = remapOn && b is 0x44 or 0x45 or 0x46 ? b - 0x44 + 0x16 : (b - 1) & 0xFF;
        return o < 100 ? o : -1;
    }

    /// <summary>A cell's world origin: the walk's offsets, the turn's step included.</summary>
    static (int X, int Y, int Z) Origin(PSMemory m, int col, int row, int turn)
    {
        int x = CellSize * col, z = CellSize * row, y = -100 * m.ReadU8(Heights + (uint)(row * Grid + col));
        if (turn == 1 || turn == 2) z += CellSize;
        if (turn == 2 || turn == 3) x += CellSize;
        return (x, y, z);
    }

    static (float X, float Y, float Z) World(PSMemory m, Mesh mesh, ReadOnlySpan<short> r, (int X, int Y, int Z) o, int offset)
    {
        uint p = mesh.Vertices + (uint)offset;
        float vx = (short)m.ReadU16(p), vy = (short)m.ReadU16(p + 2), vz = (short)m.ReadU16(p + 4);
        return (o.X + (r[0] * vx + r[1] * vy + r[2] * vz) / 4096f,
                o.Y + (r[3] * vx + r[4] * vy + r[5] * vz) / 4096f,
                o.Z + (r[6] * vx + r[7] * vy + r[8] * vz) / 4096f);
    }

    static int Place(PSMemory m, Mesh mesh, (int R, int G, int B)[] colours, ReadOnlySpan<short> rot,
        int col, int row, int turn, int slot, in Source src, int n)
    {
        var o = Origin(m, col, row, turn);
        uint half = (uint)(slot + 1) << RetainedScene.HalfShift;
        Span<ushort> vi = stackalloc ushort[4], uv = stackalloc ushort[4];
        ReadOnlySpan<int> quad = [0, 1, 2, 1, 3, 2], tri = [0, 1, 2];
        // Last face first, as the walk links them: the first face is drawn on top.
        for (int f = mesh.Faces.Length - 1; f >= 0; f--)
        {
            ref readonly var face = ref mesh.Faces[f];
            vi[0] = face.V0; vi[1] = face.V1; vi[2] = face.V2; vi[3] = face.V3;
            uv[0] = face.Uv0; uv[1] = face.Uv1; uv[2] = face.Uv2; uv[3] = face.Uv3;
            int u0 = 255, v0 = 255, u1 = 0, v1 = 0;
            for (int k = 0; k < face.N; k++)
            {
                int u = uv[k] & 255, v = uv[k] >> 8;
                u0 = Math.Min(u0, u); u1 = Math.Max(u1, u); v0 = Math.Min(v0, v); v1 = Math.Max(v1, v);
            }
            var c = colours[f];
            var order = face.N == 4 ? quad : tri;
            foreach (int k in order)
            {
                var w = World(m, mesh, rot, o, vi[k]);
                _all[n++] = new RetainedScene.Vertex
                {
                    X = w.X, Y = w.Y, Z = w.Z, R = c.R, G = c.G, B = c.B,
                    Clut = face.Clut, Texpage = face.Tpage, U = uv[k] & 255, V = uv[k] >> 8,
                    Dqa = src.Dqa, Dqb = src.Dqb + 800f * 4096f, Curve = 1,
                    Rect = (uint)(u0 | v0 << 8 | u1 << 16 | v1 << 24),
                    Flags = RetainedScene.FlagRect | half, Rgbc = src.Rgbc,
                };
            }
        }
        return n;
    }

    /// <summary>From <see cref="MapCell"/>, before it hands a cell's object to the
    /// map writer: true when the retained map draws it and the writer is skipped.</summary>
    public static bool Submit(PSMemory m, uint col, uint row, uint obj)
    {
        if (!GpuWorld.Capture || !GpuWorld.MapOn) return false;
        GpuWorld.MapCells++;
        if (!Ready) { GpuWorld.Fallback(0x8001DE18, 0x8001E5EC, "map-not-built"); return false; }
        if (col >= Grid || row >= Grid) { GpuWorld.Fallback(0x8001DE18, 0x8001E5EC, "map-cell-range"); return false; }
        int cell = (int)(row * Grid + col);
        int slot = obj < 100 ? SlotLow[cell] : SlotHigh[cell];
        if (slot < 0) { GpuWorld.Fallback(0x8001DE18, 0x8001E5EC, "map-cell-unbuilt"); return false; }
        int t = slot >> 1;
        RetainedScene.NoteHalf(t % 80, t / 80, slot & 1);
        GpuWorld.MapRetained++;
        return GpuWorld.Drawing;
    }

    // ---- KF1_GPU_CHECK: the built corners against the GTE's ----------------------

    static long _checked, _behindAgree, _behindDisagree, _screenBad, _cueBad, _colourExact, _colourBad;
    static float _maxScreen, _maxZ, _maxCue;
    static int _checkCol, _checkRow, _checkObj = -1;

    public static bool Checking => GpuWorld.Check && GpuWorld.Capture && GpuWorld.MapOn && Ready && !GpuWorld.Drawing;

    /// <summary>Before the writer runs on a cell: remember which, to check its packets.</summary>
    public static void CheckCell(uint col, uint row, uint obj)
    {
        _checkCol = (int)col; _checkRow = (int)row; _checkObj = (int)obj;
    }

    /// <summary>After the writer: every vertex of the object, projected by the GPU's
    /// formula from the built world position, against the vertex cache.</summary>
    public static void CheckVertices(PSMemory m)
    {
        if (_checkObj < 0 || MeshOf(m, _checkObj) is not { } mesh) return;
        int turn = m.ReadU8(Turns + (uint)(_checkRow * Grid + _checkCol)) - 1;
        if ((uint)turn > 3) return;
        Span<short> r = stackalloc short[9];
        for (int j = 0; j < 9; j++) r[j] = (short)m.ReadU16(Orient + (uint)turn * 32 + (uint)j * 2);
        var o = Origin(m, _checkCol, _checkRow, turn);
        // The view the frame is drawn with, as taken at its head.
        if (RetainedScene.Find(RetainedScene.Serial) is not { } frame) return;
        var view = frame.View;
        for (int i = 0; i < mesh.Count; i++)
        {
            var w = World(m, mesh, r, o, i * 8);
            var (sx, sy, z) = Project(view, w.X, w.Y, w.Z);
            uint e = Cache + (uint)i * 8;
            int gx = (short)m.ReadU16(e), gy = (short)m.ReadU16(e + 2), gz = m.ReadU16(e + 4), ir2 = m.ReadU16(e + 6);
            _checked++;
            if (z < 1f || gz == 0)
            {
                if ((z < 1f) == (gz == 0)) _behindAgree++; else _behindDisagree++;
                continue;
            }
            float ds = Math.Max(Math.Abs(sx - gx), Math.Abs(sy - gy)), dz = Math.Abs(z - gz);
            // Measured near the screen and where the GTE's divide is unsaturated (SZ at
            // least H/2): off the screen a unit's rounding is many pixels and draws nothing.
            if (gz >= view.H / 2 && gx is > -64 and < 384 && gy is > -64 and < 304)
            {
                _maxScreen = Math.Max(_maxScreen, ds);
                if (ds > 1.5f) { _screenBad++; Sample("screen", i, sx, sy, z, gx, gy, gz, w, view); }
            }
            _maxZ = Math.Max(_maxZ, dz);
            // The cue the shader works out, against the one the game used (IR0 x 2).
            float q = Math.Min(view.H * 65536f / Math.Max(z, 1f), 131071f);
            float ir0 = Math.Clamp((_src.Dqa * q + _src.Dqb) / 4096f, 0f, 4096f);
            float dc = Math.Abs(ir0 * 2f - ir2);
            _maxCue = Math.Max(_maxCue, dc);
            if (dc > 16f) { _cueBad++; Sample($"cue {ir0 * 2:0} vs {ir2}", i, sx, sy, z, gx, gy, gz, w, view); }
        }
        CheckInterior(m, mesh, r, o, view);
    }

    static long _interior, _interiorExact, _interiorOne, _interiorWorse;
    static int _interiorMax;

    /// <summary>The fog inside the faces: at each triangle's centroid and edge midpoints,
    /// the shader's per-pixel cue (the corners' raw IR0, interpolated affinely on screen,
    /// then clamped and put through the offset curve) against DPCS at the point's own
    /// depth, on a white face (the worst case), in levels of 255.</summary>
    static void CheckInterior(PSMemory m, Mesh mesh, ReadOnlySpan<short> r, (int X, int Y, int Z) o, in RetainedScene.View view)
    {
        Span<(float X, float Y, float Z)> w = stackalloc (float, float, float)[4];
        Span<(float Sx, float Sy, float Z)> p = stackalloc (float, float, float)[4];
        Span<ushort> vi = stackalloc ushort[4];
        foreach (var face in mesh.Faces)
        {
            vi[0] = face.V0; vi[1] = face.V1; vi[2] = face.V2; vi[3] = face.V3;
            for (int k = 0; k < face.N; k++) { w[k] = World(m, mesh, r, o, vi[k] * 8); p[k] = Project(view, w[k].X, w[k].Y, w[k].Z); }
            Triangle(view, w, p, 0, 1, 2);
            if (face.N == 4) Triangle(view, w, p, 1, 3, 2);
        }
    }

    static void Triangle(in RetainedScene.View view, Span<(float X, float Y, float Z)> w, Span<(float Sx, float Sy, float Z)> p, int a, int b, int c)
    {
        if (p[a].Z < view.H / 2 || p[b].Z < view.H / 2 || p[c].Z < view.H / 2) return;
        float det = (p[b].Sx - p[a].Sx) * (p[c].Sy - p[a].Sy) - (p[c].Sx - p[a].Sx) * (p[b].Sy - p[a].Sy);
        if (Math.Abs(det) < 4f) return;
        float ra = Raw(view, p[a].Z), rb = Raw(view, p[b].Z), rc = Raw(view, p[c].Z);
        ReadOnlySpan<(float, float, float)> at = [(1 / 3f, 1 / 3f, 1 / 3f), (0.5f, 0.5f, 0), (0, 0.5f, 0.5f), (0.5f, 0, 0.5f)];
        foreach (var (ka, kb, kc) in at)
        {
            float x = ka * w[a].X + kb * w[b].X + kc * w[c].X, y = ka * w[a].Y + kb * w[b].Y + kc * w[c].Y,
                  z = ka * w[a].Z + kb * w[b].Z + kc * w[c].Z;
            var q = Project(view, x, y, z);
            if (q.Z < view.H / 2) continue;
            // Screen barycentrics of the point's projection.
            float u = ((q.Sx - p[a].Sx) * (p[c].Sy - p[a].Sy) - (p[c].Sx - p[a].Sx) * (q.Sy - p[a].Sy)) / det;
            float v = ((p[b].Sx - p[a].Sx) * (q.Sy - p[a].Sy) - (q.Sx - p[a].Sx) * (p[b].Sy - p[a].Sy)) / det;
            float raw = (1 - u - v) * ra + u * rb + v * rc;
            float wgt = Math.Max(Math.Clamp(raw, 0f, 4096f) - 800f, 0f) * 2f;
            int mine = (int)MathF.Round(Math.Clamp(255f * (1f - wgt / 4096f), 0f, 255f));
            // The GTE: RTPS's IR0 at the point's SZ, DPCS at twice it.
            long sz = Math.Min((long)q.Z, 65535);
            long qd = Math.Min((long)view.H * 65536L / Math.Max(sz, 1), 131071L);
            int ir0 = (int)Math.Clamp((_src.Dqa * qd + _src.Dqb) >> 12, 0, 4096);
            int gte = Dpcs(255, 2 * ir0);
            int d = Math.Abs(mine - gte);
            _interior++;
            if (d == 0) _interiorExact++; else if (d == 1) _interiorOne++; else _interiorWorse++;
            _interiorMax = Math.Max(_interiorMax, d);
        }
    }

    /// <summary>The raw IR0 the shader interpolates for a corner at view depth z.</summary>
    static float Raw(in RetainedScene.View view, float z)
    {
        float q = Math.Min(view.H * 65536f / Math.Max(z, 1f), 131071f);
        return (_src.Dqa * q + _src.Dqb + 800f * 4096f) / 4096f;
    }

    static int _samples;

    static void Sample(string what, int i, float sx, float sy, float z, int gx, int gy, int gz,
        (float X, float Y, float Z) w, in RetainedScene.View v)
    {
        if (_samples++ >= 16) return;
        Console.WriteLine($"[KF1] map check {what}: cell {_checkCol},{_checkRow} obj {_checkObj} v{i} world ({w.X:0},{w.Y:0},{w.Z:0}) " +
            $"cam ({v.CamX:0},{v.CamY:0},{v.CamZ:0}) mine ({sx:0.0},{sy:0.0},{z:0}) gte ({gx},{gy},{gz})");
    }

    /// <summary>Just before a map packet is linked: its corners' colours against the
    /// built face's colour through DPCS at the cache's IR0.</summary>
    public static void CheckFace(PSMemory m, uint pkt, uint prim, uint p0, uint p1, uint p2, uint p3)
    {
        if (_checkObj < 0 || MeshOf(m, _checkObj) is not { } mesh) return;
        int turn = m.ReadU8(Turns + (uint)(_checkRow * Grid + _checkCol)) - 1;
        if ((uint)turn > 3) return;
        int f = Array.FindIndex(mesh.Faces, x => x.Prim == prim);
        if (f < 0) return;
        Span<short> llm = stackalloc short[9];
        for (int j = 0; j < 9; j++) llm[j] = (short)m.ReadU16(Lights + (uint)turn * 32 + (uint)j * 2);
        uint np = _tmd + 0xC + m.ReadU32(_tmd + 0xC + (uint)_checkObj * 28 + 8) + (uint)mesh.Faces[f].Normal;
        var c = Nccs(_src, llm, (short)m.ReadU16(np), (short)m.ReadU16(np + 2), (short)m.ReadU16(np + 4), _src.Rgbc);
        bool quad = mesh.Faces[f].N == 4;
        Span<uint> at = [p0, p1, p2, p3];
        for (int k = 0; k < (quad ? 4 : 3); k++)
        {
            int ir0 = (short)m.ReadU16(at[k] + 6);
            uint got = m.ReadU32(pkt + 4 + (uint)k * 12) & 0xFFFFFF;
            uint want = (uint)Dpcs(c.R, ir0) | (uint)Dpcs(c.G, ir0) << 8 | (uint)Dpcs(c.B, ir0) << 16;
            if (got == want) _colourExact++; else _colourBad++;
        }
    }

    /// <summary>DPCS to a black far colour, sf 1.</summary>
    static int Dpcs(int c, int ir0)
    {
        long ir = Math.Clamp((0L - ((long)c << 16)) >> 12, -32768, 32767);
        long mac = (((long)c << 16) + ir0 * ir) >> 12;
        return (int)Math.Clamp(mac >> 4, 0, 255);
    }

    public static (float Sx, float Sy, float Z) Project(in RetainedScene.View v, float x, float y, float z)
    {
        double dx = x - v.CamX, dy = y - v.CamY, dz = z - v.CamZ;
        float vx = (float)(v.R00 * dx + v.R01 * dy + v.R02 * dz) + v.Tx;
        float vy = (float)(v.R10 * dx + v.R11 * dy + v.R12 * dz) + v.Ty;
        float vz = (float)(v.R20 * dx + v.R21 * dy + v.R22 * dz) + v.Tz;
        if (vz < 1f) return (0, 0, vz);
        return (v.Cx + v.H * vx / vz, v.Cy + v.H * vy / vz, vz);
    }

    public static string CheckReport() =>
        $"map vertices {_checked}: screen max {_maxScreen:0.00} px (over 1.5: {_screenBad}), z max {_maxZ:0.0}, " +
        $"behind agree/disagree {_behindAgree}/{_behindDisagree}, cue max {_maxCue:0} (over 16: {_cueBad}); " +
        $"corner colours exact {_colourExact}, off {_colourBad}; interior fog {_interior}: exact {_interiorExact}, " +
        $"off by 1 {_interiorOne}, more {_interiorWorse} (max {_interiorMax} of 255)";
}
