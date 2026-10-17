using System.Runtime.InteropServices;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Diagnostics;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// The model walk's models as instances of meshes kept on the GPU
/// (<see cref="RetainedScene.AddInstance"/>), published by the polygon writer
/// <c>func_8001C7F8</c> (<see cref="PolyAssembler"/>) at its entry, where the submitter
/// has set everything up:
///
/// - the object: <c>a0</c> of the TMD at <c>0x80090FC8</c> (TMD 1, or a bank model's);
///   its faces are the TMD's own primitives, the twelve even codes
///   <c>0x20</c>-<c>0x3C</c> the writer's table dispatches;
/// - the placement: the GTE's rotation and translation, so the instance is placed in
///   view space exactly as RTPS places it (the view and the model carried between
///   ticks are already in them);
/// - the light: the GTE's light matrix, colour matrix and back colour, the RGBC at
///   <c>0x80057B58</c> for textured faces and the face's own colour otherwise, then the
///   depth cue at twice IR0 (the vertex cache's <c>+6</c>), the offset curve with DQB
///   raised by 800 x 4096, as the map's;
/// - the pose: the list at <c>0x800910BC</c>. The object's own vertices are a rigid
///   pose, kept once. The pose blender's scratch list (<c>0x800930F8</c>) is its block
///   (the vertex list plus every whole key) plus the segment's last key times the
///   weight it left in IR0, which is the runtime's morph form: key and delta texels kept
///   once, only the weight changing. <see cref="PoseBlend"/>'s post hook records them.
///
/// The cached writer <c>func_8001D730</c> (area 5's objects and some types everywhere)
/// is the same walk with its own CLUT, tpage and RGBC (<see cref="SubmitCached"/>).
/// A model with a semi-transparent face stays packets, counted by reason. Step 3 of
/// docs/RENDERER_PLAN.md.
/// </summary>
public static class RetainedModels
{
    const uint CurrentTmd = 0x80090FC8, VertexList = 0x800910BC, Cache = 0x800911B0;
    const uint Scratch = 0x800930F8, TexturedRgbc = 0x80057B58, CachedRgbc = 0x80057B5C, CachedClut = 0x80095058;

    public static long MeshBuilds, MeshHits, PoseBuilds, PoseHits, PoseTexelsUploaded, VertexUploads;

    sealed class Mesh
    {
        public int Start, Opaque, Total, MaxVertex, Gen;
        public uint Header;
        public ulong Hash;
        /// <summary>The measured blend mode of its semi faces (<see cref="SemiAbrTable"/>), -1 unmeasured.</summary>
        public int SemiAbr = -1;
        /// <summary>Its semi triangles: the corner offset from the mesh start and the three
        /// vertex indices in the writer's order (S2, S3, S4).</summary>
        public readonly List<(int Offset, uint V0, uint V1, uint V2)> SemiFaces = new();
        public bool SemiQuad;
        public bool Semi;
        public string? Reject;
        /// <summary>Every face in the writer's order: its corner offset from the mesh start,
        /// its corners in the store (3, or 6 for a quad) and its vertex indices (V3 for a quad).</summary>
        public readonly List<(int Offset, int Corners, uint V0, uint V1, uint V2, uint V3)> Faces = new();
    }

    static readonly Dictionary<(uint Header, ulong Hash), Mesh> Meshes = new();

    /// <summary>Semi meshes whose blend mode was measured on a walk (KF1_SEMI_ABR, areas 1-4,
    /// 2026-10-15 and 2026-10-16): every face draws with ABR 0. Any other semi mesh stays on the packets.</summary>
    static readonly Dictionary<(uint Header, ulong Hash), int> SemiAbrTable = new()
    {
        [(0x80182240u, 0xACAAA2D522E4E149UL)] = 0,
        [(0x800E805Cu, 0x6D833BCEA568C065UL)] = 0,
        [(0x80176528u, 0xACAAA2D522D20B51UL)] = 0,
        [(0x8016A510u, 0xF7D647947117B9B2UL)] = 0,
        [(0x8016DA24u, 0xACAAA2D522CD262DUL)] = 0,
        [(0x800E9320u, 0x908D51EA6CBAFB69UL)] = 0,
        [(0x80163F4Cu, 0xACAAA2D522BE66F5UL)] = 0,
        // Area 4's person-submitter mesh past the stencil, met only under the render
        // distance (Step 8, 2026-10-16).
        [(0x80156B70u, 0xCE0E2BF9261B866AUL)] = 0,
    };

    /// <summary>TileBias (GlMainView): a blended face's key is its table slot plus this.</summary>
    const int BlendKeyBias = 0xF0;
    static readonly Dictionary<(uint Address, int Count, ulong Hash), int> Rigid = new();
    static readonly Dictionary<(ulong Block, uint Key, int Count), int> Morphs = new();
    static int _gen = -1;

    // The last pose blend: its record, the weight it left in IR0 and its vertex count.
    static uint _blendRecord;
    static int _blendWeight, _blendCount;
    static bool _attached;

    static readonly ModInfo _self = new() { Id = "kf1.retained-models", Name = "Retained models", Version = "1.0" };

    // The frame profiler's sections for the models' CPU work (free while profiling is off).
    static readonly int ProfSubmit = Profiler.Register("retained models: submit", ProfileGroup.Hook);
    static readonly int ProfMesh = Profiler.Register("retained models: mesh build", ProfileGroup.Hook);
    static readonly int ProfPose = Profiler.Register("retained models: pose upload", ProfileGroup.Hook);

    public static void Install() => HookAttach.OnOverlayLoad("retained models", Attach);

    static bool Attach()
    {
        SymbolRegistry.Build();
        var blend = SymbolRegistry.Resolve("game", null, 0x800205D4);
        if (blend == null) return false;
        if (!_attached)
        {
            HookManager.AddPre(_self, blend, typeof(RetainedModels).GetMethod(nameof(BlendIn))!);
            HookManager.AddPost(_self, blend, typeof(RetainedModels).GetMethod(nameof(BlendOut))!);
            _attached = true;
        }
        HookManager.Commit();
        return HookAttach.Installed(blend);
    }

    static ushort _blendVertices;

    public static void BlendIn(CpuContext c, IMemory m) => _blendVertices = m.ReadU16(c.SP + 0x10u);

    public static void BlendOut(CpuContext c, IMemory m)
    {
        _blendRecord = 0;
        if (c.V0 < 0x80000000u || (c.V0 & 0x1FFFFF) < 0x10) return;
        _blendRecord = c.V0;
        _blendWeight = (short)Gte.Read(8);
        _blendCount = _blendVertices;
    }

    /// <summary>At the frame's start: an emptied store drops every cached index.</summary>
    public static void Frame()
    {
        if (_gen == RetainedScene.MeshGeneration) return;
        Meshes.Clear(); Rigid.Clear(); Morphs.Clear();
        _gen = RetainedScene.MeshGeneration;
    }

    static ulong Hash(PSMemory m, uint address, int bytes)
    {
        ulong h = 0xCBF29CE484222325UL;
        var span = m.Ram.Slice((int)(address & 0x1FFFFF), bytes);
        var words = MemoryMarshal.Cast<byte, ulong>(span);
        foreach (ulong w in words) h = (h ^ w) * 0x100000001B3UL + 0x9E3779B97F4A7C15UL;
        for (int i = words.Length * 8; i < span.Length; i++) h = (h ^ span[i]) * 0x100000001B3UL;
        return h;
    }

    static bool InRam(uint a, uint bytes) => (a >> 24) == 0x80 && (a & 0xFFFFFF) + bytes <= 0x200000;

    /// <summary>A primitive's layout: its corners, whether textured, Gouraud, and the
    /// offsets of its first normal and first vertex (standard TMD, lighting on).</summary>
    static bool Layout(uint code, out int n, out bool textured, out bool gouraud, out int normal, out int vertex)
    {
        textured = (code & 4) != 0; gouraud = (code & 0x10) != 0;
        n = (code & 8) != 0 ? 4 : 3;
        int head = textured ? (n == 4 ? 16 : 12) : 4;
        normal = head; vertex = head + 2;
        return (code & 1) == 0 && code is >= 0x20 and <= 0x3C;
    }

    /// <summary>The object's mesh. <paramref name="tex"/>, for the cached writer, is the CLUT and
    /// tpage it writes in place of the prim's (<c>0x80095058</c>/<c>5A</c>), 0 for the main writer.</summary>
    static Mesh GetMesh(PSMemory m, uint tmd, uint obj, uint tex = 0)
    {
        uint header = tmd + 0xC + obj * 28, baseAt = tmd + 0xC;
        uint prims = baseAt + m.ReadU32(header + 16), nprims = m.ReadU32(header + 20);
        uint normals = baseAt + m.ReadU32(header + 8);
        // The primitives' extent, then their bytes, key the mesh.
        uint at = prims;
        for (uint i = 0; i < nprims && i < 8192; i++) at += 4 + ((m.ReadU32(at) >> 6) & 0x3FC);
        ulong hash = Hash(m, prims, (int)(at - prims)) ^ (ulong)normals * 31 ^ (ulong)tex * 0x9E3779B97F4A7C15;
        if (Meshes.TryGetValue((header, hash), out var mesh)) { MeshHits++; return mesh; }
        int b = Profiler.Begin(ProfMesh);
        try { mesh = Build(m, prims, nprims, normals, tex); }
        finally { Profiler.End(b); }
        mesh.Header = header; mesh.Hash = hash;
        mesh.SemiAbr = SemiAbrTable.TryGetValue((header, hash), out int abr) ? abr : -1;
        Meshes[(header, hash)] = mesh;
        MeshBuilds++;
        return mesh;
    }

    static Mesh Build(PSMemory m, uint prims, uint nprims, uint normals, uint tex)
    {
        var mesh = new Mesh { MaxVertex = -1, Gen = RetainedScene.MeshGeneration };
        var faces = new List<RetainedScene.Vertex[]>();
        uint at = prims;
        Span<uint> vi = stackalloc uint[4], ni = stackalloc uint[4];
        for (uint i = 0; i < nprims; i++)
        {
            uint word = m.ReadU32(at), body = at + 4, code = word >> 24;
            at = body + ((word >> 6) & 0x3FC);
            if (!Layout(code, out int n, out bool textured, out bool gouraud, out int normal, out int vertex)) continue;
            // The cached writer takes the four lit textured kinds and skips the rest.
            if (tex != 0 && code is not (0x24 or 0x2C or 0x34 or 0x3C)) continue;
            // Untextured semi-transparent faces too: their blend mode is the GPU's draw mode, not the prim's.
            bool semi = (code & 2) != 0;
            if (semi) mesh.Semi = true;
            int step = gouraud ? 4 : 2;
            int u0 = 255, v0 = 255, u1 = 0, v1 = 0;
            for (int k = 0; k < n; k++)
            {
                vi[k] = m.ReadU16(body + (uint)(vertex + k * step)) / 8u;
                ni[k] = m.ReadU16(body + (uint)(normal + (gouraud ? k * 4 : 0)));
                mesh.MaxVertex = Math.Max(mesh.MaxVertex, (int)vi[k]);
                if (textured)
                {
                    uint uv = m.ReadU16(body + (uint)k * 4);
                    u0 = Math.Min(u0, (int)(uv & 255)); u1 = Math.Max(u1, (int)(uv & 255));
                    v0 = Math.Min(v0, (int)(uv >> 8)); v1 = Math.Max(v1, (int)(uv >> 8));
                }
            }
            uint tpage = tex != 0 ? tex >> 16 & 0x1FFu : textured ? m.ReadU16(body + 6) & 0x1FFu : 0x8000u;
            var template = new RetainedScene.Vertex
            {
                Clut = tex != 0 ? tex & 0xFFFF : textured ? m.ReadU16(body + 2) : 0, Texpage = tpage,
                Dqa = vi[0], Dqb = vi[1], Curve = vi[2], Rgbc = n == 4 ? vi[3] : uint.MaxValue,
                Rect = textured ? (uint)(u0 | v0 << 8 | u1 << 16 | v1 << 24) : 0,
                Flags = RetainedScene.FlagDots | (textured ? RetainedScene.FlagRect : 0)
                    | (semi ? RetainedScene.FlagSemi | (tpage >> 5 & 3) << 8 : 0),
                Light = textured ? 0 : RetainedScene.FaceColour | (m.ReadU32(body) & 0xFFFFFF),
            };
            ReadOnlySpan<int> order = n == 4 ? [0, 1, 2, 1, 3, 2] : [0, 1, 2];
            var corners = new RetainedScene.Vertex[order.Length];
            for (int j = 0; j < order.Length; j++)
            {
                int k = order[j];
                var v = template;
                uint np = normals + ni[k];
                v.X = vi[k];
                v.R = (short)m.ReadU16(np); v.G = (short)m.ReadU16(np + 2); v.B = (short)m.ReadU16(np + 4);
                if (textured) { uint uv = m.ReadU16(body + (uint)k * 4); v.U = uv & 255; v.V = uv >> 8; }
                if (j >= 3) v.Flags |= RetainedScene.FlagQuadTail;
                corners[j] = v;
            }
            faces.Add(corners);
        }
        // Last face first, as the writer links them: the first is drawn on top.
        var all = new List<RetainedScene.Vertex>();
        var at0 = new int[faces.Count];
        for (int f = faces.Count - 1; f >= 0; f--)
            if ((faces[f][0].Flags & RetainedScene.FlagSemi) == 0) { at0[f] = all.Count; all.AddRange(faces[f]); }
        mesh.Opaque = all.Count;
        for (int f = faces.Count - 1; f >= 0; f--)
        {
            if ((faces[f][0].Flags & RetainedScene.FlagSemi) == 0) continue;
            if (faces[f].Length == 6) mesh.SemiQuad = true;
            else mesh.SemiFaces.Add((all.Count, (uint)faces[f][0].X, (uint)faces[f][1].X, (uint)faces[f][2].X));
            at0[f] = all.Count;
            all.AddRange(faces[f]);
        }
        // A quad's corners are kept as 0, 1, 2 then 1, 3, 2: its fourth vertex is the fifth corner.
        for (int f = 0; f < faces.Count; f++)
        {
            var c = faces[f];
            mesh.Faces.Add((at0[f], c.Length, (uint)c[0].X, (uint)c[1].X, (uint)c[2].X, c.Length == 6 ? (uint)c[4].X : 0u));
        }
        mesh.Total = all.Count;
        if (mesh.Total == 0) { mesh.Reject = "model-no-faces"; return mesh; }
        mesh.Start = RetainedScene.AddMesh(CollectionsMarshal.AsSpan(all));
        return mesh;
    }

    static int StoreRigid(PSMemory m, uint address, int count)
    {
        ulong hash = Hash(m, address, count * 8);
        var key = (address, count, hash);
        if (Rigid.TryGetValue(key, out int pose)) { PoseHits++; return pose; }
        int u = Profiler.Begin(ProfPose);
        try { pose = RetainedScene.AddPose(MemoryMarshal.Cast<byte, short>(m.Ram.Slice((int)(address & 0x1FFFFF), count * 8))) + 1; }
        finally { Profiler.End(u); }
        Rigid[key] = pose; PoseBuilds++; PoseTexelsUploaded += count;
        return pose;
    }

    static short[] _texels = [];

    /// <summary>The blend's block and last key as key and delta texels; the index + 1.</summary>
    static int StoreMorph(PSMemory m, uint record, int count)
    {
        uint block = m.ReadU32(record + 0xC), key = m.ReadU32(record + 8);
        if (!InRam(block, (uint)count * 8) || !InRam(key, 12)) return 0;
        uint start = m.ReadU32(key + 4), keys = m.ReadU32(key + 8);
        if (start + keys > count || !InRam(key + 0xC, keys * 8)) return 0;
        var id = (Hash(m, block, count * 8), key, count);
        if (Morphs.TryGetValue(id, out int pose)) { PoseHits++; return pose; }
        int u = Profiler.Begin(ProfPose);
        try
        {
            if (_texels.Length < count * 8) _texels = new short[count * 8];
            var t = _texels.AsSpan(0, count * 8);
            t.Clear();
            for (int i = 0; i < count; i++)
                for (int k = 0; k < 4; k++) t[i * 8 + k] = (short)m.ReadU16(block + (uint)(i * 8 + k * 2));
            // gteMIMefunc is called one entry early on both lists and one long (the
            // scratch's entry before start saved and restored), so vertex start + i takes
            // the key's entry i, at +0xC.
            for (uint i = 0; i < keys; i++)
                for (int k = 0; k < 3; k++) t[(int)(start + i) * 8 + 4 + k] = (short)m.ReadU16(key + 0xC + i * 8 + (uint)k * 2);
            pose = RetainedScene.AddPose(t) + 1;
        }
        finally { Profiler.End(u); }
        Morphs[id] = pose; PoseBuilds++; PoseTexelsUploaded += count * 2;
        return pose;
    }

    static int Ctl(int r) => (int)Gte.ReadControl(r);
    static short Lo(int r) => (short)Gte.ReadControl(r);
    static short Hi(int r) => (short)(Gte.ReadControl(r) >> 16);

    /// <summary>From <see cref="PolyAssembler"/>'s entry: true when the model is
    /// retained and drawn, and the writer is skipped.</summary>
    public static bool Submit(CpuContext c, PSMemory m)
    {
        int p = Profiler.Begin(ProfSubmit);
        try { return SubmitCore(c, m, false); }
        finally { Profiler.End(p); }
    }

    /// <summary>From <see cref="PolyAssemblerCached"/>'s entry: the same object, posed and lit
    /// the same way, with the CLUT and tpage at <c>0x80095058</c>/<c>5A</c> and the RGBC at
    /// <c>0x80057B5C</c>.</summary>
    public static bool SubmitCached(CpuContext c, PSMemory m)
    {
        int p = Profiler.Begin(ProfSubmit);
        try { return SubmitCore(c, m, true); }
        finally { Profiler.End(p); }
    }

    static bool SubmitCore(CpuContext c, PSMemory m, bool cached)
    {
        SemiAbr.Begin();
        if (!GpuWorld.Capture) return false;
        uint routine = cached ? 0x8001D730u : 0x8001C7F8u;
        if (!cached && GpuWorld.Domain == "arm" && GpuWorld.ArmOn) return SubmitArm(c, m);
        // The compass and the menu's model keep their packets: drawn over the world in
        // painter's order, with no depth record (PacketDepth).
        if (GpuWorld.Domain != "models") { GpuWorld.Fallback(routine, c.RA, $"packet-owned-{GpuWorld.Domain}"); return false; }
        if (!GpuWorld.ModelsOn) return false;
        GpuWorld.ModelSubmits++;
        uint tmd = m.ReadU32(CurrentTmd), obj = c.A0 & 0xFFFF;
        if (!InRam(tmd, 16) || obj >= m.ReadU32(tmd + 8)) { GpuWorld.Fallback(routine, c.RA, "model-object-range"); return false; }
        var mesh = GetMesh(m, tmd, obj, cached ? m.ReadU16(CachedClut) | (uint)m.ReadU16(CachedClut + 2) << 16 | 0x80000000u : 0);
        if (mesh.Reject != null) { GpuWorld.Fallback(routine, c.RA, mesh.Reject); return false; }
        if (mesh.Semi)
        {
            SemiAbr.Current = (mesh.Header, mesh.Hash);
            if (mesh.SemiAbr < 0) { GpuWorld.Fallback(routine, c.RA, "model-semi-abr-unmeasured"); return false; }
            if (mesh.SemiQuad) { GpuWorld.Fallback(routine, c.RA, "model-semi-quad"); return false; }
        }
        uint header = tmd + 0xC + obj * 28;
        uint own = tmd + 0xC + m.ReadU32(header), count = m.ReadU32(header + 4);
        uint list = m.ReadU32(VertexList);
        if (mesh.MaxVertex >= count) { GpuWorld.Fallback(routine, c.RA, "model-vertex-range"); return false; }
        int pose, weight = 0;
        bool morph = false;
        if (list == own) pose = StoreRigid(m, own, (int)count);
        else if (list == Scratch && _blendRecord != 0 && _blendCount == count)
        {
            pose = StoreMorph(m, _blendRecord, (int)count);
            weight = _blendWeight; morph = true;
        }
        else { GpuWorld.Fallback(routine, c.RA, "model-pose-source"); return false; }
        if (pose == 0) { GpuWorld.Fallback(routine, c.RA, "model-pose-range"); return false; }

        int bias = (short)c.A1;
        _caller = c.RA; _bias = bias;
        var view = GpuWorld.ReadView(m);
        var ins = new RetainedScene.ModelInstance
        {
            MeshStart = mesh.Start, MeshCount = mesh.Opaque, MeshAll = mesh.Total,
            Pose = pose, PoseWeight = weight, PoseMorph = morph,
            ViewSpace = true,
            V00 = Lo(0), V01 = Hi(0), V02 = Lo(1), V10 = Hi(1), V11 = Lo(2), V12 = Hi(2),
            V20 = Lo(3), V21 = Hi(3), V22 = Lo(4), Vtx = Ctl(5), Vty = Ctl(6), Vtz = Ctl(7),
            Llm0 = Lo(8) / 4096f, Llm1 = Hi(8) / 4096f, Llm2 = Lo(9) / 4096f, Llm3 = Hi(9) / 4096f, Llm4 = Lo(10) / 4096f,
            Llm5 = Hi(10) / 4096f, Llm6 = Lo(11) / 4096f, Llm7 = Hi(11) / 4096f, Llm8 = Lo(12) / 4096f,
            Bk0 = Ctl(13), Bk1 = Ctl(14), Bk2 = Ctl(15),
            L0 = Lo(16), L1 = Hi(16), L2 = Lo(17), L3 = Hi(17), L4 = Lo(18), L5 = Hi(18), L6 = Lo(19), L7 = Hi(19), L8 = Lo(20),
            Dqa = (short)Gte.ReadControl(27), Dqb = (int)Gte.ReadControl(28) + 800f * 4096f, Curve = 1,
            // The writer drops a face whose mean SZ / 4 plus the bias is under 5; nothing far.
            Near = 5 - bias, Far = float.MaxValue,
            Rgbc = m.ReadU32(cached ? CachedRgbc : TexturedRgbc) & 0xFFFFFF,
        };
        PlaceWorld(ref ins, view);
        if (GpuWorld.Check) CheckModel(m, ins, list, (int)count, morph);
        RetainedScene.AddInstance(ins);
        if (mesh.Semi) AddSemiFaces(m, mesh, bias);
        GpuWorld.ModelRetained++;
        if (cached) GpuWorld.CachedRetained++;
        return GpuWorld.Drawing;
    }

    static int[] _armOrder = new int[1024], _armRunKey = new int[64], _armRunAt = new int[64];
    static (int Key, int Face)[] _armFaces = new (int, int)[256];
    static readonly float[] _armBox = new float[4];
    static readonly Comparer<(int Key, int Face)> FarFirst =
        Comparer<(int Key, int Face)>.Create((p, q) => p.Key != q.Key ? q.Key - p.Key : q.Face - p.Face);

    /// <summary>The arm (<c>func_8001F798</c>) from its mesh, in painter's order as its
    /// packets were (<see cref="RetainedScene.SetArm"/>): placed in view space with the
    /// GTE's own matrix under its own projection (H 2000), its faces those the writer
    /// links, culled and keyed from the vertex cache as the writer does, far key first and
    /// the last linked first within a key.</summary>
    static bool SubmitArm(CpuContext c, PSMemory m)
    {
        const uint routine = 0x8001C7F8;
        uint tmd = m.ReadU32(CurrentTmd), obj = c.A0 & 0xFFFF;
        if (!InRam(tmd, 16) || obj >= m.ReadU32(tmd + 8)) { GpuWorld.Fallback(routine, c.RA, "arm-object-range"); return false; }
        var mesh = GetMesh(m, tmd, obj);
        if (mesh.Reject != null) { GpuWorld.Fallback(routine, c.RA, mesh.Reject); return false; }
        if (mesh.Semi) { GpuWorld.Fallback(routine, c.RA, "arm-semi"); return false; }
        uint header = tmd + 0xC + obj * 28;
        uint own = tmd + 0xC + m.ReadU32(header), count = m.ReadU32(header + 4);
        uint list = m.ReadU32(VertexList);
        if (mesh.MaxVertex >= count) { GpuWorld.Fallback(routine, c.RA, "arm-vertex-range"); return false; }
        int pose, weight = 0;
        bool morph = false;
        if (list == own) pose = StoreRigid(m, own, (int)count);
        else if (list == Scratch && _blendRecord != 0 && _blendCount == count)
        {
            pose = StoreMorph(m, _blendRecord, (int)count);
            weight = _blendWeight; morph = true;
        }
        else { GpuWorld.Fallback(routine, c.RA, "arm-pose-source"); return false; }
        if (pose == 0) { GpuWorld.Fallback(routine, c.RA, "arm-pose-range"); return false; }
        // One centre for the whole frame: the arm's must be the world's.
        if (Ctl(24) / 65536f != GpuWorld.ProjCx || Ctl(25) / 65536f != GpuWorld.ProjCy)
        { GpuWorld.Fallback(routine, c.RA, "arm-centre"); return false; }
        float h = (ushort)Gte.ReadControl(26);
        if (h <= 0) { GpuWorld.Fallback(routine, c.RA, "arm-projection"); return false; }

        // The writer's faces: NCLIP on the first three corners, the key the mean cached SZ
        // over four plus the bias, under 5 dropped.
        int bias = (short)c.A1, n = 0;
        float bx0 = float.MaxValue, by0 = float.MaxValue, bx1 = float.MinValue, by1 = float.MinValue;
        for (int i = 0; i < mesh.Faces.Count; i++)
        {
            var f = mesh.Faces[i];
            uint a = Cache + f.V0 * 8, b = Cache + f.V1 * 8, cc = Cache + f.V2 * 8, d = Cache + f.V3 * 8;
            long x0 = (short)m.ReadU16(a), y0 = (short)m.ReadU16(a + 2);
            long x1 = (short)m.ReadU16(b), y1 = (short)m.ReadU16(b + 2);
            long x2 = (short)m.ReadU16(cc), y2 = (short)m.ReadU16(cc + 2);
            if (x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1 <= 0) continue;
            bool quad = f.Corners == 6;
            int z = (short)m.ReadU16(a + 4) + (short)m.ReadU16(b + 4) + (short)m.ReadU16(cc + 4);
            int key = quad ? (z + (short)m.ReadU16(d + 4)) >> 4 : z / 3 >> 2;
            key += bias;
            if (key < 5) continue;
            if (n == _armFaces.Length) Array.Resize(ref _armFaces, n * 2);
            _armFaces[n++] = (key & 0x3FFF, i);
            bx0 = Math.Min(bx0, Math.Min(x0, Math.Min(x1, x2))); bx1 = Math.Max(bx1, Math.Max(x0, Math.Max(x1, x2)));
            by0 = Math.Min(by0, Math.Min(y0, Math.Min(y1, y2))); by1 = Math.Max(by1, Math.Max(y0, Math.Max(y1, y2)));
            if (quad)
            {
                long x3 = (short)m.ReadU16(d), y3 = (short)m.ReadU16(d + 2);
                bx0 = Math.Min(bx0, x3); bx1 = Math.Max(bx1, x3); by0 = Math.Min(by0, y3); by1 = Math.Max(by1, y3);
            }
        }
        if (GpuWorld.Check)
        {
            var slots = new List<int>(n);
            for (int i = 0; i < n; i++) slots.Add(_armFaces[i].Key);
            SemiAbr.Expect(slots, all: true);
        }
        // Nothing linked: the packets would have drawn nothing either.
        if (n == 0) { GpuWorld.ArmRetained++; return GpuWorld.Drawing; }
        // Far key first; within a key, the face linked last is walked first.
        Array.Sort(_armFaces, 0, n, FarFirst);
        int count2 = 0, runs = 0;
        for (int i = 0; i < n; i++)
        {
            var (key, face) = _armFaces[i];
            var f = mesh.Faces[face];
            if (i == 0 || key != _armFaces[i - 1].Key)
            {
                if (runs == _armRunKey.Length) { Array.Resize(ref _armRunKey, runs * 2); Array.Resize(ref _armRunAt, runs * 2); }
                // The runtime walks a table of 0x2000 (slot 0x1FFF less the key); this one
                // is 0x4000 long, so slot 0x3FFF less the key is the key less 0x2000 there.
                _armRunKey[runs] = key - 0x2000;
                _armRunAt[runs++] = count2;
            }
            if (count2 + f.Corners > _armOrder.Length) Array.Resize(ref _armOrder, _armOrder.Length * 2);
            for (int j = 0; j < f.Corners; j++) _armOrder[count2++] = mesh.Start + f.Offset + j;
        }
        _armBox[0] = bx0 - 1f; _armBox[1] = by0 - 1f; _armBox[2] = bx1 + 1f; _armBox[3] = by1 + 1f;

        var view = GpuWorld.ReadView(m);
        var ins = new RetainedScene.ModelInstance
        {
            MeshStart = mesh.Start, MeshCount = mesh.Opaque, MeshAll = mesh.Total,
            Pose = pose, PoseWeight = weight, PoseMorph = morph,
            ViewSpace = true,
            V00 = Lo(0), V01 = Hi(0), V02 = Lo(1), V10 = Hi(1), V11 = Lo(2), V12 = Hi(2),
            V20 = Lo(3), V21 = Hi(3), V22 = Lo(4), Vtx = Ctl(5), Vty = Ctl(6), Vtz = Ctl(7),
            Llm0 = Lo(8) / 4096f, Llm1 = Hi(8) / 4096f, Llm2 = Lo(9) / 4096f, Llm3 = Hi(9) / 4096f, Llm4 = Lo(10) / 4096f,
            Llm5 = Hi(10) / 4096f, Llm6 = Lo(11) / 4096f, Llm7 = Hi(11) / 4096f, Llm8 = Lo(12) / 4096f,
            Bk0 = Ctl(13), Bk1 = Ctl(14), Bk2 = Ctl(15),
            L0 = Lo(16), L1 = Hi(16), L2 = Lo(17), L3 = Hi(17), L4 = Lo(18), L5 = Hi(18), L6 = Lo(19), L7 = Hi(19), L8 = Lo(20),
            Dqa = (short)Gte.ReadControl(27), Dqb = (int)Gte.ReadControl(28) + 800f * 4096f, Curve = 1,
            // The faces are chosen above; no depth range.
            Near = float.MinValue, Far = float.MaxValue,
            Rgbc = m.ReadU32(TexturedRgbc) & 0xFFFFFF,
        };
        PlaceWorld(ref ins, view);
        if (GpuWorld.Check) CheckArm(m, ins, h, (int)count, morph, list);
        RetainedScene.SetArm(ins, _armOrder.AsSpan(0, count2), _armRunKey.AsSpan(0, runs), _armRunAt.AsSpan(0, runs), _armBox, h);
        GpuWorld.ArmRetained++;
        return GpuWorld.Drawing;
    }

    /// <summary>A measured semi mesh's faces, after the instance they belong to: each face the
    /// writer would link (front-facing by the GTE's NCLIP, slot at least 5) is a blended face at
    /// its slot plus <see cref="BlendKeyBias"/>, drawn at the mesh's measured mode. Under the
    /// check the slots are expected by the writer's packets (<see cref="SemiAbr"/>).</summary>
    static void AddSemiFaces(PSMemory m, Mesh mesh, int bias)
    {
        var slots = GpuWorld.Check ? new List<int>() : null;
        // Built last first (Build), so the writer's prim order is the list's reverse.
        for (int i = mesh.SemiFaces.Count - 1; i >= 0; i--)
        {
            var f = mesh.SemiFaces[i];
            uint a = Cache + f.V0 * 8, b = Cache + f.V1 * 8, c = Cache + f.V2 * 8;
            long x0 = (short)m.ReadU16(a), y0 = (short)m.ReadU16(a + 2);
            long x1 = (short)m.ReadU16(b), y1 = (short)m.ReadU16(b + 2);
            long x2 = (short)m.ReadU16(c), y2 = (short)m.ReadU16(c + 2);
            // The writer's NCLIP: a face is kept when the screen cross product is positive.
            if (x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1 <= 0) continue;
            // The writer's slot: the mean of the three SZ over four (sum / 3, then >> 2), plus the bias.
            int z = (short)m.ReadU16(a + 4) + (short)m.ReadU16(b + 4) + (short)m.ReadU16(c + 4);
            int slot = (z / 3 >> 2) + bias;
            if (slot < 5) continue;
            RetainedScene.AddBlendFace(slot + BlendKeyBias, mesh.Start + f.Offset, 3, mesh.SemiAbr,
                Math.Min(x0, Math.Min(x1, x2)), Math.Min(y0, Math.Min(y1, y2)),
                Math.Max(x0, Math.Max(x1, x2)), Math.Max(y0, Math.Max(y1, y2)));
            slots?.Add(slot & 0x3FFF);
        }
        if (slots != null) SemiAbr.Expect(slots);
    }

    /// <summary>The world placement (what lights and fogs by position read): the view
    /// matrix's transpose undoes the camera.</summary>
    internal static void PlaceWorld(ref RetainedScene.ModelInstance i, in RetainedScene.View v)
    {
        // World = Rv^T (Rgte p + T - Tv) + cam.
        float[] rv = [v.R00, v.R01, v.R02, v.R10, v.R11, v.R12, v.R20, v.R21, v.R22];
        float[] rg = [i.V00, i.V01, i.V02, i.V10, i.V11, i.V12, i.V20, i.V21, i.V22];
        Span<float> r = stackalloc float[9];
        for (int a = 0; a < 3; a++)
            for (int b = 0; b < 3; b++)
                r[a * 3 + b] = (rv[0 * 3 + a] * rg[0 * 3 + b] + rv[1 * 3 + a] * rg[1 * 3 + b] + rv[2 * 3 + a] * rg[2 * 3 + b]) / 4096f;
        float dx = i.Vtx - v.Tx, dy = i.Vty - v.Ty, dz = i.Vtz - v.Tz;
        i.R00 = r[0]; i.R01 = r[1]; i.R02 = r[2]; i.R10 = r[3]; i.R11 = r[4]; i.R12 = r[5]; i.R20 = r[6]; i.R21 = r[7]; i.R22 = r[8];
        i.Tx = (float)(rv[0] * dx + rv[3] * dy + rv[6] * dz + v.CamX);
        i.Ty = (float)(rv[1] * dx + rv[4] * dy + rv[7] * dz + v.CamY);
        i.Tz = (float)(rv[2] * dx + rv[5] * dy + rv[8] * dz + v.CamZ);
    }

    // ---- KF1_GPU_CHECK -----------------------------------------------------------------

    static long _checked, _screenBad, _behindAgree, _behindDisagree, _poseChecked, _poseBad;
    static float _maxScreen, _maxZ;
    static int _samples, _screenSamples, _bias;
    static uint _caller;

    /// <summary>The instance's vertices as the model shader places them (the pose
    /// texels blended by the weight, the GTE's integer transform), against the posed
    /// list and the vertex cache the writer reads.</summary>
    static void CheckModel(PSMemory m, in RetainedScene.ModelInstance i, uint list, int count, bool morph)
    {
        var store = RetainedScene.PoseStore;
        float h = GpuWorld.ProjH, cx = GpuWorld.ProjCx, cy = GpuWorld.ProjCy;
        for (int v = 0; v < count; v++)
        {
            int px, py, pz;
            if (morph)
            {
                int t = (i.Pose - 1 + v * 2) * 4;
                px = (short)(store[t] + (short)((store[t + 4] * i.PoseWeight) >> 12));
                py = (short)(store[t + 1] + (short)((store[t + 5] * i.PoseWeight) >> 12));
                pz = (short)(store[t + 2] + (short)((store[t + 6] * i.PoseWeight) >> 12));
                _poseChecked++;
                int lx = (short)m.ReadU16(list + (uint)v * 8), ly = (short)m.ReadU16(list + (uint)v * 8 + 2), lz = (short)m.ReadU16(list + (uint)v * 8 + 4);
                if (px != lx || py != ly || pz != lz)
                {
                    _poseBad++;
                    if (_samples++ < 16)
                    {
                        int q = (i.Pose - 1 + v * 2) * 4;
                        uint key = m.ReadU32(_blendRecord + 8);
                        Console.WriteLine($"[KF1] model check pose: v{v}/{count} w {i.PoseWeight} K ({store[q]},{store[q + 1]},{store[q + 2]}) " +
                            $"D ({store[q + 4]},{store[q + 5]},{store[q + 6]}) mine ({px},{py},{pz}) list ({lx},{ly},{lz}) " +
                            $"key start {m.ReadU32(key + 4)} n {m.ReadU32(key + 8)}");
                    }
                }
            }
            else
            {
                int t = (i.Pose - 1 + v) * 4;
                px = store[t]; py = store[t + 1]; pz = store[t + 2];
            }
            long ex = (((long)i.V00 * px + (long)i.V01 * py + (long)i.V02 * pz) >> 12) + i.Vtx;
            long ey = (((long)i.V10 * px + (long)i.V11 * py + (long)i.V12 * pz) >> 12) + i.Vty;
            long ez = (((long)i.V20 * px + (long)i.V21 * py + (long)i.V22 * pz) >> 12) + i.Vtz;
            uint e = Cache + (uint)v * 8;
            int gx = (short)m.ReadU16(e), gy = (short)m.ReadU16(e + 2), gz = m.ReadU16(e + 4);
            _checked++;
            if (ez < 1 || gz == 0)
            {
                if ((ez < 1) == (gz == 0)) _behindAgree++; else _behindDisagree++;
                continue;
            }
            float sx = cx + h * ex / ez, sy = cy + h * ey / ez;
            _maxZ = Math.Max(_maxZ, Math.Abs(Math.Min(ez, 65535) - gz));
            if (gz >= h / 2 && gx is > -64 and < 384 && gy is > -64 and < 304)
            {
                float ds = Math.Max(Math.Abs(sx - gx), Math.Abs(sy - gy));
                _maxScreen = Math.Max(_maxScreen, ds);
                if (ds > 1.5f)
                {
                    _screenBad++;
                    if (_screenSamples++ < 12)
                        Console.WriteLine($"[KF1] model check screen: v{v}/{count} morph {morph} p ({px},{py},{pz}) eye ({ex},{ey},{ez}) " +
                            $"mine ({sx:0.0},{sy:0.0}) gte ({gx},{gy},{gz}) caller {_caller:X8} bias {_bias}");
                }
            }
        }
    }

    static long _armChecked, _armBad, _armPoseBad;
    static float _armMax;

    /// <summary>The arm's vertices as the model shader places them under its own H (the
    /// corner held at H/2 where the GTE's divide saturates, the screen clamp), against the
    /// vertex cache.</summary>
    static void CheckArm(PSMemory m, in RetainedScene.ModelInstance i, float h, int count, bool morph, uint list)
    {
        var store = RetainedScene.PoseStore;
        for (int v = 0; v < count; v++)
        {
            int px, py, pz;
            if (morph)
            {
                int t = (i.Pose - 1 + v * 2) * 4;
                px = (short)(store[t] + (short)((store[t + 4] * i.PoseWeight) >> 12));
                py = (short)(store[t + 1] + (short)((store[t + 5] * i.PoseWeight) >> 12));
                pz = (short)(store[t + 2] + (short)((store[t + 6] * i.PoseWeight) >> 12));
                if (px != (short)m.ReadU16(list + (uint)v * 8) || py != (short)m.ReadU16(list + (uint)v * 8 + 2)
                    || pz != (short)m.ReadU16(list + (uint)v * 8 + 4)) _armPoseBad++;
            }
            else
            {
                int t = (i.Pose - 1 + v) * 4;
                px = store[t]; py = store[t + 1]; pz = store[t + 2];
            }
            long ex = (((long)i.V00 * px + (long)i.V01 * py + (long)i.V02 * pz) >> 12) + i.Vtx;
            long ey = (((long)i.V10 * px + (long)i.V11 * py + (long)i.V12 * pz) >> 12) + i.Vty;
            long ez = (((long)i.V20 * px + (long)i.V21 * py + (long)i.V22 * pz) >> 12) + i.Vtz;
            float z = Math.Max(Math.Clamp(ez, 0, 65535), h * 0.5f);
            float sx = Math.Clamp(GpuWorld.ProjCx + h * Math.Clamp(ex, -32768, 32767) / z, -1024f, 1023f);
            float sy = Math.Clamp(GpuWorld.ProjCy + h * Math.Clamp(ey, -32768, 32767) / z, -1024f, 1023f);
            uint e = Cache + (uint)v * 8;
            float ds = Math.Max(Math.Abs(sx - (short)m.ReadU16(e)), Math.Abs(sy - (short)m.ReadU16(e + 2)));
            _armChecked++;
            _armMax = Math.Max(_armMax, ds);
            if (ds > 1.5f && _armBad++ < 8)
                Console.WriteLine($"[KF1] arm check: v{v}/{count} eye ({ex},{ey},{ez}) mine ({sx:0.0},{sy:0.0}) " +
                    $"gte ({(short)m.ReadU16(e)},{(short)m.ReadU16(e + 2)}) sz>>3 {(short)m.ReadU16(e + 4)} h {h}");
        }
    }

    public static string CheckReport() =>
        $"model vertices {_checked}: screen max {_maxScreen:0.00} px (over 1.5: {_screenBad}), z max {_maxZ:0}, " +
        $"behind agree/disagree {_behindAgree}/{_behindDisagree}; posed vertices {_poseChecked}, off {_poseBad}; " +
        $"arm vertices {_armChecked}: screen max {_armMax:0.00} px (over 1.5: {_armBad}), posed off {_armPoseBad}";
}
