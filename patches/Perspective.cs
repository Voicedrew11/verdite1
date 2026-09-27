using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// Perspective-correct texture mapping — the fix for the swimming, rippling
/// textures that are the most recognisable thing about a PlayStation picture.
///
///     KF2_PERSPECTIVE=0             off; 1 or unset leaves it on
///     KF2_PERSPECTIVE_PROBE=1       report the vertex map's hit rate
///     KF2_PERSPECTIVE_FALLBACK=1    also guess, by screen position, for the
///                                   vertices the exact map could not answer for
///
/// The console's GPU has no depth. The GTE does the perspective divide, the game
/// copies the resulting 2D screen coordinate into a GP0 packet, and the GPU
/// receives a polygon that is nothing but screen positions, UVs and colours. It
/// can only interpolate U and V linearly across the screen, which is exact for a
/// surface square-on to the camera and progressively wrong as the surface turns
/// away — so a floor's texture slides as you walk over it, and a quad shows a
/// crease along the diagonal where its two triangles disagree about where the
/// middle of the texture went.
///
/// Nothing in the packet can fix that; the depth is simply gone by then. So it is
/// caught earlier, in <see cref="GteVertexMap"/>: <c>Gte.Rtp</c> knows the screen
/// position and the view depth SZ3 in the same call, and the coordinate then travels
/// to the GPU through memory — <c>swc2</c> into the game's vertex cache, a whole-word
/// <c>lw</c>/<c>sw</c> into the primitive packet, and a read by <c>DrawOTag</c> or by
/// the DMA. The map follows it by **address**, which is the vertex itself, and checks
/// the word it recorded against the word the GPU is drawing before it answers.
///
/// It used to follow the coordinate by its screen position, which is not an identity:
/// two vertices share a pixel constantly and every off-screen vertex shares the ±1024
/// clamp, so the answer was a depth for that pixel rather than for that vertex. W is
/// the denominator of the perspective divide, so a wrong one did not soften a texture,
/// it threw it across the screen — and the same wrong pick handed a corner another
/// corner's sub-pixel fraction, which is a vertex that snaps for no visible reason.
///
/// A vertex that misses is left alone, which is what makes this safe to leave on:
///
///   * The **HUD and the menus** never went through the GTE, so they never hit,
///     so they keep the affine mapping 2D work actually wants.
///   * A polygon is corrected **only if every one of its vertices hit**, so it can
///     never mix a recovered depth with a fallback and shear itself apart.
///   * A coordinate the game computed itself, or edited after copying it, fails the
///     value check and stays affine rather than borrowing a stranger's depth.
///
/// Both renderers honour it. The software rasterizer interpolates U/W, V/W and 1/W
/// and divides per pixel; the hardware backend puts the depth in
/// <c>gl_Position.w</c> and lets the rasterizer do it, with <c>noperspective</c> on
/// the colour so Gouraud shading stays exactly as flat as the console's. A vertex
/// with no depth carries W = 1 and the vertex shader writes the original expression
/// for it, so everything that is not corrected is bit-identical to before.
///
/// This patch itself is only the switch and the report — the work is all in the
/// runtime, since a texture coordinate is decided a long way below anything
/// <c>HookManager</c> can reach. The one hook is on <c>DrawOTag</c>, purely to have
/// a frame boundary to count against, and it is a post-hook so it composes with the
/// widescreen patch's replacement of the same function.
/// </summary>
public static class Perspective
{
    // libgpu DrawOTag, per overlay -- the same three addresses NoDither hooks.
    static readonly (string Overlay, uint Addr)[] DrawOTag =
    [
        .. SdkAddr.DrawOTag,
    ];

    /// <summary>Where the choice is kept between runs.</summary>
    public const string OnKey = "kf2.perspective.on";

    /// <summary>False restores the console's affine mapping, warts and all.</summary>
    public static bool Enabled
    {
        get => GteDepth.Enabled;
        private set => GteDepth.Enabled = value;
    }

    /// <summary>KF2_PERSPECTIVE: an explicit choice on the command line, which wins
    /// over the saved setting for the run.</summary>
    static bool? _forced;

    /// <summary>KF2_PERSPECTIVE_PROBE: also write the report to the console.</summary>
    static bool _toConsole;

    /// <summary>KF2_PERSPECTIVE_FALLBACK: keep answering by screen position for the
    /// vertices the exact map missed. Off by default; it exists so the two can be
    /// compared in one build, since the guess is what the map replaced.</summary>
    static bool _fallback;

    static long _frames;
    static double _windowStart;

    static double Now => Environment.TickCount64 / 1000.0;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.perspective",
        Name = "Perspective-correct textures",
        Version = "1.0",
        Description = "Recovers per-vertex depth from the GTE so textures stop swimming.",
    };

    public static void Configure(string? on, string? probe, string? fallback = null)
    {
        if (!string.IsNullOrWhiteSpace(on))
            _forced = !on.Equals("0", StringComparison.Ordinal);

        if (!string.IsNullOrWhiteSpace(probe) && !probe.Equals("0", StringComparison.Ordinal))
            _toConsole = true;

        if (!string.IsNullOrWhiteSpace(fallback) && !fallback.Equals("0", StringComparison.Ordinal))
            _fallback = true;
    }

    public static void Install()
    {
        _windowStart = Now;

        // On from the first projected vertex, before the config file has been read,
        // so the title screen is already correct; RuntimeReadyEvent then applies
        // whatever was saved. ConfigManager only loads inside HostWindow.Initialize,
        // which is after Program.cs -- reading the setting here would read an empty
        // config and write it back over the real one.
        GteDepth.PositionFallback = _fallback;
        Enabled = _forced ?? true;

        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            Enabled = _forced ?? RecompOne.Runtime.Runtime.View.GetBool(OnKey, true);
            Console.WriteLine($"[KF2] perspective: {(Enabled ? "on" : "off (affine)")}");
            // Runs before Pgxp's own listener only by registration order, so this
            // is belt and braces: Pgxp.Reload reads GteDepth.Enabled either way.
            if (Pgxp.Enabled) Pgxp.Reload();
        });

        bool attached = false;
        Event.AddListener<OverlayLoadedEvent>(_ =>
        {
            if (attached || !_toConsole) return;
            attached = true;
            Attach();
        });
    }

    /// <summary>Change the setting at run time. The table starts or stops filling;
    /// a frame drawn during the change is at worst partly corrected.</summary>
    public static void SetEnabled(bool on)
    {
        Enabled = on;

        // The same question, asked once. PGXP keeps its own flag for it and reads
        // it out of the view store on Load, so it has to be told when this moves.
        if (Pgxp.Enabled) Pgxp.Reload();
    }

    // Only attached under the probe: without it there is nothing to count and the
    // patch has no reason to touch the game's code at all.
    static void Attach()
    {
        SymbolRegistry.Build();
        var after = typeof(Perspective).GetMethod(nameof(AfterDrawOTag), BindingFlags.Public | BindingFlags.Static)!;

        int n = 0;
        foreach (var (overlay, addr) in DrawOTag)
        {
            var target = SymbolRegistry.Resolve(overlay, null, addr);
            if (target == null)
            {
                Console.Error.WriteLine($"[KF2] perspective: no function at {overlay}/0x{addr:X8}");
                continue;
            }
            if (HookManager.AddPost(_self, target, after)) n++;
        }

        HookManager.Commit();
        Console.WriteLine($"[KF2] perspective: probe on, {n} hook(s)");
    }

    public static void AfterDrawOTag(CpuContext c, IMemory m)
    {
        _frames++;
        double window = Now - _windowStart;
        if (window < 2.0) return;

        long hits = GteVertexMap.Hits, misses = GteVertexMap.Misses;
        long asked = hits + misses;
        long roots = GteVertexMap.Roots, copied = GteVertexMap.Propagated;
        long recorded = GteDepth.Recorded, clipped = GteDepth.Saturated;

        // The hit rate is the whole measurement, and it now means something exact: a
        // hit is a vertex word whose own address carried its own depth. Roots are the
        // coordinates caught leaving the GTE and copied are the ones followed from
        // there into a packet, so the two together say whether the association is
        // being made or only started. A rate near zero would mean the copy is not the
        // one this follows and every polygon quietly stayed affine.
        string fallback = GteDepth.PositionFallback
            ? $", {GteDepth.Hits / window:F0} by position/s"
            : "";

        Console.WriteLine($"[KF2] perspective: {recorded / window:F0} vertices projected/s, " +
                          $"{roots / window:F0} caught/s, {copied / window:F0} copied/s, " +
                          $"{asked / window:F0} looked up/s, {(asked == 0 ? 0.0 : 100.0 * hits / asked):F1}% hit, " +
                          $"{clipped / window:F0} saturated/s{fallback}, over {_frames / window:F0} frames/s");

        GteVertexMap.ResetCounters();
        GteDepth.ResetCounters();
        _frames = 0;
        _windowStart = Now;
    }
}
