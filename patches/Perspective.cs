using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// Perspective-correct textures, Verdite3's Perspective on this disc. The work is
/// in the fork: <see cref="GteDepth.Enabled"/> switches the address map
/// (<see cref="GteVertexMap"/>) on and the renderers interpolate U/W, V/W and 1/W.
/// This file is the switch and the probe only. Off until judged.
///
///     KF1_PERSPECTIVE=1        on; 0 or unset is the console's affine mapping
///     KF1_PERSPECTIVE_PROBE=1  a line every 2 s: the map's roots, propagations,
///                              hits and misses, and its hit rate
///
/// See "Unit 2" in docs/PICTURE.md.
/// </summary>
public static class Perspective
{
    // libgpu DrawOTag, per overlay, for the probe's frame boundary.
    static readonly (string Overlay, uint Addr)[] DrawOTag =
        [("open", 0x800309D4), ("game", 0x80050CF0)];

    /// <summary>False restores the console's affine mapping. Live.</summary>
    public static bool Enabled
    {
        get => GteDepth.Enabled;
        set { GteDepth.Enabled = value; GteVertexMap.SetActive(GteDepth.Active); }
    }

    static bool _probe;

    /// <summary>KF1_PERSPECTIVE_PROBE: count the map and report it. Live.</summary>
    public static bool ProbeOn
    {
        get => _probe;
        set
        {
            if (_probe == value) return;
            _probe = value;
            _frames = 0;
            _windowStart = Now;
            Baseline();
        }
    }

    /// <summary>KF1_PERSPECTIVE: an explicit command-line choice, which wins for the run.</summary>
    static bool? _forced;

    static long _frames;
    static double _windowStart = Now;

    // The map's counters are lifetime totals, so each window is a difference.
    static long _hits, _misses, _roots, _copied;

    static double Now => Environment.TickCount64 / 1000.0;

    static readonly ModInfo _self = new()
    {
        Id = "kf1.perspective",
        Name = "Perspective-correct textures",
        Version = "1.0",
        Description = "Recovers per-vertex depth from the GTE so textures stop swimming.",
    };

    public static void Configure(string? on, string? probe)
    {
        if (!string.IsNullOrWhiteSpace(on)) _forced = on.Trim() is not ("0" or "off");
        ProbeOn = probe?.Trim() == "1";
    }

    public static void Install()
    {
        Enabled = _forced ?? false;
        Baseline();
        // Attached in every state, so the probe can be switched on live.
        HookAttach.OnOverlayLoad("perspective", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var after = typeof(Perspective).GetMethod(nameof(AfterDrawOTag), BindingFlags.Public | BindingFlags.Static)!;

        int n = 0;
        foreach (var (overlay, addr) in DrawOTag)
        {
            var target = SymbolRegistry.Resolve(overlay, null, addr);
            if (target == null) { Console.Error.WriteLine($"[KF1] perspective: no function at {overlay}/0x{addr:X8}"); continue; }
            if (HookManager.AddPost(_self, target, after)) n++;
        }

        HookManager.Commit();
        Console.WriteLine($"[KF1] perspective: {(Enabled ? "on" : "off (affine)")}, probe {(_probe ? "on" : "off")}, {n}/2 function(s) hooked");
        return n == 2;
    }

    static void Baseline()
    {
        _hits = GteVertexMap.Hits;
        _misses = GteVertexMap.Misses;
        _roots = GteVertexMap.Roots;
        _copied = GteVertexMap.Propagated;
    }

    public static void AfterDrawOTag(CpuContext c, IMemory m)
    {
        if (!_probe) return;

        _frames++;
        double window = Now - _windowStart;
        if (window < 2.0) return;

        long hits = GteVertexMap.Hits - _hits, misses = GteVertexMap.Misses - _misses;
        long roots = GteVertexMap.Roots - _roots, copied = GteVertexMap.Propagated - _copied;
        long asked = hits + misses;

        Console.WriteLine($"[KF1] perspective: {roots / window:F0} roots/s, {copied / window:F0} propagated/s, " +
                          $"{hits / window:F0} hits/s, {misses / window:F0} misses/s " +
                          $"({(asked == 0 ? 0.0 : 100.0 * hits / asked):F1}% hit), over {_frames / window:F0} frames/s");

        Baseline();
        _frames = 0;
        _windowStart = Now;
    }
}
