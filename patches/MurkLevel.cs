using RecompOne.Runtime;
using RecompOne.Runtime.Memory;

namespace Kf2;

/// <summary>
/// Where the water is, for the murk: per map tile, the world Y of its water's surface,
/// from the water faces of the tile's meshes. The prim shader fades whatever it draws
/// below that level by the view ray's run through the water to it, so the murk is on
/// what is seen through the water and the water's own ripples stay on top, and it is
/// fixed to the world. Water with nothing drawn under it (the open sea past the
/// shelves, which the map gives no floor) is the reflection pass's.
///
///     KF2_MURK_PROBE=1   a line every 2 s: water tiles, the build, and the levels round the camera
///
/// Rebuilt when the map, the bank or the water's rects change. See "Murky water" in
/// docs/RENDERING.md.
/// </summary>
public static class MurkLevel
{
    const uint MapBase = 0x801C8484;
    const uint Banks = 0x8018E18C;
    const int Span = WaterMurk.GridSpan;

    static ulong _hash;
    static bool _probe;
    static int _water;
    static double _buildMs;
    static long _lastReport;

    public static void Configure(string? probe) => _probe = probe is not (null or "" or "0");

    /// <summary>From the start of <see cref="TileWalk"/>'s sweep, on the frame's own
    /// walk: the camera it draws with, and the grid if the map changed.</summary>
    public static void AtWalk(PSMemory mem)
    {
        if (!WaterMurk.Enabled) return;
        var v = RetainedMap.ReadView(mem);
        var o = WaterMurk.View;
        o[0] = v.R00; o[1] = v.R01; o[2] = v.R02;
        o[3] = v.R10; o[4] = v.R11; o[5] = v.R12;
        o[6] = v.R20; o[7] = v.R21; o[8] = v.R22;
        o[9] = (float)v.CamX; o[10] = (float)v.CamY; o[11] = (float)v.CamZ;
        o[12] = v.Tx; o[13] = v.Ty; o[14] = v.Tz;
        WaterMurk.ViewSet = true;
        WaterMurk.ViewGen++;

        ulong h = Hash(mem);
        if (h != _hash)
        {
            _hash = h;
            var start = System.Diagnostics.Stopwatch.GetTimestamp();
            Build(mem);
            _buildMs = System.Diagnostics.Stopwatch.GetElapsedTime(start).TotalMilliseconds;
        }
        if (_probe) Report(mem);
    }

    public static void Forget()
    {
        _hash = 0;
        WaterMurk.Level = null;
        WaterMurk.ViewSet = false;
    }

    static ulong Hash(PSMemory mem)
    {
        ulong h = 0xCBF29CE484222325ul;
        void Mix(uint v) { h = (h ^ v) * 0x100000001B3ul; }
        Mix(mem.ReadU32(Banks));
        Mix((uint)SurfaceMaterial.RectN);
        for (int i = 0; i < SurfaceMaterial.RectN; i++)
        {
            ref var r = ref SurfaceMaterial.Rects[i];
            Mix((uint)r.X); Mix((uint)r.Y); Mix((uint)r.W); Mix((uint)r.H); Mix(r.Material);
        }
        var ram = mem.Ram;
        int at = (int)(MapBase & (uint)(ram.Length - 1));
        for (int i = 0; i < Span * Span; i++, at += 10)
        {
            Mix((uint)(ram[at] | ram[at + 1] << 8 | (ram[at + 2] & 3) << 16));
            Mix((uint)(ram[at + 5] | ram[at + 6] << 8 | (ram[at + 7] & 3) << 16));
        }
        return h;
    }

    static void Build(PSMemory mem)
    {
        uint table = mem.ReadU32(Banks);
        if (table == 0 || SurfaceMaterial.RectN == 0) { WaterMurk.Level = null; return; }

        // Pass one: each tile's water level, Y being down.
        var level = new float[Span * Span];
        var levelN = new int[Span * Span];
        var meshes = new Dictionary<uint, MeshData?>();
        for (int i = 0; i < Span * Span * 2; i++)
        {
            uint rec = MapBase + (uint)(i >> 1) * 10u + (uint)(i & 1) * 5u;
            uint model = mem.ReadU8(rec);
            if (model >= 240) continue;
            var mesh = MeshOf(mem, table, model, meshes);
            if (mesh is not { } m || m.Water.Length == 0) continue;
            int tile = i >> 1, baseY = -mem.ReadU8(rec + 1u) * 128;
            foreach (int v in m.Water) { level[tile] += baseY + m.Verts[v * 4 + 1]; levelN[tile]++; }
        }

        var grid = new float[Span * Span * 2];
        _water = 0;
        for (int t = 0; t < Span * Span; t++)
        {
            if (levelN[t] == 0) continue;
            grid[t * 2] = level[t] / levelN[t];
            grid[t * 2 + 1] = 1f;
            _water++;
        }
        // A tile beside water takes its level too: a shore's or a platform's faces
        // below the surface lie in the land tile, which has no water of its own.
        for (int t = 0; t < Span * Span; t++)
        {
            if (levelN[t] != 0) continue;
            int x = t % Span, z = t / Span, n = 0;
            float sum = 0f;
            for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++)
            {
                int nx = x + dx, nz = z + dz;
                if (nx < 0 || nz < 0 || nx >= Span || nz >= Span || levelN[nz * Span + nx] == 0) continue;
                sum += grid[(nz * Span + nx) * 2];
                n++;
            }
            if (n == 0) continue;
            grid[t * 2] = sum / n;
            grid[t * 2 + 1] = 1f;
        }
        WaterMurk.Level = grid;
        WaterMurk.LevelGen++;
    }

    /// <summary>A model's vertices (x, y, z, pad) and the indices of those its water
    /// faces use.</summary>
    sealed record MeshData(short[] Verts, int[] Water);

    /// <summary>A model's mesh, once per model per build.</summary>
    static MeshData? MeshOf(PSMemory mem, uint table, uint model, Dictionary<uint, MeshData?> known)
    {
        if (known.TryGetValue(model, out var k)) return k;
        uint header = table + model * 28u + 0xCu;
        uint ramEnd = 0x80000000u + (uint)mem.Ram.Length;
        uint verts = table + mem.ReadU32(header) + 0xCu;
        uint vcount = mem.ReadU32(header + 4u);
        uint face = table + mem.ReadU32(header + 0x10u) + 0xCu;
        uint count = mem.ReadU32(header + 0x14u);
        if (verts < 0x80000000u || verts + vcount * 8u > ramEnd || face < 0x80000000u || face >= ramEnd
            || count > 4096u || vcount > 4096u)
        {
            known[model] = null;
            return null;
        }
        var v = new short[vcount * 4];
        for (uint i = 0; i < vcount * 4; i++) v[i] = (short)mem.ReadU16(verts + i * 2u);
        var water = new HashSet<int>();
        for (uint f = 0; f < count; f++)
        {
            uint word = mem.ReadU32(face);
            uint at = face + 4u;
            uint type = (word >> 24) & 0xFDu;
            int corners = type == 0x2Cu ? 4 : type == 0x24u ? 3 : 0;
            if (corners != 0)
            {
                uint idx = at + (corners == 4 ? 0x12u : 0x0Eu);
                Span<int> o = stackalloc int[4];
                bool ok = true;
                for (int c = 0; c < corners; c++)
                {
                    o[c] = mem.ReadU16(idx + (uint)c * 2u) / 8;
                    ok &= o[c] < vcount;
                }
                if (ok && IsWater(mem, word, at, corners))
                    for (int c = 0; c < corners; c++) water.Add(o[c]);
            }
            face = at + ((word >> 6) & 0x3FCu);
        }
        var r = new MeshData(v, water.ToArray());
        known[model] = r;
        return r;
    }

    /// <summary>Semi-transparent, in an averaging blend, textured from a rect the
    /// reflections publish as water, as <see cref="SurfaceMaterial.Classify"/> takes it.</summary>
    static bool IsWater(PSMemory mem, uint word, uint at, int corners)
    {
        if (((word >> 24) & 2u) == 0) return false;
        int tpage = mem.ReadU16(at + 6u) & 0x1FF;
        int blend = (tpage >> 5) & 3;
        if (blend is not (0 or 3)) return false;
        int u0 = 255, v0 = 255, u1 = 0, v1 = 0;
        for (int k = 0; k < corners; k++)
        {
            uint uv = mem.ReadU16(at + (uint)k * 4u);
            int u = (int)(uv & 0xFF), vv = (int)(uv >> 8);
            u0 = Math.Min(u0, u); v0 = Math.Min(v0, vv); u1 = Math.Max(u1, u); v1 = Math.Max(v1, vv);
        }
        int mode = (tpage >> 7) & 3;
        int div = mode == 0 ? 4 : mode == 1 ? 2 : 1;
        int x0 = (tpage & 0xF) * 64 + u0 / div, x1 = (tpage & 0xF) * 64 + u1 / div + 1;
        int y0 = ((tpage >> 4) & 1) * 256 + v0, y1 = ((tpage >> 4) & 1) * 256 + v1 + 1;
        for (int i = 0; i < SurfaceMaterial.RectN; i++)
        {
            ref var r = ref SurfaceMaterial.Rects[i];
            if (r.Material != SurfaceMaterial.Water) continue;
            if (x0 < r.X + r.W && x1 > r.X && y0 < r.Y + r.H && y1 > r.Y) return true;
        }
        return false;
    }

    static void Report(PSMemory mem)
    {
        long now = Environment.TickCount64;
        if (now - _lastReport < 2000) return;
        _lastReport = now;
        var grid = WaterMurk.Level;
        var v = WaterMurk.View;
        int px = (int)Math.Floor(v[9] / WaterMurk.TileUnits), pz = (int)Math.Floor(v[11] / WaterMurk.TileUnits);
        var sb = new System.Text.StringBuilder();
        if (grid != null)
            for (int z = Math.Min(Span - 1, pz + 3); z >= Math.Max(0, pz - 3); z--)
            {
                sb.Append($"\n[KF2] murk level z{z,2}:");
                for (int x = Math.Max(0, px - 5); x <= Math.Min(Span - 1, px + 5); x++)
                {
                    int t = z * Span + x;
                    sb.Append(grid[t * 2 + 1] > 0f ? $" {grid[t * 2],6:F0}" : "      -");
                }
            }
        Console.WriteLine($"[KF2] murk level: {_water} water tile(s); built in {_buildMs:F2} ms; camera tile {px},{pz} at Y {v[10]:F0}{sb}");
    }
}
