using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// Occluders the camera cannot see (<c>0059</c>).
///
///     KF2_AO_WORLD=1           on; unset is off
///     KF2_AO_WORLD_STRENGTH=0.6  how dark the world term goes
///     KF2_AO_WORLD_RADIUS=3072   how far it reaches, in world units (a tile is 2048)
///     KF2_AO_WORLD_PROBE=1       the transform, the grid, and whether either is missing
///     KF2_AO_WORLD_PROBE=2       also the camera's own tile and its neighbours
///
/// **A screen-space pass can only be occluded by what is on screen.** Stand in a
/// corner facing one wall and the wall behind the camera contributes nothing, so
/// the corner is only as dark as the part of it in shot — and the shading changes
/// as the view turns, which is the artefact that reads as the effect being unstable
/// rather than as the room being dark. No radius and no sample count fixes it; it
/// is what the depth buffer *is*.
///
/// The port is not limited to the picture any more. `TileWalk` enumerates the map's
/// cells, `Map` reads the same 80x80 grid for the minimap, and the grid holds the
/// whole area — including the tiles behind the camera. This publishes two things the
/// occlusion pass needs to use it, and nothing else:
///
/// **The transform, and the reason it is exact.** `func_80031950` loads a view
/// matrix from `0x80192E18` before every tile half, and `TileWalk.Place` shows what
/// the game feeds it: `(tx &lt;&lt; 11) - CamWorldX`, a position *already relative to
/// the camera* in world axes. So that matrix is the camera's rotation alone — no
/// translation, no per-object rotation composed into it — and inverting it turns a
/// view position straight back into a world one. Nothing has to be assumed about how
/// the game composes pitch, yaw and roll, which is the part a guess would get wrong.
///
/// **The floor plan.** The grid's height byte is the same one the game turns into a
/// tile's Y (`Elevate`: `(0 - height) &lt;&lt; 7`), so a height of `h` is a floor
/// `h * 128` above the world's zero. A step between two tiles is a wall, which is
/// what makes a plain heightfield enough to occlude a dungeon: the pass marches it
/// in world space, where being off screen means nothing.
///
/// **Off by default.** The mechanism is measured; the picture is one nobody has
/// seen, and unlike the rest of the occlusion pass this one can darken a place the
/// player is looking away from, which is a judgement rather than a measurement.
/// See "Occluders the camera cannot see" in docs/RENDERING.md.
/// </summary>
public static class AoWorld
{
    // libgpu DrawOTag, per overlay -- the frame boundary the rest of the pass uses.
    static readonly (string Overlay, uint Addr)[] DrawOTag =
    [
        .. SdkAddr.DrawOTag,
    ];

    /// <summary>The view matrix `func_80031950` loads before every tile half: nine
    /// shorts at 4096, then a translation this never reads.</summary>
    const uint ViewMatrix = 0x80192E18;

    /// <summary>The camera's world position as the tile walk sees it, u16 each, and
    /// the player's own as a full s32 -- the u16 supplies the low bits the renderer
    /// actually drew with, the s32 the high bits it cannot hold.</summary>
    const uint CamWorldX = 0x80192E78, CamWorldY = 0x80192E7C, CamWorldZ = 0x80192E80;
    const uint PlayerX = 0x801994EC, PlayerY = 0x801994F0, PlayerZ = 0x801994F4;

    /// <summary>The map: 80x80 tiles of 10 bytes; +0/+1 the lower half's model and
    /// height byte, +5/+6 the upper's. A model of 240 or more is not drawn.</summary>
    const uint TileBase = 0x801C8484;
    const int Span = GteDepth.AoHeightSpan, Stride = 10, NotDrawn = 240;

    /// <summary>`+4` bit 0x80: the tile stops the visibility flood. It is what a
    /// wall is in this grid -- the floor heights either side of one are equal.</summary>
    const byte Blocks = 0x80;

    /// <summary>Frames between grid reads. Doors and lifts rewrite tiles, so it is a
    /// clock rather than an area-load event. A change must be read twice, so this is
    /// half the delay before one reaches the picture.</summary>
    const int Rebuild = 30;

    /// <summary>How far the renderer's camera may be from the player before the
    /// reading is refused. A tile is 2048 and the camera sits on the player.</summary>
    const float NearPlayer = 4096f;

    static bool _probe, _detail;
    static long _frames, _rebuilds;
    static double _windowStart;
    static int _sinceRebuild;
    static int[]? _raw;
    static byte[]? _live, _pending;
    static long _gridChanges, _gridPending;

    static bool Same(byte[] a, byte[] b)
    {
        if (a.Length != b.Length) return false;
        for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return false;
        return true;
    }
    static int _minX, _maxX, _minZ, _maxZ;

    static double Now => Environment.TickCount64 / 1000.0;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.aoworld",
        Name = "World-space occlusion",
        Version = "1.0",
        Description = "The occlusion pass also sees the floor plan it is standing in.",
    };

    public static void Configure(string? on, string? strength, string? radius, string? probe)
    {
        if (!string.IsNullOrWhiteSpace(on))
            GteDepth.AoWorld = !on.Equals("0", StringComparison.Ordinal);
        if (float.TryParse(strength, out float st) && st >= 0f)
            GteDepth.AoWorldStrength = Math.Clamp(st, 0f, 1f);
        if (float.TryParse(radius, out float r) && r > 0f)
            GteDepth.AoWorldRadius = r;
        if (!string.IsNullOrWhiteSpace(probe) && !probe.Equals("0", StringComparison.Ordinal))
            _probe = true;
        if (probe == "2") _detail = true;
    }

    public static void Install()
    {
        _windowStart = Now;
        Event.AddListener<OverlayLoadedEvent>(_ =>
        {
            // A new executable is a new area's worth of everything; the grid is
            // rebuilt on the next frame rather than trusted across the load.
            GteDepth.AoWorldReady = false;
            _sinceRebuild = Rebuild;
            _live = _pending = null;
        });

        bool attached = false;
        Event.AddListener<OverlayLoadedEvent>(_ =>
        {
            if (attached) return;
            attached = true;
            Attach();
        });
    }

    static void Attach()
    {
        SymbolRegistry.Build();
        var pre = typeof(AoWorld).GetMethod(nameof(BeforeDrawOTag), BindingFlags.Public | BindingFlags.Static)!;

        int n = 0;
        foreach (var (overlay, addr) in DrawOTag)
        {
            var target = SymbolRegistry.Resolve(overlay, null, addr);
            if (target == null) continue;
            if (HookManager.AddPre(_self, target, pre)) n++;
        }
        HookManager.Commit();
        Console.WriteLine($"[KF2] ao world: {(GteDepth.AoWorld ? "on" : "off")}, {n} hook(s)");
    }

    public static void BeforeDrawOTag(CpuContext c, IMemory m)
    {
        if (!GteDepth.AoWorld || !GteDepth.AmbientOcclusion) return;
        if (m is not PSMemory mem) return;

        _frames++;
        ReadTransform(mem);
        // The grid is rewritten by doors and lifts, so it is re-read on a clock
        // rather than only on an area load.
        if (_sinceRebuild++ >= Rebuild) { BuildHeightfield(mem); _sinceRebuild = 0; _rebuilds++; }

        if (_probe) Report();
    }

    /// <summary>
    /// The camera's rotation and world position, published for the pass.
    ///
    /// **The last good one is held rather than dropped.** The matrix belongs to the
    /// tile walk, so a frame that draws no map tiles — a menu, a fade, a cutscene,
    /// or simply a second present inside one game frame — can read it stale or
    /// zeroed. Switching the term off for that frame and on again for the next one
    /// changes the shading of the whole picture between two levels, which is seen as
    /// the screen flashing. A transform that fails the checks is refused and the
    /// previous one stands; only an area load clears it.
    /// </summary>
    static void ReadTransform(PSMemory mem)
    {
        Span<float> m = stackalloc float[9];
        for (int i = 0; i < 9; i++)
            m[i] = (short)mem.ReadU16(ViewMatrix + (uint)(i * 2)) * (1f / 4096f);

        // A rotation's rows are unit vectors. Anything else is not the matrix.
        for (int row = 0; row < 3; row++)
        {
            float len = MathF.Sqrt(m[row * 3] * m[row * 3] + m[row * 3 + 1] * m[row * 3 + 1]
                                 + m[row * 3 + 2] * m[row * 3 + 2]);
            if (len is < 0.9f or > 1.1f) { GteDepth.AoWorldStale++; return; }
        }

        // And the renderer's camera has to be where the player is. A stale u16
        // splices onto the player's high bits as a position a world away, which
        // samples the floor plan somewhere else entirely.
        float cx = Compose(mem, PlayerX, CamWorldX);
        float cy = Compose(mem, PlayerY, CamWorldY);
        float cz = Compose(mem, PlayerZ, CamWorldZ);
        if (MathF.Abs(cx - (int)mem.ReadU32(PlayerX)) > NearPlayer
         || MathF.Abs(cz - (int)mem.ReadU32(PlayerZ)) > NearPlayer)
        {
            GteDepth.AoWorldStale++;
            return;
        }

        _raw ??= new int[6];
        _raw[0] = (int)mem.ReadU32(PlayerX); _raw[1] = (int)mem.ReadU32(PlayerY); _raw[2] = (int)mem.ReadU32(PlayerZ);
        _raw[3] = (int)mem.ReadU16(CamWorldX); _raw[4] = (int)mem.ReadU16(CamWorldY); _raw[5] = (int)mem.ReadU16(CamWorldZ);

        var r = GteDepth.AoViewR;
        for (int i = 0; i < 9; i++) r[i] = m[i];
        GteDepth.AoCamX = cx; GteDepth.AoCamY = cy; GteDepth.AoCamZ = cz;
        GteDepth.AoWorldReady = GteDepth.AoHeight != null;
        return;

        // The renderer's own camera is the u16 pair; the player's s32 supplies the
        // bits a u16 cannot hold. Pick the wrap nearest the player, which is the
        // only one within half a world of it.
    }

    static float Compose(PSMemory mem, uint full, uint low)
    {
        int p = (int)mem.ReadU32(full);
        int lo = (int)mem.ReadU16(low);
        int v = (p & ~0xFFFF) | lo;
        if (v - p > 0x8000) v -= 0x10000;
        else if (p - v > 0x8000) v += 0x10000;
        return v;
    }

    /// <summary>
    /// The area's floor plan: per tile, the two halves' floor heights and whether
    /// either half blocks sight.
    ///
    /// **The floor heights alone occlude almost nothing, and that is a fact about
    /// the game rather than about the pass.** A corridor's walls are not a step in
    /// the height grid — the floor either side of a wall is the same height — so a
    /// pure heightfield march found a horizon nowhere and the term measured as
    /// exactly zero. What a wall *is* here is `+4 &amp; 0x80`, the bit `CullGrid`
    /// floods visibility against: a tile that stops sight is a tile that stops
    /// ambient light, which is the same claim the flood already makes.
    /// </summary>
    static void BuildHeightfield(PSMemory mem)
    {
        var h = GteDepth.AoHeight ??= new byte[Span * Span * 4];
        int blockers = 0;
        for (int z = 0; z < Span; z++)
        {
            uint row = TileBase + (uint)(z * Span * Stride);
            int o = z * Span * 4;
            for (int x = 0; x < Span; x++)
            {
                uint t = row + (uint)(x * Stride);
                bool lowDrawn = mem.ReadU8(t) < NotDrawn;
                bool upDrawn = mem.ReadU8(t + 5u) < NotDrawn;
                bool block = (mem.ReadU8(t + 4u) & Blocks) != 0 || (mem.ReadU8(t + 9u) & Blocks) != 0;
                h[o + x * 4 + 0] = mem.ReadU8(t + 1u);
                h[o + x * 4 + 1] = mem.ReadU8(t + 6u);
                // Validity is its own channel and not a height of zero: zero is a
                // real floor, and overloading it emptied every tile in the first
                // area the probe looked at.
                h[o + x * 4 + 2] = (byte)((lowDrawn ? 1 : 0) | (upDrawn ? 2 : 0) | (block ? 4 : 0));
                h[o + x * 4 + 3] = 255;
                if (block) blockers++;
            }
        }
        GteDepth.AoHeightBlockers = blockers;

        // **A change has to be read twice before it is believed.**
        //
        // The read is on a clock and the game rewrites tile records while it runs —
        // a door, a lift, and the whole grid while an area loads. A read that lands
        // mid-rewrite comes back with tiles missing, and a missing tile is solid
        // rock to the march, so *the whole picture darkens* until the next read puts
        // it back. That is one step down and one step up a few tenths of a second
        // apart, which is exactly what a flash is.
        //
        // Nothing here can tell a half-written grid from a real change by looking at
        // it, but a half-written one does not survive being read again. So a
        // difference is held as pending and only accepted when a second read agrees
        // with it; the live grid stands until then. A door costs two read periods to
        // appear, which is well inside how long a door takes to open.
        if (_live != null && Same(h, _live)) { _pending = null; return; }

        if (_pending == null || !Same(h, _pending))
        {
            (_pending ??= new byte[h.Length]).AsSpan().Clear();
            h.AsSpan().CopyTo(_pending);
            _gridPending++;
            // The live grid keeps its generation, so nothing is uploaded and the
            // picture does not move.
            if (_live != null) { _live.AsSpan().CopyTo(h); return; }
        }

        (_live ??= new byte[h.Length]).AsSpan().Clear();
        h.AsSpan().CopyTo(_live);
        _pending = null;
        _gridChanges++;
        _minX = _minZ = Span; _maxX = _maxZ = -1;
        for (int z = 0; z < Span; z++)
            for (int x = 0; x < Span; x++)
            {
                int o = (z * Span + x) * 4;
                if (h[o + 2] == 0) continue;
                if (x < _minX) _minX = x;
                if (x > _maxX) _maxX = x;
                if (z < _minZ) _minZ = z;
                if (z > _maxZ) _maxZ = z;
            }
        GteDepth.AoHeightGen++;
    }

    static void Report()
    {
        double window = Now - _windowStart;
        if (window < 2.0) return;

        var r = GteDepth.AoViewR;
        Console.WriteLine($"[KF2] ao world: {(GteDepth.AoWorldReady ? "ready" : "NOT ready")}, " +
                          $"cam {GteDepth.AoCamX:F0},{GteDepth.AoCamY:F0},{GteDepth.AoCamZ:F0} " +
                          $"(tile {(int)GteDepth.AoCamX >> 11},{(int)GteDepth.AoCamZ >> 11}), " +
                          $"row0 {r[0]:F3},{r[1]:F3},{r[2]:F3}, " +
                          $"{GteDepth.AoHeightBlockers} blocking tiles, " +
                          $"{_rebuilds / window:F1} grid reads/s ({_gridChanges / window:F1} changed, {_gridPending / window:F1} held), " +
                          $"{GteDepth.AoWorldFrames / window:F0} shaded frames/s, " +
                          $"{GteDepth.AoWorldStale / window:F0} stale reads/s, " +
                          $"{GteDepth.AoWorldUnready / window:F0} unready/s, over {_frames / window:F0} frames/s");

        // The transform, checked against something that has an answer: the third
        // row of a world-to-view rotation is the camera's own forward axis, so a
        // point 2048 in front of the camera must land in a tile next to its own.
        float fx = r[6], fy = r[7], fz = r[8];
        float len = MathF.Sqrt(fx * fx + fy * fy + fz * fz);
        int ftx = (int)(GteDepth.AoCamX + 2048f * fx) >> 11;
        int ftz = (int)(GteDepth.AoCamZ + 2048f * fz) >> 11;
        Console.WriteLine($"[KF2] ao world: forward {fx:F3},{fy:F3},{fz:F3} (len {len:F3}) " +
                          $"-> tile {ftx},{ftz} from {(int)GteDepth.AoCamX >> 11},{(int)GteDepth.AoCamZ >> 11}");

        if (!_detail) { Reset(); return; }

        if (_raw != null)
            Console.WriteLine($"[KF2] ao world: raw player {_raw[0]},{_raw[1]},{_raw[2]}  " +
                              $"camU16 {_raw[3]},{_raw[4]},{_raw[5]}  " +
                              $"occupied tiles x {_minX}..{_maxX} z {_minZ}..{_maxZ}");

        // The floor plan the pass is actually marching, around the camera.
        var h = GteDepth.AoHeight;
        if (h != null)
        {
            int cx = (int)GteDepth.AoCamX >> 11, cz = (int)GteDepth.AoCamZ >> 11;
            Console.WriteLine($"[KF2] ao world: cam up {-GteDepth.AoCamY:F0}; tiles around " +
                              $"(height*128, * = stops sight)");
            int near = 0;
            for (int dz = -3; dz <= 3; dz++)
                for (int dx = -3; dx <= 3; dx++)
                {
                    int x = cx + dx, z = cz + dz;
                    if ((uint)x >= Span || (uint)z >= Span) continue;
                    if ((h[(z * Span + x) * 4 + 2] & 4) != 0) near++;
                }
            Console.WriteLine($"[KF2] ao world: {near} blocking tiles within 3 of the camera");

            for (int dz = -1; dz <= 1; dz++)
            {
                var row = new System.Text.StringBuilder("[KF2] ao world:  ");
                for (int dx = -1; dx <= 1; dx++)
                {
                    int x = cx + dx, z = cz + dz;
                    if ((uint)x >= Span || (uint)z >= Span) { row.Append("  ---   "); continue; }
                    int o = (z * Span + x) * 4;
                    int f = h[o + 2];
                    row.Append($"[{((f & 1) != 0 ? h[o] * 128 : -1),6}/{((f & 2) != 0 ? h[o + 1] * 128 : -1),6}{((f & 4) != 0 ? "*" : " ")}]");
                }
                Console.WriteLine(row.ToString());
            }
        }

        Reset();
    }

    static void Reset()
    {
        GteDepth.AoWorldFrames = GteDepth.AoWorldUnready = GteDepth.AoWorldStale = 0;
        _frames = _rebuilds = _gridChanges = _gridPending = 0;
        _windowStart = Now;
    }
}
