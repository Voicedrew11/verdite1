using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// Sub-pixel vertex positioning — the fix for the wobbling, shimmering geometry
/// that is the other half of what makes a PlayStation picture recognisable.
///
///     KF2_SUBPIXEL=0             off; unset is on (the shipped default)
///     KF2_SUBPIXEL_PROBE=1       report how far vertices are actually moving
///
/// The GTE projects a vertex to 16.16 fixed point and then keeps only the whole
/// part: <c>SX2</c>/<c>SY2</c> are pixels, and the game copies those pixels into a
/// GP0 packet. So a vertex that should drift a twentieth of a pixel per frame sits
/// still for twenty frames and then jumps a whole one, and the polygon it belongs
/// to twitches and shears as its corners jump at different moments. Walk slowly
/// towards a wall and its edges crawl; that is the whole of it.
///
/// The fraction is not computed and lost, it is computed and *truncated* — the low
/// sixteen bits of the same expression <see cref="Perspective"/> takes the depth
/// from, one shift earlier. So it is caught in the same place and carried the same
/// way: <c>Gte.Rtp</c> hands (z, fx, fy) to <see cref="GteVertexMap"/> as the
/// coordinate leaves the GTE, the map follows the word into the primitive packet by
/// the address it is stored at, and the GPU asks for it again by the address it read
/// the word back from. The two halves share one map and one lookup and are switched
/// on independently.
///
/// Everything that makes the depth safe to recover makes the fraction safe too — a
/// miss leaves the vertex on the whole pixel the packet named, 2D never hits, and a
/// vertex that saturated off screen is still recorded for its depth so the polygon
/// can stay perspective-correct, but it stays on the clamp — moving it to the
/// GTE's true position opened holes along shared edges. One rule is deliberately
/// *not* carried over: correction is per vertex here, where W is all-or-nothing per
/// primitive. W is an interpolation parameter and one corner disagreeing about it
/// tears the texture across the whole triangle; a fraction is just where a corner
/// is. And because two triangles sharing a vertex read the same word out of the same
/// address, a shared edge keeps identical endpoints on both sides — no crack can open
/// along one.
///
/// Both renderers honour it. The hardware backend needed nothing at all: its vertex
/// position has been a float the whole time, so the fraction is simply added before
/// it is handed over. The software rasterizer did need something — it walks whole
/// pixels with integer edge functions — and now works in sixteenths of a pixel for
/// any triangle that recovered a fraction, which scales its edge functions and its
/// area by 256 and leaves every ratio taken from them identical. A triangle where
/// nothing was recovered runs at shift zero, which is the arithmetic it always did,
/// to the bit.
///
/// **On by default.** The mechanism is confirmed: 47k vertices a second recover a
/// fraction at a 90% hit rate, those fractions are spread evenly across the pixel
/// they were truncated from, and the frame rate does not move. A miss is still the
/// old whole-pixel behaviour. See "Sub-pixel vertex positioning" in NOTES.md.
///
/// As with perspective correction this patch is only the switch and the report; the
/// work is in the runtime (<c>patches/recompone/0010</c> and <c>0012</c>), because where a vertex
/// lands is decided far below anything <c>HookManager</c> can reach. The one hook is
/// on <c>DrawOTag</c>, purely to have a frame boundary to count against, and it is a
/// post-hook so it composes with the widescreen patch's replacement of the same
/// function.
/// </summary>
public static class Subpixel
{
    // libgpu DrawOTag, per overlay -- the same three addresses NoDither hooks.
    static readonly (string Overlay, uint Addr)[] DrawOTag =
    [
        .. SdkAddr.DrawOTag,
    ];

    /// <summary>Where the choice is kept between runs.</summary>
    public const string OnKey = "kf2.subpixel.on";

    /// <summary>False snaps every vertex to a whole pixel, as the console does.</summary>
    public static bool Enabled
    {
        get => GteDepth.Subpixel;
        private set => GteDepth.Subpixel = value;
    }

    /// <summary>KF2_SUBPIXEL: an explicit choice on the command line, which wins
    /// over the saved setting for the run.</summary>
    static bool? _forced;

    /// <summary>KF2_SUBPIXEL_PROBE: also write the report to the console.</summary>
    static bool _toConsole;

    static long _frames;
    static double _windowStart;

    static double Now => Environment.TickCount64 / 1000.0;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.subpixel",
        Name = "Sub-pixel vertex positioning",
        Version = "1.0",
        Description = "Recovers the fraction of a pixel the GTE truncates, so vertices stop snapping.",
    };

    /// <summary>0052, KF2_SUBPIXEL_CULL=0 to compare: cull the C# assemblers' faces at
    /// their fractional corners while sub-pixel is on.</summary>
    public static bool Cull = true;

    public static void Configure(string? on, string? probe, string? cull = null)
    {
        if (cull?.Trim() == "0") Cull = false;
        if (!string.IsNullOrWhiteSpace(on))
            _forced = !on.Equals("0", StringComparison.Ordinal);

        if (!string.IsNullOrWhiteSpace(probe) && !probe.Equals("0", StringComparison.Ordinal))
            _toConsole = true;
    }

    public static void Install()
    {
        _windowStart = Now;

        // RuntimeReadyEvent is the first and only place the saved setting is
        // decided. ConfigManager only loads inside HostWindow.Initialize, which
        // is after Program.cs, so reading it here would read an empty config and
        // write it back over the real one.
        Enabled = _forced ?? true;
        GteDepth.Probe = _toConsole;

        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            Enabled = _forced ?? RecompOne.Runtime.Runtime.View.GetBool(OnKey, true);
            Console.WriteLine($"[KF2] subpixel: {(Enabled ? "on" : "off (whole pixels)")}");
        });

        bool attached = false;
        Event.AddListener<OverlayLoadedEvent>(_ =>
        {
            if (attached || !_toConsole) return;
            attached = true;
            Attach();
        });
    }

    /// <summary>Change the setting at run time. Vertices start or stop carrying
    /// their fraction; a frame drawn during the change is at worst partly moved,
    /// which is a frame of the wobble the setting is about.</summary>
    public static void SetEnabled(bool on) => Enabled = on;

    // Only attached under the probe: without it there is nothing to count and the
    // patch has no reason to touch the game's code at all.
    static void Attach()
    {
        SymbolRegistry.Build();
        var after = typeof(Subpixel).GetMethod(nameof(AfterDrawOTag), BindingFlags.Public | BindingFlags.Static)!;

        int n = 0;
        foreach (var (overlay, addr) in DrawOTag)
        {
            var target = SymbolRegistry.Resolve(overlay, null, addr);
            if (target == null)
            {
                Console.Error.WriteLine($"[KF2] subpixel: no function at {overlay}/0x{addr:X8}");
                continue;
            }
            if (HookManager.AddPost(_self, target, after)) n++;
        }

        HookManager.Commit();
        Console.WriteLine($"[KF2] subpixel: probe on, {n} hook(s)");
    }

    public static void AfterDrawOTag(CpuContext c, IMemory m)
    {
        _frames++;
        double window = Now - _windowStart;
        if (window < 2.0) return;

        long n = GteDepth.OffsetCount;
        double mean = n == 0 ? 0.0 : GteDepth.Offset / n;

        // The mean displacement is the measurement, and it is a different question
        // from the hit rate KF2_PERSPECTIVE_PROBE asks -- which is why this reads
        // its own counters and resets only those. Positions spread evenly inside a
        // pixel are 0.7652 of one from its corner on average and at most sqrt(2)
        // from it, so a mean near that says the recovered fraction really is
        // uniform and not a rounding artefact of a table full of zeroes. Measured
        // in the attract demo: 0.760-0.773, max 1.413.
        Console.WriteLine($"[KF2] subpixel: {n / window:F0} vertices/s carrying a fraction, " +
                          $"mean offset {mean:F3} px, max {GteDepth.OffsetMax:F3} px, " +
                          $"over {_frames / window:F0} frames/s");
        Console.WriteLine($"[KF2] subpixel: {GteDepth.CensusPolys / window:F0} polys/s with every corner fractional " +
                          $"({GteDepth.CensusTiny / window:F0} under 1 px^2), {GteDepth.CensusMixed / window:F0} mixed, " +
                          $"{GteDepth.CensusFlipped / window:F0}/s drawn back to front; cull {(Cull ? "fractional" : "whole pixels")}, " +
                          $"whole pixels would have culled {PolyAssembler.CullKept / window:F0}/s kept and kept {PolyAssembler.CullDropped / window:F0}/s dropped");
        Console.WriteLine($"[KF2] subpixel: HUD {PolyAssembler.ScreenVertices / window:F0} vertices/s placed on the screen, " +
                          $"{PolyAssembler.ScreenFractional / window:F0} with a fraction, " +
                          $"{PolyAssembler.ScreenAligned / window:F0} on unturned pieces kept whole");
        GteDepth.CensusPolys = GteDepth.CensusTiny = GteDepth.CensusMixed = GteDepth.CensusFlipped = 0;
        PolyAssembler.CullKept = PolyAssembler.CullDropped = 0;
        PolyAssembler.ScreenVertices = PolyAssembler.ScreenFractional = PolyAssembler.ScreenAligned = 0;

        GteDepth.ResetOffsets();
        _frames = 0;
        _windowStart = Now;
    }
}
