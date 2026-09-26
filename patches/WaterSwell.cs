using System.Runtime.InteropServices;
using RecompOne.Runtime;
using RecompOne.Runtime.Memory;

namespace Kf2;

/// <summary>
/// The swell half of <see cref="Waves"/>: the water's own vertices lifted and lowered
/// by a slow wave field in world space, before any of the game's routines sees them.
///
/// **Where it goes in.** A half's mesh is read in three places: the vertex transforms
/// (through <c>VertexBase</c>, which <c>func_8002E1F0</c> sets from the bank), the
/// view-space clipper (the same), and the subdivider <c>func_80030C94</c>, which reads
/// the bank's header directly. So the header's vertex offset is pointed at a displaced
/// copy in guest RAM (<see cref="PrimBuffer.WaveScratch"/>) for the length of the half
/// and put back after it, and every one of them -- and everything downstream: the
/// depth records, the sub-pixel fractions, the Z-buffer, the surface buffer the
/// reflections read -- draws the same moved surface. A subdivided half's midpoints
/// are made from moved corners, so near and far tiles agree along every edge.
///
/// **What may move.** The area's water is one quad a tile for open water and a few
/// triangles a tile at a pool's edge, so a vertex is a tile corner at best: the swell
/// is long, a crest several tiles apart. A vertex moves only if it is interior to the
/// water -- not on an edge only one water face has (the rim), and not where any
/// other face of the area has a vertex -- so a shore, a wall or a bank is never
/// pulled and nothing opens a crack. Worked out per area from the map, when the map,
/// the bank or the water's rects change.
/// </summary>
public static class WaterSwell
{
    const uint MapBase = 0x801C8484;
    /// <summary>The map's model bank, and the table pointer the assemblers read, which
    /// holds the map's bank only while the tile walk runs.</summary>
    const uint Banks = 0x8018E18C;
    const uint ModelTable = 0x8018E19C;
    const uint VertexBase = 0x8018EAA0;

    /// <summary>A mesh's water: the byte offsets of the vertices its water faces use,
    /// and each water face's outline as offsets, in loop order.</summary>
    sealed class Mesh
    {
        public ushort[] WaterVerts = [];
        public List<ushort[]> WaterFaces = [];
        public ushort[] OtherVerts = [];
    }

    static readonly Dictionary<uint, Mesh> _meshes = new();
    static readonly HashSet<long> _free = new();
    static ulong _hash;
    static uint _table;
    static bool _ready;

    // What the last build found, and what the frames since the probe's last line moved.
    static int _waterPositions, _rimPositions, _sharedPositions;
    static long _halves, _moved, _pinned;
    static double _buildMs;

    public static void Forget() { _hash = 0; _ready = false; }

    // ---- once a walk: the area's water ---------------------------------------------

    public static void AtWalk(PSMemory mem)
    {
        if (Waves.Swell <= 0f) return;
        ulong h = Hash(mem);
        if (h == _hash) return;
        _hash = h;
        var start = System.Diagnostics.Stopwatch.GetTimestamp();
        Build(mem);
        _buildMs = System.Diagnostics.Stopwatch.GetElapsedTime(start).TotalMilliseconds;
    }

    /// <summary>The bank, the water's rects, and each half's model, height and turn.</summary>
    static ulong Hash(PSMemory mem)
    {
        ulong h = 0xCBF29CE484222325ul;
        void Mix(uint v) { h = (h ^ v) * 0x100000001B3ul; }
        Mix(mem.ReadU32(Banks));
        Mix((uint)WaterWaves.RectN);
        for (int i = 0; i < WaterWaves.RectN * 4; i++) Mix((uint)(int)WaterWaves.Rects[i]);
        var ram = mem.Ram;
        int at = (int)(MapBase & (uint)(ram.Length - 1));
        for (int i = 0; i < 6400; i++, at += 10)
        {
            Mix((uint)(ram[at] | ram[at + 1] << 8 | (ram[at + 2] & 3) << 16));
            Mix((uint)(ram[at + 5] | ram[at + 6] << 8 | (ram[at + 7] & 3) << 16));
        }
        return h;
    }

    static void Build(PSMemory mem)
    {
        _meshes.Clear();
        _free.Clear();
        _ready = false;
        _table = mem.ReadU32(Banks);
        if (_table == 0 || WaterWaves.RectN == 0) return;

        // Every water position, and how many water faces use each edge between two.
        var water = new HashSet<long>();
        var edges = new Dictionary<(long, long), int>();
        var halves = new List<(uint Rec, Mesh Mesh)>();
        for (int i = 0; i < 12800; i++)
        {
            uint rec = MapBase + (uint)(i >> 1) * 10u + (uint)(i & 1) * 5u;
            uint model = mem.ReadU8(rec);
            if (model >= 240) continue;
            var mesh = MeshOf(mem, model);
            if (mesh == null) continue;
            halves.Add((rec, mesh));
            if (mesh.WaterFaces.Count == 0) continue;
            var place = Place.Of(mem, rec, _table, model);
            foreach (var face in mesh.WaterFaces)
                for (int k = 0; k < face.Length; k++)
                {
                    long a = place.Key(mem, face[k]), b = place.Key(mem, face[(k + 1) % face.Length]);
                    water.Add(a);
                    var e = a < b ? (a, b) : (b, a);
                    edges[e] = edges.GetValueOrDefault(e) + 1;
                }
        }

        var rim = new HashSet<long>();
        foreach (var (e, n) in edges)
            if (n == 1) { rim.Add(e.Item1); rim.Add(e.Item2); }

        // Any other face's vertex at a water position holds it.
        var shared = new HashSet<long>();
        foreach (var (rec, mesh) in halves)
        {
            if (mesh.OtherVerts.Length == 0) continue;
            var place = Place.Of(mem, rec, _table, mem.ReadU8(rec));
            foreach (ushort v in mesh.OtherVerts)
            {
                long k = place.Key(mem, v);
                if (water.Contains(k)) shared.Add(k);
            }
        }

        foreach (long k in water)
            if (!rim.Contains(k) && !shared.Contains(k)) _free.Add(k);
        _waterPositions = water.Count;
        _rimPositions = rim.Count;
        _sharedPositions = shared.Count;
        _ready = true;
    }

    /// <summary>A mesh's faces sorted into water and not, once per model per build.</summary>
    static Mesh? MeshOf(PSMemory mem, uint model)
    {
        if (_meshes.TryGetValue(model, out var known)) return known;
        uint header = _table + model * 28u + 0xCu;
        uint ramEnd = 0x80000000u + (uint)mem.Ram.Length;
        uint verts = _table + mem.ReadU32(header) + 0xCu;
        uint face = _table + mem.ReadU32(header + 0x10u) + 0xCu;
        uint count = mem.ReadU32(header + 0x14u);
        if (verts < 0x80000000u || verts >= ramEnd || face < 0x80000000u || face >= ramEnd || count > 4096u
            || mem.ReadU32(header + 4u) * 8u > PrimBuffer.WaveBytes)
        {
            _meshes[model] = null!;
            return null;
        }

        var mesh = new Mesh();
        var waterVerts = new HashSet<ushort>();
        var otherVerts = new HashSet<ushort>();
        for (uint f = 0; f < count; f++)
        {
            uint word = mem.ReadU32(face);
            uint at = face + 4u;
            uint type = (word >> 24) & 0xFDu;
            int corners = type == 0x2Cu ? 4 : type == 0x24u ? 3 : 0;
            if (corners != 0)
            {
                uint idx = at + (corners == 4 ? 0x12u : 0x0Eu);
                var offs = new ushort[corners];
                for (int k = 0; k < corners; k++) offs[k] = (ushort)mem.ReadU16(idx + (uint)k * 2u);
                if (IsWater(mem, word, at, corners))
                {
                    // A quad is the strip 0,1,2 / 1,3,2: its outline is 0,1,3,2.
                    mesh.WaterFaces.Add(corners == 4 ? [offs[0], offs[1], offs[3], offs[2]] : offs);
                    foreach (ushort o in offs) waterVerts.Add(o);
                }
                else foreach (ushort o in offs) otherVerts.Add(o);
            }
            face = at + ((word >> 6) & 0x3FCu);
        }
        mesh.WaterVerts = [.. waterVerts];
        mesh.OtherVerts = [.. otherVerts];
        _meshes[model] = mesh;
        return mesh;
    }

    /// <summary>Semi-transparent, in an averaging blend, textured from a water rect.</summary>
    static bool IsWater(PSMemory mem, uint word, uint at, int corners)
    {
        if (((word >> 24) & 2u) == 0) return false;
        uint tpage = mem.ReadU16(at + 6u) & 0x1FFu;
        uint blend = (tpage >> 5) & 3u;
        if (blend is not (0u or 3u)) return false;
        int u0 = 255, v0 = 255, u1 = 0, v1 = 0;
        for (int k = 0; k < corners; k++)
        {
            uint uv = mem.ReadU16(at + (uint)k * 4u);
            int u = (int)(uv & 0xFF), v = (int)(uv >> 8);
            u0 = Math.Min(u0, u); v0 = Math.Min(v0, v); u1 = Math.Max(u1, u); v1 = Math.Max(v1, v);
        }
        return Waves.InRect(tpage, u0, v0, u1, v1);
    }

    /// <summary>Where a half puts its mesh: the tile's centre, the height times -128
    /// and the quarter-turn, as <c>func_80031950</c> and <see cref="RetainedMap"/> place it.</summary>
    readonly struct Place
    {
        public readonly int X, Y, Z;
        public readonly uint Rot, Verts;

        Place(int x, int y, int z, uint rot, uint verts) { X = x; Y = y; Z = z; Rot = rot; Verts = verts; }

        public static Place Of(PSMemory mem, uint rec, uint table, uint model)
        {
            uint off = rec - MapBase;
            int tz = (int)(off / 800u), tx = (int)(off % 800u / 10u);
            uint header = table + model * 28u + 0xCu;
            return new Place(tx * 2048 + 1024, -(int)mem.ReadU8(rec + 1u) * 128, tz * 2048 + 1024,
                             mem.ReadU8(rec + 2u) & 3u, table + mem.ReadU32(header) + 0xCu);
        }

        public void World(PSMemory mem, ushort v, out int x, out int y, out int z)
        {
            uint a = Verts + v;
            int lx = (short)mem.ReadU16(a), ly = (short)mem.ReadU16(a + 2u), lz = (short)mem.ReadU16(a + 4u);
            (int ox, int oz) = Rot switch
            {
                1 => (lz, -lx),
                2 => (-lx, -lz),
                3 => (-lz, lx),
                _ => (lx, lz),
            };
            x = X + ox; y = Y + ly; z = Z + oz;
        }

        public long Key(PSMemory mem, ushort v)
        {
            World(mem, v, out int x, out int y, out int z);
            return ((long)(x + (1 << 19)) << 41) | ((long)(y + (1 << 20)) << 20) | (uint)(z + (1 << 19));
        }
    }

    // ---- per half ------------------------------------------------------------------

    static uint _savedHeader, _savedOffset;
    static readonly List<(ushort Offset, short Dy)> _moves = new();

    /// <summary>The swell's height at a world position, up being negative Y. Three
    /// long waves at unrelated headings; each one's period goes as the root of its
    /// length, as deep water's does.</summary>
    static double Height(double x, double z)
    {
        double t = Waves.Time, len = Waves.SwellSize;
        static double Wave(double x, double z, double dx, double dz, double len, double t, double amp)
            => amp * Math.Sin(2.0 * Math.PI / len * (dx * x + dz * z) - 2.0 * Math.PI / (7.0 * Math.Sqrt(len / 12000.0)) * t);
        return Waves.Swell * (Wave(x, z, 0.87, 0.50, len, t, 0.5)
                            + Wave(x, z, -0.34, 0.94, len * 0.71, t, 0.3)
                            + Wave(x, z, 0.60, -0.80, len * 0.53, t, 0.2));
    }

    /// <summary>From <see cref="TileWalk"/> after <c>func_8002E1F0</c> set the half's
    /// vertex base: point the half's mesh at a moved copy when any of its water may
    /// move. <see cref="Leave"/> puts the bank back.</summary>
    public static void Enter(PSMemory mem, uint rec, uint model)
    {
        if (!_ready || !Waves.Enabled || Waves.Swell <= 0f || TileWalk.Verifying || !PrimBuffer.Relocated) return;
        if (!_meshes.TryGetValue(model, out var mesh) || mesh == null || mesh.WaterVerts.Length == 0) return;
        if (mem.ReadU32(ModelTable) != _table) return;

        var place = Place.Of(mem, rec, _table, model);
        _moves.Clear();
        foreach (ushort v in mesh.WaterVerts)
        {
            place.World(mem, v, out int x, out int y, out int z);
            long key = ((long)(x + (1 << 19)) << 41) | ((long)(y + (1 << 20)) << 20) | (uint)(z + (1 << 19));
            if (!_free.Contains(key)) { _pinned++; continue; }
            _moves.Add((v, (short)Math.Round(-Height(x, z))));
        }
        if (_moves.Count == 0) return;

        uint header = _table + model * 28u + 0xCu;
        uint bytes = mem.ReadU32(header + 4u) * 8u;
        uint scratch = PrimBuffer.WaveScratch;
        if ((scratch & 0x1FFFFFFFu) + bytes > (uint)mem.Ram.Length) return;
        for (uint i = 0; i < bytes; i += 4u) mem.WriteU32(scratch + i, mem.ReadU32(place.Verts + i));
        foreach (var (off, dy) in _moves)
        {
            uint a = scratch + off + 2u;
            mem.WriteU16(a, (ushort)Math.Clamp((short)mem.ReadU16(a) + dy, short.MinValue, short.MaxValue));
        }

        _savedHeader = header;
        _savedOffset = mem.ReadU32(header);
        mem.WriteU32(header, scratch - _table - 0xCu);
        mem.WriteU32(VertexBase, scratch);
        _halves++;
        _moved += _moves.Count;
    }

    public static void Leave(PSMemory mem)
    {
        if (_savedHeader == 0) return;
        mem.WriteU32(_savedHeader, _savedOffset);
        _savedHeader = 0;
    }

    public static string Report(double dt)
    {
        string s = !_ready
            ? "swell: no water"
            : $"swell: {_free.Count} of {_waterPositions} water position(s) free ({_rimPositions} rim, {_sharedPositions} shared; " +
              $"built in {_buildMs:F1} ms), {_halves / dt:F0} halves/s moved, {_moved / dt:F0} vertices/s, {_pinned / dt:F0} held";
        _halves = _moved = _pinned = 0;
        return s;
    }
}
