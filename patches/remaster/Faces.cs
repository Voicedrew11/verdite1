using System.Numerics;
using RecompOne.Runtime;
using RecompOne.Runtime.Memory;

namespace Kf2.Remaster;

/// <summary>One polygon of a tile half's mesh: the half, the mesh it drew, and the
/// face's index in that mesh's face list.</summary>
public readonly record struct FaceRef(TileKey Tile, int Mesh, int Face)
{
    public override string ToString() => $"{Tile}:{Face}";
}

/// <summary>
/// Tile faces: which face of a half's mesh is being assembled, the frame's triangles
/// by face (for the pick), and the meshes themselves. A face is its index in the
/// mesh's face list, which every assembler walks in order; the subdivider writes each
/// source face's pieces contiguously, four for a quad or a triangle and one for
/// anything else, so its output maps back by counting. See "A tile half is a whole
/// mesh, and a face is the key under it" in docs/REMASTER.md.
/// </summary>
public static class Faces
{
    const uint ModelTable = 0x8018E19C;

    /// <summary>Whether anything wants the face index; set once a frame by <see cref="Host"/>.</summary>
    public static bool Wanted;

    /// <summary>Whether the frame's triangles are recorded for the pick.</summary>
    public static bool Recording;

    /// <summary>The source face being assembled, or -1.</summary>
    public static int Current { get; private set; } = -1;

    /// <summary>Subdivided calls whose output did not count out to the source.</summary>
    public static long MapRefused;

    static int[] _map = new int[256];
    static int _mapN = -1;

    // ---- the face being assembled ---------------------------------------------

    /// <summary>PolyAssembler's face loops, before each face.</summary>
    public static void Enter(bool subdivided, int index)
    {
        if (TileWalk.CurrentRecord == 0 || PolyAssembler.InModel) { Current = -1; return; }
        int f = index;
        if (subdivided) f = _mapN >= 0 && index < _mapN ? _map[index] : -1;
        Current = f;
        if (Surfaces.PerFace) PolyAssembler.TileMaterial = Surfaces.Face(f);
        if (FaceProbe.On) FaceProbe.Enter(f);
    }

    public static void LeaveHalf() => Current = -1;

    static bool Splits(uint cmd) => cmd is 0x2C or 0x2E or 0x24 or 0x26;

    static uint Next(IMemory m, uint f) => f + 4u + ((m.ReadU32(f) >> 6) & 0x3FCu);

    /// <summary>After func_80030C94 built <paramref name="mesh"/> from <paramref name="model"/>.
    /// A count that does not come out is refused whole: nothing is guessed.</summary>
    public static void Subdivided(PSMemory mem, uint model, uint mesh)
    {
        _mapN = -1;
        uint table = mem.ReadU32(ModelTable);
        uint sh = table + 0xCu + model * 28u;
        uint sf = table + 0xCu + mem.ReadU32(sh + 0x10u);
        int sn = (int)mem.ReadU32(sh + 0x14u);
        int on = (int)mem.ReadU32(mesh + 0xCu + 0x14u);
        if (_map.Length < on) _map = new int[Math.Max(on, _map.Length * 2)];
        int o = 0;
        for (int s = 0; s < sn; s++, sf = Next(mem, sf))
        {
            int k = Splits(mem.ReadU32(sf) >> 24) ? 4 : 1;
            if (o + k > on) { MapRefused++; return; }
            for (int q = 0; q < k; q++) _map[o++] = s;
        }
        if (o != on) { MapRefused++; return; }
        _mapN = on;
    }

    public static void SubdividedDone() => _mapN = -1;

    // ---- the frame's triangles ------------------------------------------------------

    public struct Tri
    {
        public float X0, Y0, Z0, X1, Y1, Z1, X2, Y2, Z2;
        public uint Rec;     // 0 for a model's triangle
        public int Mesh, Face;
        public byte Label;   // the material it was sealed with
        public TexKey Tex;   // the art it draws, or default
        public int Model;    // a model's id, or -1 (a tile, or a model the C# walk did not submit)
        public ModelKind Kind;
    }

    static List<Tri> _cur = new(), _last = new();

    /// <summary>The last complete frame's triangles, in the order they were sealed.</summary>
    public static IReadOnlyList<Tri> Last => _last;

    /// <summary>The start of each tile walk from the real camera.</summary>
    public static void FrameStart()
    {
        if (PlanarWalk.Mirroring) return;
        (_last, _cur) = (_cur, _last);
        _cur.Clear();
        if (FaceProbe.On) FaceProbe.FrameDone();
    }

    /// <summary>SealDepth: a recorded packet, with its corners from the packet itself.</summary>
    public static void Seal(PSMemory mem, uint pkt, uint last, in GtePacketDepth.Rec r)
    {
        if (PlanarWalk.Mirroring) return;
        // The corners' stride: 8 for the flat packets, 0xC for the shaded ones. A clipped
        // fan clears Z3, so a 0x20 packet with a fourth depth is a flat quad.
        int n = last == 0x2Cu || (last == 0x20u && r.Z3 > 0f) ? 4 : 3;
        uint stride = (last - 8u) / (uint)(n - 1);
        Span<float> x = stackalloc float[4], y = stackalloc float[4];
        for (int i = 0; i < n; i++)
        {
            uint w = mem.ReadU32(pkt + 8u + stride * (uint)i);
            x[i] = (short)w; y[i] = (short)(w >> 16);
        }
        bool tile = TileWalk.CurrentRecord != 0 && !PolyAssembler.InModel;
        uint rec = tile ? TileWalk.CurrentRecord : 0u;
        int mesh = tile ? mem.ReadU8(rec) : -1;
        int face = tile ? Current : -1;
        byte label = r.Material;
        TextureKeys.OfPacket(mem, pkt, r.Cmd, out var tex);
        int model = PolyAssembler.InModel ? ModelWalk.SubmitModel : -1;
        var kind = ModelWalk.SubmitKind;
        _cur.Add(new Tri { X0 = x[0], Y0 = y[0], Z0 = r.Z0, X1 = x[1], Y1 = y[1], Z1 = r.Z1, X2 = x[2], Y2 = y[2], Z2 = r.Z2,
                           Rec = rec, Mesh = mesh, Face = face, Label = label, Model = model, Kind = kind, Tex = tex });
        if (n == 4)
            _cur.Add(new Tri { X0 = x[1], Y0 = y[1], Z0 = r.Z1, X1 = x[3], Y1 = y[3], Z1 = r.Z3, X2 = x[2], Y2 = y[2], Z2 = r.Z2,
                               Rec = rec, Mesh = mesh, Face = face, Label = label, Model = model, Kind = kind, Tex = tex });
    }

    /// <summary>The view depth of a triangle at a point, or +inf when it does not cover it.</summary>
    public static float DepthAt(in Tri t, float px, float py)
    {
        float d = (t.X1 - t.X0) * (t.Y2 - t.Y0) - (t.X2 - t.X0) * (t.Y1 - t.Y0);
        if (MathF.Abs(d) < 1e-3f) return float.PositiveInfinity;
        float a = ((t.X1 - px) * (t.Y2 - py) - (t.X2 - px) * (t.Y1 - py)) / d;
        float b = ((t.X2 - px) * (t.Y0 - py) - (t.X0 - px) * (t.Y2 - py)) / d;
        float c = 1f - a - b;
        if (a < 0f || b < 0f || c < 0f) return float.PositiveInfinity;
        // 1/z is what is affine on the screen.
        return 1f / (a / t.Z0 + b / t.Z1 + c / t.Z2);
    }

    /// <summary>How far behind the nearest surface another still counts as the same
    /// surface: overlapping meshes lie about a unit apart, and which of them the depth
    /// test keeps changes per pixel.</summary>
    public static float Coplanar(float z) => 2f + z * 0.002f;

    /// <summary>The index in <see cref="Last"/> of the nearest triangle at a game pixel, or -1.</summary>
    public static int Nearest(Vector2 p, out float z)
    {
        int best = -1;
        z = float.PositiveInfinity;
        for (int i = 0; i < _last.Count; i++)
        {
            float d = DepthAt(_last[i], p.X, p.Y);
            if (d < z) { z = d; best = i; }
        }
        return best;
    }

    /// <summary>
    /// The tile faces under a game pixel: the nearest, and every other within the
    /// coplanar tolerance of it. When the nearest is a model's, that model instead, in
    /// <paramref name="model"/>. Null with a reason when there is nothing to pick: no
    /// triangle, a model the C# walk did not submit, or a face the subdivider could not
    /// map.
    /// </summary>
    public static List<FaceRef>? PickAt(Vector2 p, out ModelKey? model, out string? why) => PickAt(p, out model, out why, out _);

    /// <summary><see cref="PickAt(Vector2, out ModelKey?, out string?)"/>, and the art the
    /// nearest triangle draws, if it is textured.</summary>
    public static List<FaceRef>? PickAt(Vector2 p, out ModelKey? model, out string? why, out TexKey? texture)
    {
        why = null;
        model = null;
        texture = null;
        if (_last.Count == 0) { why = "no triangles recorded (is the editor open?)"; return null; }
        int n = Nearest(p, out float z);
        if (n < 0) { why = "nothing drawn there"; return null; }
        if (_last[n].Tex.Index != 0) texture = _last[n].Tex;
        if (_last[n].Rec == 0)
        {
            if (_last[n].Model >= 0) model = new ModelKey(Identity.Area, _last[n].Kind, _last[n].Model);
            else why = "a model the object walk did not name is in front";
            return null;
        }
        var list = new List<FaceRef>();
        float limit = z + Coplanar(z);
        foreach (var t in _last)
        {
            if (t.Rec == 0 || t.Face < 0 || DepthAt(t, p.X, p.Y) > limit) continue;
            if (!Identity.FromRecord(t.Rec, out int x, out int zz, out int half)) continue;
            var f = new FaceRef(new TileKey(Identity.Area, x, zz, half), t.Mesh, t.Face);
            if (!list.Contains(f)) list.Add(f);
        }
        if (list.Count == 0) why = "a face the subdivider could not map";
        return list.Count == 0 ? null : list;
    }

    // ---- the meshes -------------------------------------------------------------

    /// <summary>One face of a mesh as the model table holds it.</summary>
    public readonly record struct MeshFace(uint Cmd, ushort Clut, ushort Tpage, int[] Verts, uint Rect = 0);

    /// <summary>The tile meshes' table. The pointer at <c>ModelTable</c> names it only
    /// while the tile walk runs -- the object walk points it at the models' -- so the
    /// walk notes it for every half, and a mesh is read from what it noted.</summary>
    public static uint TileTable { get; private set; }

    /// <summary>Bumped when <see cref="TileTable"/> moves.</summary>
    public static int TableSerial { get; private set; }

    public static void NoteTable(uint table)
    {
        if (table == TileTable) return;
        TileTable = table;
        TableSerial++;
    }

    static int _cacheSettle = -1, _cacheTable = -1;
    static readonly Dictionary<int, MeshFace[]> _meshes = new();
    static readonly Dictionary<int, string> _hashes = new();

    static void Validate()
    {
        if (_cacheSettle == Identity.Settles && _cacheTable == TableSerial) return;
        _cacheSettle = Identity.Settles;
        _cacheTable = TableSerial;
        _meshes.Clear();
        _hashes.Clear();
    }

    /// <summary>A model byte a half can draw (240 and up draw nothing). The table's +4
    /// word is the far-model limit, not a count, so it is not a bound.</summary>
    static bool Drawable(int model) => model is >= 0 and < 240;

    /// <summary>A mesh's faces, or null past the table.</summary>
    public static MeshFace[]? Mesh(IMemory m, int model)
    {
        Validate();
        if (_meshes.TryGetValue(model, out var cached)) return cached;
        if (!Drawable(model) || TileTable == 0) return null;
        uint table = TileTable;
        uint h = table + 0xCu + (uint)model * 28u;
        uint f = table + 0xCu + m.ReadU32(h + 0x10u);
        int n = (int)m.ReadU32(h + 0x14u);
        if (n is < 0 or > 4096) return null;
        var faces = new MeshFace[n];
        for (int i = 0; i < n; i++, f = Next(m, f))
        {
            uint cmd = m.ReadU32(f) >> 24;
            uint type = cmd & 0xFDu;
            int corners = type == 0x2Cu ? 4 : type == 0x24u ? 3 : 0;
            uint at = f + 4u + (corners == 4 ? 0x12u : 0x0Eu);
            var v = new int[corners];
            for (int k = 0; k < corners; k++) v[k] = m.ReadU16(at + 2u * (uint)k);
            // The face's UV rectangle, u0 | v0 << 8 | u1 << 16 | v1 << 24, from the UV
            // halfwords at +4, +8, +0xC and +0x10.
            uint rect = 0;
            if (corners != 0)
            {
                int u0 = 255, v0 = 255, u1 = 0, v1 = 0;
                for (int k = 0; k < corners; k++)
                {
                    uint uv = m.ReadU16(f + 4u + 4u * (uint)k);
                    int u = (int)(uv & 0xFF), vv = (int)(uv >> 8);
                    u0 = Math.Min(u0, u); v0 = Math.Min(v0, vv); u1 = Math.Max(u1, u); v1 = Math.Max(v1, vv);
                }
                rect = (uint)u0 | (uint)v0 << 8 | (uint)u1 << 16 | (uint)v1 << 24;
            }
            faces[i] = corners == 0 ? new MeshFace(cmd, 0, 0, v)
                                    : new MeshFace(cmd, m.ReadU16(f + 6u), m.ReadU16(f + 10u), v, rect);
        }
        _meshes[model] = faces;
        return faces;
    }

    /// <summary>
    /// What an authored face list is checked against: the face count, and each face's
    /// command, length and corners -- the structure a face index means, and neither
    /// its texture nor where its vertices are. A hash, not a copy.
    /// </summary>
    public static string? MeshHash(IMemory m, int model)
    {
        Validate();
        if (_hashes.TryGetValue(model, out var cached)) return cached;
        if (!Drawable(model) || TileTable == 0) return null;
        uint table = TileTable;
        uint h = table + 0xCu + (uint)model * 28u;
        uint f = table + 0xCu + m.ReadU32(h + 0x10u);
        int n = (int)m.ReadU32(h + 0x14u);
        if (n is < 0 or > 4096) return null;
        ulong x = 0xCBF29CE484222325UL;
        void Mix(uint v) { for (int b = 0; b < 4; b++) { x ^= (byte)(v >> (b * 8)); x *= 0x100000001B3UL; } }
        Mix((uint)n);
        for (int i = 0; i < n; i++, f = Next(m, f))
        {
            uint word = m.ReadU32(f);
            Mix(word >> 24);
            Mix((word >> 8) & 0xFFu);
            uint type = (word >> 24) & 0xFDu;
            int corners = type == 0x2Cu ? 4 : type == 0x24u ? 3 : 0;
            uint at = f + 4u + (corners == 4 ? 0x12u : 0x0Eu);
            for (int k = 0; k < corners; k++) Mix(m.ReadU16(at + 2u * (uint)k));
        }
        var text = x.ToString("x16");
        _hashes[model] = text;
        return text;
    }

    /// <summary>The faces of <paramref name="model"/> reachable from <paramref name="start"/>
    /// through shared edges, optionally only across faces with the same texture.</summary>
    public static List<int> Connected(IMemory m, int model, int start, bool sameTexture)
    {
        var faces = Mesh(m, model);
        var result = new List<int>();
        if (faces == null || (uint)start >= (uint)faces.Length) return result;
        var seen = new bool[faces.Length];
        var queue = new Queue<int>();
        queue.Enqueue(start);
        seen[start] = true;
        var s = faces[start];
        while (queue.Count > 0)
        {
            int i = queue.Dequeue();
            result.Add(i);
            for (int j = 0; j < faces.Length; j++)
            {
                if (seen[j]) continue;
                if (sameTexture && (faces[j].Clut != s.Clut || faces[j].Tpage != s.Tpage)) continue;
                if (Shared(faces[i].Verts, faces[j].Verts) < 2) continue;
                seen[j] = true;
                queue.Enqueue(j);
            }
        }
        result.Sort();
        return result;
    }

    /// <summary>Every face of <paramref name="model"/> with the texture <paramref name="of"/> has.</summary>
    public static List<int> SameTexture(IMemory m, int model, int of)
    {
        var faces = Mesh(m, model);
        var result = new List<int>();
        if (faces == null || (uint)of >= (uint)faces.Length) return result;
        for (int j = 0; j < faces.Length; j++)
            if (faces[j].Verts.Length > 0 && faces[j].Clut == faces[of].Clut && faces[j].Tpage == faces[of].Tpage)
                result.Add(j);
        return result;
    }

    static int Shared(int[] a, int[] b)
    {
        int n = 0;
        foreach (int v in a)
            if (Array.IndexOf(b, v) >= 0) n++;
        return n;
    }
}
