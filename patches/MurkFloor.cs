using RecompOne.Runtime;
using RecompOne.Runtime.Memory;

namespace Kf2;

/// <summary>
/// The floor under the murk, from the map: where the water has nothing drawn under it
/// (the open sea, the cells along the pier), the murk would otherwise take the sky's
/// full run and go solid. Per tile, the water's level from its water faces and the
/// deepest vertex of the tile's meshes below it; a floorless tile takes the average of
/// its eight neighbours' floors, and one with none stays deep. The reflection pass
/// reads it by world position, so it is fixed to the world as the camera turns.
///
///     KF2_MURK_PROBE=1   a line every 2 s: water tiles, floored, filled, deep, and the grid round the player
///
/// Rebuilt when the map, the bank or the water's rects change. See "Murky water" in
/// docs/RENDERING.md.
/// </summary>
public static class MurkFloor
{
    const uint MapBase = 0x801C8484;
    const uint Banks = 0x8018E18C;
    const int Span = WaterMurk.GridSpan;

    static ulong _hash;
    static bool _probe;
    static int _water, _floored, _filled;
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
        WaterMurk.Grid = null;
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
        if (table == 0 || SurfaceMaterial.RectN == 0) { WaterMurk.Grid = null; return; }

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

        // Pass two: the floor below it, from both halves -- the faces wholly under
        // the surface, each by its mean depth weighed by the area it covers seen from
        // above, so a cliff's wall or a pillar's side counts for nothing.
        var depthSum = new double[Span * Span];
        var areaSum = new double[Span * Span];
        for (int i = 0; i < Span * Span * 2; i++)
        {
            int tile = i >> 1;
            if (levelN[tile] == 0) continue;
            uint rec = MapBase + (uint)(i >> 1) * 10u + (uint)(i & 1) * 5u;
            uint model = mem.ReadU8(rec);
            if (model >= 240) continue;
            if (MeshOf(mem, table, model, meshes) is not { } m) continue;
            float surface = level[tile] / levelN[tile];
            int baseY = -mem.ReadU8(rec + 1u) * 128;
            foreach (var f in m.Floors)
            {
                double y0 = baseY + m.Verts[f.A * 4 + 1] - surface, y1 = baseY + m.Verts[f.B * 4 + 1] - surface,
                       y2 = baseY + m.Verts[f.C * 4 + 1] - surface;
                if (y0 <= 16 || y1 <= 16 || y2 <= 16) continue;
                double ax = m.Verts[f.B * 4] - m.Verts[f.A * 4], az = m.Verts[f.B * 4 + 2] - m.Verts[f.A * 4 + 2];
                double bx = m.Verts[f.C * 4] - m.Verts[f.A * 4], bz = m.Verts[f.C * 4 + 2] - m.Verts[f.A * 4 + 2];
                double area = Math.Abs(ax * bz - az * bx) * 0.5;
                depthSum[tile] += area * (y0 + y1 + y2) / 3.0;
                areaSum[tile] += area;
            }
        }
        var depth = new float[Span * Span];
        for (int t = 0; t < Span * Span; t++)
            if (areaSum[t] > 1e4) depth[t] = (float)(depthSum[t] / areaSum[t]);

        // The grid: a floored tile weighs 1. A tile without a floor, water or not,
        // takes the floors within Reach tiles, the nearer the more, and weighs less
        // the further the nearest is: 1 beside it, none past Reach. So open water past
        // a shelf deepens over a few tiles rather than at the shelf's edge, and
        // floorless water beside a bank or the pier's deck is not faded by it.
        const int Reach = 3;
        var grid = new float[Span * Span * 2];
        _water = _floored = _filled = 0;
        for (int t = 0; t < Span * Span; t++)
        {
            if (levelN[t] > 0) _water++;
            if (depth[t] > 0f) { grid[t * 2] = depth[t]; grid[t * 2 + 1] = 1f; _floored++; }
        }
        for (int z = 0; z < Span; z++)
            for (int x = 0; x < Span; x++)
            {
                int t = z * Span + x;
                if (depth[t] > 0f) continue;
                double sum = 0, wsum = 0, nearest = double.MaxValue;
                for (int dz = -Reach; dz <= Reach; dz++)
                    for (int dx = -Reach; dx <= Reach; dx++)
                    {
                        int nx = x + dx, nz = z + dz;
                        if (nx < 0 || nz < 0 || nx >= Span || nz >= Span) continue;
                        float d = depth[nz * Span + nx];
                        if (d <= 0f) continue;
                        double dist = Math.Sqrt(dx * dx + dz * dz);
                        sum += d / (dist * dist);
                        wsum += 1.0 / (dist * dist);
                        nearest = Math.Min(nearest, dist);
                    }
                double w = Math.Clamp((Reach + 1 - nearest) / Reach, 0.0, 1.0);
                if (wsum <= 0 || w <= 0) continue;
                grid[t * 2] = (float)(sum / wsum * w);
                grid[t * 2 + 1] = (float)w;
                if (levelN[t] > 0) _filled++;
            }
        WaterMurk.Grid = grid;
        WaterMurk.GridGen++;
    }

    /// <summary>A model's vertices (x, y, z, pad), the indices of those its water
    /// faces use, and its other faces as triangles.</summary>
    sealed record MeshData(short[] Verts, int[] Water, (int A, int B, int C)[] Floors);

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
        var floors = new List<(int A, int B, int C)>();
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
                else if (ok)
                {
                    // A quad is the strip 0,1,2 / 1,3,2.
                    floors.Add((o[0], o[1], o[2]));
                    if (corners == 4) floors.Add((o[1], o[3], o[2]));
                }
            }
            face = at + ((word >> 6) & 0x3FCu);
        }
        var r = new MeshData(v, water.ToArray(), floors.ToArray());
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
        var grid = WaterMurk.Grid;
        var v = WaterMurk.View;
        int px = (int)Math.Floor(v[9] / WaterMurk.TileUnits), pz = (int)Math.Floor(v[11] / WaterMurk.TileUnits);
        var sb = new System.Text.StringBuilder();
        if (grid != null)
            for (int z = Math.Min(Span - 1, pz + 3); z >= Math.Max(0, pz - 3); z--)
            {
                sb.Append($"\n[KF2] murk floor z{z,2}:");
                for (int x = Math.Max(0, px - 5); x <= Math.Min(Span - 1, px + 5); x++)
                {
                    int t = z * Span + x;
                    sb.Append(grid[t * 2 + 1] > 0f ? $" {grid[t * 2],5:F0}" : "     -");
                }
            }
        Console.WriteLine($"[KF2] murk floor: {_water} water tile(s), {_floored} floored, {_filled} water filled from a neighbour, " +
                          $"{_water - _floored - _filled} deep; built in {_buildMs:F2} ms; camera tile {px},{pz}{sb}");
    }
}
