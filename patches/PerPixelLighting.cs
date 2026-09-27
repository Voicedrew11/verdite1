using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// Per-pixel lighting: the world's and the models' colours evaluated at every pixel
/// from what the GTE made them from, instead of interpolated between the colours it
/// produced at the corners.
///
///     KF2_PERPIXEL=0        off (on by default; GL core only)
///     KF2_PERPIXEL_PROBE=1  packets recorded, and polygons lit per pixel or drawn from
///                           their corner colours, every two seconds
///     KF2_PERPIXEL_PROBE=2  also the shader's formula at every recorded corner against
///                           the colour the GTE wrote there
///
/// A map tile is lit once per face, so its only per-vertex variation is the depth
/// cue: a weight that is clamped, then bent by the game's own curve (a 3x slope past
/// IR0 2800, or IR0 / 2 for a clipped polygon), then applied to the colour, which
/// saturates. Interpolating the corner colours across a two-tile floor puts none of
/// those where they belong, and they move as the camera does. A model's gouraud face
/// adds per-vertex normals, and one fog weight for the whole face.
///
/// <see cref="PolyAssembler"/> records each packet's inputs as it builds it
/// (PolyAssemblerLight.cs), the runtime's <c>GteLightMap</c> hands them to the GPU by
/// packet address, and the core-profile prim shader lights the pixel (0048). A packet
/// the port did not build draws its own colours, as before. GL backend only.
/// See "Per-pixel lighting" in docs/RENDERING.md.
/// </summary>
public static class PerPixelLighting
{
    static readonly (string Overlay, uint Addr)[] DrawOTag =
    [
        .. SdkAddr.DrawOTag,
    ];

    public const string OnKey = "kf2.perpixel.on";

    public static bool Enabled => GteLightMap.Enabled;

    static bool? _forced;
    static bool _probe;

    /// <summary>KF2_PERPIXEL_PROBE=2: check every recorded corner against the GTE's colour.</summary>
    public static bool Check;
    public static long CheckFlat;
    /// <summary>Vertices the transform's record did not describe: out of range, screen
    /// word, fog word; and faces that fell back to the fog words.</summary>
    public static readonly long[] Fallback = new long[4];
    static readonly long[] _cornerErr = new long[4];
    static readonly long[] _cornerByCurve = new long[16];
    static readonly long[] _cornerBadByCurve = new long[16];

    public static void NoteCorner(int err, uint mode)
    {
        _cornerErr[Math.Min(err, 3)]++;
        int k = (int)(mode & 7u) | ((mode & GteLightMap.Directional) != 0 ? 8 : 0);
        _cornerByCurve[k]++;
        if (err > 1) _cornerBadByCurve[k]++;
    }

    static long _frames;
    static double _windowStart;

    static double Now => Environment.TickCount64 / 1000.0;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.perpixel",
        Name = "Per-pixel lighting",
        Version = "1.0",
        Description = "The GTE's lighting and depth cue evaluated per pixel.",
    };

    public static void Configure(string? on, string? probe)
    {
        if (!string.IsNullOrWhiteSpace(on)) _forced = on.Trim() != "0";
        _probe = !string.IsNullOrWhiteSpace(probe) && probe.Trim() != "0";
        Check = probe?.Trim() == "2";
    }

    public static void Install()
    {
        // The saved key is only readable once the runtime is up (see AmbientOcclusion).
        GteLightMap.Enabled = _forced ?? true;
        _windowStart = Now;

        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            GteLightMap.Enabled = _forced ?? RecompOne.Runtime.Runtime.View.GetBool(OnKey, true);
            Console.WriteLine($"[KF2] per-pixel lighting: {(GteLightMap.Enabled ? "on" : "off")}" +
                              (GteLightMap.Enabled && !GteLightMap.Supported ? " (not drawn: needs the core GL backend)" : ""));
        });

        HookAttach.OnOverlayLoad("per-pixel lighting", AttachHud);

        bool attached = false;
        Event.AddListener<OverlayLoadedEvent>(_ =>
        {
            if (attached || !_probe) return;
            attached = Attach();
        });
    }

    /// <summary>The HUD builder, which draws its icons through the lit model assembler.</summary>
    const uint HudBuilder = 0x80031D5C;

    static bool AttachHud()
    {
        SymbolRegistry.Build();
        var target = SymbolRegistry.Resolve("game", null, HudBuilder);
        if (target == null) return false;
        const BindingFlags flags = BindingFlags.Public | BindingFlags.Static;
        if (!_hudQueued)
        {
            HookManager.AddPre(_self, target, typeof(PerPixelLighting).GetMethod(nameof(BeforeHud), flags)!);
            HookManager.AddPost(_self, target, typeof(PerPixelLighting).GetMethod(nameof(AfterHud), flags)!);
            _hudQueued = true;
        }
        HookManager.Commit();
        return HookAttach.Installed(target);
    }

    static bool _hudQueued;

    public static void BeforeHud(CpuContext c, IMemory m) => PolyAssembler.InHud = true;
    public static void AfterHud(CpuContext c, IMemory m) => PolyAssembler.InHud = false;

    public static void SetEnabled(bool on) => GteLightMap.Enabled = on;

    static bool Attach()
    {
        SymbolRegistry.Build();
        var after = typeof(PerPixelLighting).GetMethod(nameof(AfterDrawOTag), BindingFlags.Public | BindingFlags.Static)!;
        var targets = new List<MethodInfo>();
        foreach (var (overlay, addr) in DrawOTag)
        {
            var target = SymbolRegistry.Resolve(overlay, null, addr);
            if (target == null) continue;
            HookManager.AddPost(_self, target, after);
            targets.Add(target);
        }
        HookManager.Commit();
        int n = targets.Count(HookManager.IsCommitted);
        Console.WriteLine($"[KF2] per-pixel lighting: probe on, {n} hook(s)");
        return n > 0;
    }

    public static void AfterDrawOTag(CpuContext c, IMemory m)
    {
        _frames++;
        double window = Now - _windowStart;
        if (window < 2.0) return;

        long hits = GteLightMap.Hits, misses = GteLightMap.Misses;
        double share = hits + misses == 0 ? 0 : 100.0 * hits / (hits + misses);
        Console.WriteLine($"[KF2] per-pixel lighting: {(GteLightMap.Active ? "on" : GteLightMap.Enabled ? "unsupported" : "off")}, " +
                          $"{GteLightMap.Recorded / window:F0} packets recorded/s, " +
                          $"{hits / window:F0} polygons lit per pixel/s, {misses / window:F0} from their corner colours/s ({share:F1}%), " +
                          $"over {_frames / window:F0} frames/s");
        if (Check)
        {
            var kinds = Enumerable.Range(0, 16).Where(k => _cornerByCurve[k] > 0)
                .Select(k => $"{((k & 8) != 0 ? "lit+" : "")}curve{k & 7} {_cornerByCurve[k]} ({_cornerBadByCurve[k]} off by 2+)");
            Console.WriteLine($"[KF2] per-pixel lighting: corners exact {_cornerErr[0]}, off by 1 {_cornerErr[1]}, " +
                              $"by 2 {_cornerErr[2]}, by 3+ {_cornerErr[3]}; flat faces {CheckFlat}; {string.Join(", ", kinds)}");
            Console.WriteLine($"[KF2] per-pixel lighting: fog-word fallbacks {Fallback[3]} face(s): vertices out of range {Fallback[0]}, " +
                              $"screen word moved {Fallback[1]}, fog word moved {Fallback[2]}");
            Array.Clear(Fallback);
            Array.Clear(_cornerErr); Array.Clear(_cornerByCurve); Array.Clear(_cornerBadByCurve);
            CheckFlat = 0;
        }
        GteLightMap.ResetCounters();
        _frames = 0;
        _windowStart = Now;
    }
}
