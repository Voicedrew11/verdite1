using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// Widen the cells the renderer walks with the aspect, so a wide picture's sides
/// are not cut off where a 4:3 one did not need them.
///
///     KF1_CULLCONE=0          leave the game's stencils at any aspect, to compare
///     KF1_CULLCONE_PROBE=1    a line every 2 s: cells drawn a frame, cells added
///
/// The map walk func_8001E83C draws the cells of one of sixteen 14x14 stencils
/// around the camera's cell, picked by the yaw's high byte (a 22.5-degree sector
/// each), and the model walk draws only what stands in the same stencil's cells.
/// The stencils are data, read from KF/COM/COM.DAT to 0x80065BE8 (0xCC bytes each:
/// width, height, origin x and z, a byte a cell). They are not a clean rule; the
/// nearest is a cone of about 48 degrees either side within 10 cells, where the 4:3
/// screen needs 38.7. So nothing of the game's is removed: before each walk, a
/// stencil gets the cells that cone gains when it opens by the aspect's extra
/// half-angle, inside the 14x14 grid, and the game's own tables are put back at
/// 4:3. See "The cull" in docs/WIDESCREEN.md.
/// </summary>
public static class CullCone
{
    const uint MapWalk = 0x8001E83C;
    const uint CellDraw = 0x8001E5EC;
    const uint Stencils = 0x80065BE8;
    const int Stride = 0xCC, Count = 16, Grid = 14;

    // The game's tables are matched by a cone of this half-angle and reach.
    const double BaseHalf = 48.0 * Math.PI / 180.0;
    const double Reach = 10.0;
    static readonly double Half43 = Math.Atan(160.0 / 200.0);   // SetGeomScreen(200), 320 wide

    public static bool Enabled { get; set; } = true;
    public static bool ProbeOn { get; set; }

    static readonly ModInfo _self = new() { Id = "kf1.cullcone", Name = "Cull cone", Version = "1.0" };

    static byte[]? _original, _widened;
    static float _widenedFor = -1f;
    static int _added;

    static long _cells, _frames;
    static double _windowStart = Environment.TickCount64 / 1000.0;

    public static void Configure(string? on, string? probe)
    {
        if (!string.IsNullOrWhiteSpace(on)) Enabled = on.Trim() is not ("0" or "off");
        ProbeOn = probe?.Trim() == "1";
    }

    public static void Install() => HookAttach.OnOverlayLoad("cull cone", () =>
    {
        SymbolRegistry.Build();
        var walk = SymbolRegistry.Resolve("game", null, MapWalk);
        var cell = SymbolRegistry.Resolve("game", null, CellDraw);
        if (walk == null || cell == null) return false;
        var self = typeof(CullCone);
        HookManager.AddPre(_self, walk, self.GetMethod(nameof(BeforeMapWalk), BindingFlags.Public | BindingFlags.Static)!);
        HookManager.AddPre(_self, cell, self.GetMethod(nameof(BeforeCell), BindingFlags.Public | BindingFlags.Static)!);
        HookManager.Commit();
        return HookAttach.Installed(walk) && HookAttach.Installed(cell);
    });

    public static void BeforeCell(CpuContext c, IMemory m) { if (ProbeOn) _cells++; }

    public static void BeforeMapWalk(CpuContext c, IMemory m)
    {
        if (ProbeOn) Probe();
        var now = Read(m);
        if (_original == null || (!now.AsSpan().SequenceEqual(_original) && (_widened == null || !now.AsSpan().SequenceEqual(_widened))))
        {
            // First sight, or the tables were read in again: these are the game's.
            if (!LooksLikeStencils(now)) return;
            _original = now;
            _widened = null;
        }

        float aspect = Widescreen.On ? Widescreen.Aspect : Widescreen.FourThree;
        bool wide = Enabled && aspect > Widescreen.FourThree + 0.001f;
        if (wide && (_widened == null || _widenedFor != aspect))
        {
            _widened = Widen(_original, aspect, out _added);
            _widenedFor = aspect;
            Console.WriteLine($"[KF1] cull cone: {aspect:0.###}:1 adds {_added} cell(s) over the {Count} stencils");
        }
        var want = wide ? _widened! : _original;
        if (!now.AsSpan().SequenceEqual(want)) Write(m, want);
    }

    static byte[] Read(IMemory m)
    {
        var b = new byte[Stride * Count];
        for (int i = 0; i < b.Length; i++) b[i] = m.ReadU8(Stencils + (uint)i);
        return b;
    }

    static void Write(IMemory m, byte[] b)
    {
        for (int i = 0; i < b.Length; i++) m.WriteU8(Stencils + (uint)i, b[i]);
    }

    static bool LooksLikeStencils(byte[] b)
    {
        for (int s = 0; s < Count; s++)
            if (b[s * Stride] != Grid || b[s * Stride + 2] != Grid) return false;
        return true;
    }

    /// <summary>The game's tables plus the cells the base cone gains at this aspect.</summary>
    static byte[] Widen(byte[] original, float aspect, out int added)
    {
        var b = (byte[])original.Clone();
        double extra = Math.Atan(Math.Tan(Half43) * aspect / (4.0 / 3.0)) - Half43;
        added = 0;
        for (int s = 0; s < Count; s++)
        {
            int o = s * Stride;
            int ox = b[o + 4] | b[o + 5] << 8, oz = b[o + 6] | b[o + 7] << 8;
            for (int row = 0; row < Grid; row++)
            for (int col = 0; col < Grid; col++)
            {
                int at = o + 8 + row * Grid + col;
                if (b[at] != 0) continue;
                int dx = col - ox, dz = row - oz;
                if (Seen(s, dx, dz, BaseHalf + extra) && !Seen(s, dx, dz, BaseHalf))
                {
                    b[at] = 1;
                    added++;
                }
            }
        }
        return b;
    }

    /// <summary>Whether any point of cell (dx, dz) lies inside a cone of this half-angle
    /// and <see cref="Reach"/> for some heading in stencil s's sector and some camera
    /// place in its own cell. Stencil s serves yaws 256*(15-s) to 256*(16-s); a yaw
    /// faces (-sin, cos) in (x, z).</summary>
    static bool Seen(int s, int dx, int dz, double half)
    {
        double cos = Math.Cos(half);
        int k = 15 - s;
        ReadOnlySpan<int> headings = [0, 64, 128, 192, 255];
        ReadOnlySpan<double> cams = [0.0, 0.5, 1.0];
        foreach (int t in headings)
        {
            double phi = (256 * k + t) * 2.0 * Math.PI / 4096.0;
            double fx = -Math.Sin(phi), fz = Math.Cos(phi);
            foreach (double cx in cams)
            foreach (double cz in cams)
                for (int px = 0; px < 4; px++)
                for (int pz = 0; pz < 4; pz++)
                {
                    double vx = dx + px / 3.0 - cx, vz = dz + pz / 3.0 - cz;
                    double len = Math.Sqrt(vx * vx + vz * vz);
                    if (len > Reach) continue;
                    if (len < 1e-6 || (vx * fx + vz * fz) / len >= cos) return true;
                }
        }
        return false;
    }

    static void Probe()
    {
        _frames++;
        double now = Environment.TickCount64 / 1000.0;
        double window = now - _windowStart;
        if (window < 2.0) return;
        Console.WriteLine($"[KF1] cull cone: {(_frames == 0 ? 0 : _cells / (double)_frames):0.0} cell(s) drawn a frame, " +
                          $"{(Enabled && Widescreen.On ? _added : 0)} cell(s) added to the stencils");
        Console.Out.Flush();
        _cells = _frames = 0;
        _windowStart = now;
    }
}
