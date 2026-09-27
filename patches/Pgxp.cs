using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;
using Rp = RecompOne.Runtime.Pgxp;
using Rt = RecompOne.Runtime.Runtime;

namespace Kf2;

/// <summary>
/// PGXP — the second, and better, way of recovering what the console threw away.
///
///     KF2_PGXP=1                 use PGXP instead of the address map
///     KF2_PGXP_TEXTURE=0         its share of perspective correction off
///     KF2_PGXP_CULLING=0         leave backface culling on truncated positions
///     KF2_PGXP_CPU=0             no per-instruction register tracking
///     KF2_PGXP_MEMORY=0          no RAM shadow
///     KF2_PGXP_VERTEXCACHE=1     the screen-position cache of last resort
///     KF2_PGXP_CACHEW=0          let that cache answer positions but not depths
///     KF2_PGXP_TOLERANCE=2       how far a recovered position may sit from the
///                                packet's before it is refused; -1 turns it off
///     KF2_PGXP_PROBE=1           report the coverage, against the address map's
///
/// **The port has two mechanisms for one number and this is the switch between
/// them.** Both answer the same question — what were this vertex's true screen
/// position and view depth, before the GTE truncated the first and discarded the
/// second — and both answer it in the only place the two are ever in hand at once,
/// <c>Gte.Rtp</c>. What differs is how the answer is carried from there to the GP0
/// packet the GPU actually draws:
///
///   * <see cref="GteVertexMap"/> (<c>patches/recompone/0012</c>) watches
///     <c>PSMemory</c>'s word reads and writes and pairs them by value in a small
///     ring. It needs no recompile, and it is a guess about registers made without
///     being able to see any: a coordinate the game <em>computes</em> rather than
///     copies is lost, and a lost vertex is a polygon with no depth.
///   * PGXP (upstream RecompOne, backported as <c>0034</c>–<c>0036</c>) tracks the
///     value through the registers themselves, with hooks the recompiler emits
///     beside every load, store, move, shift, add and multiply. Nothing is
///     inferred, so the coverage is not a rate that happens to be high — it is
///     every path the value can take.
///
/// **Coverage was the reason this was taken and coverage is not what it bought.**
/// Measured in area 2 at 144 fps, over eight windows each: the address map answers
/// for 92.2-97.1% of vertices and PGXP for 93.4-96.7%, which is the same number.
/// King's Field assembles its packets with whole-word <c>lw</c>/<c>sw</c> out of a
/// transform cache — the one shape a value-matching ring follows perfectly — so
/// there was very little for register tracking to find. What it costs is real:
/// **144.0 fps against 106.7-114.9**, all of it in the emitted hooks
/// (<c>KF2_PGXP_CPU=0</c> reads 144.0 again, and 79.2-85.7% coverage from the
/// screen-position cache alone, since <c>PgxpMemory.Store</c> is only ever reached
/// from <c>PgxpCpu</c>).
///
/// **It is off, not off-by-default, and it has no control in the settings
/// window.** A mechanism whose picture nobody has judged, which buys no coverage
/// here and costs a fifth of the frame rate, is a comparison rather than a
/// setting -- so it went the way the map's style and the widescreen ticks went,
/// out of the pane and onto the console. <c>KF2_PGXP=1</c> is the only way in and
/// the saved key is no longer read, so a config that ticked it while upstream's
/// PGXP block was drawn is not left running slow with nothing to explain it.
///
/// It is kept, for what it has that the ring cannot:
/// backface culling decided on precise positions rather than truncated ones, a
/// projection in floats rather than a recovered fraction, and coverage that is a
/// property of the mechanism instead of a rate that happens to be high for this
/// game's packet assembly. **The number that actually fixed the Z-buffer was the
/// clip W** (see <c>GpuHleForward.HleTri</c>), and that helps both sources.
///
/// The comparison is one switch, live, in the same build.
/// </summary>
public static class Pgxp
{
    // libgpu DrawOTag, per overlay -- the same three addresses Perspective hooks.
    static readonly (string Overlay, uint Addr)[] DrawOTag =
    [
        .. SdkAddr.DrawOTag,
    ];

    /// <summary>
    /// PGXP's settings are kept under **upstream's own keys**, not the port's
    /// <c>kf2.&lt;patch&gt;.&lt;name&gt;</c> convention, because
    /// <c>RecompOne.Runtime.Pgxp.Pgxp.Load()</c> reads them itself and the file it
    /// reads is the same <c>interface.ini</c> the port writes. Giving them port
    /// names would mean either editing an upstream file that is otherwise verbatim
    /// or keeping two copies of every value in step.
    /// </summary>
    public const string OnKey = Rp.Pgxp.KeyEnable;

    /// <summary>False leaves <see cref="GteVertexMap"/> answering, which is what
    /// every measurement in this port so far was taken against.</summary>
    public static bool Enabled => Rp.Pgxp.Enabled;

    static bool? _forcedOn, _forcedTexture, _forcedCulling, _forcedCpu, _forcedMemory, _forcedCache, _forcedCacheW;
    static float? _forcedTolerance;

    /// <summary>KF2_PGXP_PROBE: also write the report to the console.</summary>
    static bool _toConsole;

    static long _frames;
    static double _windowStart;

    static double Now => Environment.TickCount64 / 1000.0;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.pgxp",
        Name = "PGXP",
        Version = "1.0",
        Description = "Per-vertex position and depth tracked through the CPU, not guessed from memory traffic.",
    };

    public static void Configure(string? on, string? texture, string? culling, string? cpu, string? memory,
                                 string? cache, string? cacheW, string? tolerance, string? probe)
    {
        _forcedOn = Flag(on);
        _forcedTexture = Flag(texture);
        _forcedCulling = Flag(culling);
        _forcedCpu = Flag(cpu);
        _forcedMemory = Flag(memory);

        // Enabling PGXP is env-only, so the CPU hooks are decided here, before any
        // recompiled code is compiled; read the gate now so it is fixed before then.
        Rp.Pgxp.CpuHooksArmed = _forcedOn == true && _forcedCpu != false;
        _ = Rp.PgxpGate.Cpu;
        _forcedCache = Flag(cache);
        _forcedCacheW = Flag(cacheW);

        if (!string.IsNullOrWhiteSpace(tolerance) &&
            float.TryParse(tolerance, System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out float t))
            _forcedTolerance = t;

        if (!string.IsNullOrWhiteSpace(probe) && !probe.Equals("0", StringComparison.Ordinal))
            _toConsole = true;
    }

    static string Pct(long n, long total) => $"{(total == 0 ? 0.0 : 100.0 * n / total):F1}%";

    static bool? Flag(string? v) =>
        string.IsNullOrWhiteSpace(v) ? null : !v.Equals("0", StringComparison.Ordinal);

    public static void Install()
    {
        _windowStart = Now;

        // Off until the config has been read. ConfigManager only loads inside
        // HostWindow.Initialize, which is after Program.cs, so reading the setting
        // here would read an empty config and then write it back over the real one.
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            Reload();
            Console.WriteLine($"[KF2] pgxp: {Status}");
        });

        bool attached = false;
        Event.AddListener<OverlayLoadedEvent>(_ =>
        {
            if (attached || !_toConsole) return;
            attached = true;
            Attach();
        });
    }

    /// <summary>
    /// Re-read every PGXP setting out of the view store, with the environment
    /// winning for this run.
    ///
    /// The forcing is done by writing the environment's answer into the store,
    /// loading, and putting the store back the way it was. That looks roundabout
    /// and it is deliberate: <c>Pgxp.Load()</c> is upstream's, its properties are
    /// private-set, and the alternative — leaving the forced value in the store —
    /// would let <c>KF2_PGXP=1</c> on one run silently become the saved setting the
    /// moment anything else calls SaveView. Nothing is written to disk here.
    /// </summary>
    public static void Reload()
    {
        var view = Rt.View;

        // Texture correction is not a setting of PGXP's here. The port already asks
        // that question once, under Enhancements, and it means the same thing
        // whichever mechanism is answering -- so PGXP is told what that switch says
        // rather than given a second tick of its own. KF2_PGXP_TEXTURE still
        // overrides it, for isolating one half of PGXP from the other.
        (string Key, bool? Forced)[] flags =
        [
            // Deliberately not read out of the store. PGXP has no control in the
            // window any more, and a saved `true` with nothing to untick it is a
            // session running a fifth slower for no reason the player can see --
            // the same trap kf2.framepacing.logichz was retired for. KF2_PGXP=1
            // is the comparison, and it is the only way in.
            (Rp.Pgxp.KeyEnable, _forcedOn ?? false),
            (Rp.Pgxp.KeyTextureCorrection, _forcedTexture ?? GteDepth.Enabled),
            (Rp.Pgxp.KeyCulling, _forcedCulling),
            (Rp.Pgxp.KeyCpu, _forcedCpu),
            (Rp.Pgxp.KeyMemory, _forcedMemory),
            (Rp.Pgxp.KeyVertexCache, _forcedCache),
            (Rp.Pgxp.KeyCacheW, _forcedCacheW),
        ];

        var saved = new List<(string Key, bool Had, string Value)>();

        foreach (var (key, forced) in flags)
        {
            if (forced == null) continue;
            saved.Add((key, view.Values.TryGetValue(key, out var old), old ?? ""));
            view.SetBool(key, forced.Value);
        }

        if (_forcedTolerance != null)
        {
            saved.Add((Rp.Pgxp.KeyTolerance,
                view.Values.TryGetValue(Rp.Pgxp.KeyTolerance, out var oldTol), oldTol ?? ""));
            view.SetFloat(Rp.Pgxp.KeyTolerance, _forcedTolerance.Value);
        }

        Rp.Pgxp.Load();

        foreach (var (key, had, value) in saved)
        {
            if (had) view.Values[key] = value;
            else view.Values.Remove(key);
        }

        // The vertex cache is 64 MB and allocated lazily; give it back the moment
        // it is switched off rather than at the next restart.
        if (!Rp.Pgxp.VertexCache) Rp.PgxpGpu.Free();

        Rp.PgxpGte.Invalidate();
    }

    /// <summary>One line for the console and the log: which mechanism is answering
    /// and, if it is this one, what it has been allowed to use.</summary>
    public static string Status
    {
        get
        {
            if (!Rp.Pgxp.Enabled) return "off (the address map answers)";

            var parts = new List<string>();
            if (Rp.Pgxp.TextureCorrection) parts.Add("texture");
            if (Rp.Pgxp.CullingCorrection) parts.Add("culling");
            if (Rp.Pgxp.CpuTracking) parts.Add("cpu");
            if (Rp.Pgxp.MemoryTracking) parts.Add("memory");
            if (Rp.Pgxp.VertexCache) parts.Add(Rp.Pgxp.CacheW ? "cache+w" : "cache");

            string tol = Rp.Pgxp.Tolerance < 0f ? "no tolerance" : $"tolerance {Rp.Pgxp.Tolerance:0.##}px";
            return $"on ({string.Join(", ", parts)}; {tol})";
        }
    }

    // Only attached under the probe: without it there is nothing to count.
    static void Attach()
    {
        SymbolRegistry.Build();
        var after = typeof(Pgxp).GetMethod(nameof(AfterDrawOTag), BindingFlags.Public | BindingFlags.Static)!;

        int n = 0;
        foreach (var (overlay, addr) in DrawOTag)
        {
            var target = SymbolRegistry.Resolve(overlay, null, addr);
            if (target == null)
            {
                Console.Error.WriteLine($"[KF2] pgxp: no function at {overlay}/0x{addr:X8}");
                continue;
            }
            if (HookManager.AddPost(_self, target, after)) n++;
        }

        HookManager.Commit();
        Console.WriteLine($"[KF2] pgxp: probe on, {n} hook(s)");
    }

    public static void AfterDrawOTag(CpuContext c, IMemory m)
    {
        _frames++;
        double window = Now - _windowStart;
        if (window < 2.0) return;

        long asked = Rp.PgxpStats.Asked, hits = Rp.PgxpStats.Hits;

        // The hit rate is the measurement this backport exists to move, so it is
        // reported the same way Perspective reports the address map's: vertices
        // asked about a second, and the share of them that were answered. Where the
        // answer came from matters as much as whether there was one -- the RAM
        // shadow is exact, the screen-position cache is the same kind of guess the
        // address map was replaced for -- so the two are counted apart.
        Console.WriteLine($"[KF2] pgxp: {asked / window:F0} vertices looked up/s, " +
                          $"{(asked == 0 ? 0.0 : 100.0 * hits / asked):F1}% hit " +
                          $"({Rp.PgxpStats.FromMemory / window:F0} exact/s, " +
                          $"{Rp.PgxpStats.FromCache / window:F0} cached/s, " +
                          $"{Rp.PgxpStats.Resolved / window:F0} disambiguated/s), " +
                          $"{Rp.PgxpStats.Refused / window:F0} refused/s, " +
                          $"{Rp.PgxpStats.NoDepth / window:F0} without depth/s, " +
                          $"over {_frames / window:F0} frames/s");

        // The tolerance census. A guard is only pickable if the two populations it
        // separates are actually separate, so both are printed side by side: a
        // vertex recovered more precisely can only disagree by the fraction the
        // GTE truncated (under one pixel), and a vertex recovered *wrongly* can
        // disagree by anything. Choose a tolerance in the gap. No gap means the
        // guard is not doing what it says and the number is arbitrary.
        var d = Rp.PgxpStats.Disagree;
        long total = 0;
        foreach (var n in d) total += n;

        if (total > 0)
            Console.WriteLine($"[KF2] pgxp: disagreement with the packet, {total / window:F0}/s: " +
                              $"{Pct(d[0], total)} under 0.5px, {Pct(d[1], total)} under 1, " +
                              $"{Pct(d[2], total)} under 2, {Pct(d[3], total)} under 4, " +
                              $"{Pct(d[4], total)} under 8, {Pct(d[5], total)} beyond; " +
                              $"widest {Rp.PgxpStats.DisagreeMax:F2}px");

        Rp.PgxpStats.Reset();
        _frames = 0;
        _windowStart = Now;
    }
}
