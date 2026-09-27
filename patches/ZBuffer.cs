using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// A Z-buffer — per-pixel occlusion from the view depth the GTE already computed,
/// instead of the console's painter's algorithm.
///
///     KF2_ZBUFFER=0             off; unset is on (the shipped default)
///     KF2_ZBUFFER_PROBE=1       report how many triangles actually depth-tested
///     KF2_BLENDORDER=0          blended surfaces in table order again (0079)
///     KF2_BLENDORDER_PROBE=1    opaque geometry drawn over a nearer translucent surface
///
/// The PlayStation GPU has no depth buffer. The game sorts every polygon into an
/// ordering table by one number, the GTE's OTZ (the average of its vertices), and
/// <c>DrawOTag</c> walks that table back to front. Two surfaces that actually
/// interpenetrate — a floor meeting a wall, two cave rocks crossing — can only
/// take turns in front of each other, because each polygon is wholly in front or
/// wholly behind. That flicker is what this turns off.
///
/// The depth is the same SZ3 <see cref="Perspective"/> recovers for the texture:
/// caught at <c>Gte.Rtp</c>, followed through memory by <see cref="GteVertexMap"/>,
/// and asked for again by the address <c>DrawOTag</c> read the vertex word from.
/// Both renderers interpolate it per pixel (1/Z is linear in screen space for a
/// plane) and keep the pixel if it is closer than what is already there. Equal
/// depths still prefer the later table entry, so coplanar surfaces that the game
/// stacked on purpose keep the console's order.
///
/// A miss is the old behaviour, which is what makes this safe to leave available:
///
///   * The <b>HUD and the menus</b> never went through the GTE, so they never
///     hit, so they still draw on top in table order with the depth test off.
///   * A triangle is all-or-nothing. One vertex left without a depth would punch
///     a hole through the surface, so that triangle keeps painter's order rather
///     than testing.
///   * Semi-transparent primitives test against Z but do not write it, so two
///     overlapping additives still blend in the order the table named. They are
///     drawn after the opaque ones that follow them in the table (0079), which
///     would otherwise paint over them.
///   * Untextured geometry is tested too. Perspective correction only cares
///     about textured polygons; a flat-shaded wall still has a view depth.
///
/// Both renderers honour it. The hardware backend attaches a 24-bit depth buffer
/// to each display render target and writes window depth from SZ in the fragment
/// shader, so OpenGL does not clip the already-projected triangle; the software
/// rasterizer keeps a float per VRAM pixel and tests it in the same inner loop
/// that plots. Turning the setting off is an exact no-op: the attachment is
/// never tested, the clip W is unchanged, and a vertex with no depth still
/// writes <c>vec4(p, 0, 1)</c>.
///
/// <b>On by default</b>, under Video ▸ Enhancements, with sliders for the two
/// coplanar-tolerance terms. A miss is still painter's order. See "Z-buffer"
/// in NOTES.md.
///
/// This patch is only the console switch and the report;
/// the work is in the runtime (<c>patches/recompone/0014</c>), because a pixel's
/// depth is decided far below anything <c>HookManager</c> can reach. The one hook
/// is on <c>DrawOTag</c>, purely to have a frame boundary to count against, and
/// it is a post-hook so it composes with the widescreen patch's replacement of
/// the same function.
/// </summary>
public static class ZBuffer
{
    static readonly (string Overlay, uint Addr)[] DrawOTag =
    [
        .. SdkAddr.DrawOTag,
    ];

    /// <summary>Where the choice is kept between runs.</summary>
    public const string OnKey = "kf2.zbuffer.on";

    /// <summary>The coplanar tolerance's two terms, kept between runs (Video ▸ Enhancements).</summary>
    public const string BiasKey = "kf2.zbuffer.bias", SlopeKey = "kf2.zbuffer.slope";
    public const float DefaultBias = 1f, DefaultSlope = 0.5f;

    /// <summary>False leaves occlusion to the ordering table, as the console does.</summary>
    public static bool Enabled
    {
        get => GteDepth.ZBuffer;
        private set => GteDepth.ZBuffer = value;
    }

    /// <summary>KF2_ZBUFFER: an explicit choice on the command line, which wins
    /// over the saved setting for the run.</summary>
    static bool? _forced;

    /// <summary>KF2_ZBUFFER_PROBE: also write the report to the console.</summary>
    static bool _toConsole;

    static long _frames;
    static double _windowStart;

    static double Now => Environment.TickCount64 / 1000.0;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.zbuffer",
        Name = "Z-buffer",
        Version = "1.0",
        Description = "Per-pixel occlusion from recovered GTE depth, instead of the ordering table.",
    };

    /// <summary>KF2_ZBUFFER_PROBE=2: also take the frame's occlusion census — which
    /// large primitive is standing in front of which, and how much of the picture
    /// that costs.</summary>
    static bool _census;

    /// <summary>KF2_ZBUFFER_THRESHOLD: how far the scene may jump towards the
    /// camera between two primitives before the depth buffer is started again.
    /// Zero or less is one buffer for the whole frame.</summary>
    static float? _forcedThreshold;

    /// <summary>KF2_ZBUFFER_SOURCE: the address map's depth (the default in King's
    /// Field, whose polygon assemblers are not in C# yet), or <c>packet</c> for the
    /// depth the C# assemblers record, which was King's Field II's default.</summary>
    static bool _packetSource;

    /// <summary>KF2_ZBUFFER_BIAS and KF2_ZBUFFER_SLOPE: the coplanar tolerance.</summary>
    static float? _forcedBias, _forcedSlope;

    /// <summary>KF2_BLENDORDER_PROBE: the blend order's line.</summary>
    static bool _blendProbe;

    public static void Configure(string? on, string? probe, string? threshold = null, string? source = null,
                                 string? bias = null, string? slope = null, string? blendOrder = null,
                                 string? blendProbe = null)
    {
        BlendOrder.Enabled = blendOrder?.Trim() != "0";
        _blendProbe = !string.IsNullOrWhiteSpace(blendProbe) && blendProbe.Trim() != "0";
        BlendOrder.Probe = _blendProbe;

        _packetSource = source?.Trim().ToLowerInvariant() == "packet";
        _forcedBias = ParseFloat(bias);
        _forcedSlope = ParseFloat(slope);

        if (!string.IsNullOrWhiteSpace(on))
            _forced = !on.Equals("0", StringComparison.Ordinal);

        if (!string.IsNullOrWhiteSpace(threshold) &&
            float.TryParse(threshold, System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out float t))
            _forcedThreshold = t;

        if (!string.IsNullOrWhiteSpace(probe) && !probe.Equals("0", StringComparison.Ordinal))
        {
            _toConsole = true;
            _census = !probe.Equals("1", StringComparison.Ordinal);
            GteDepth.TriCensus = _census;
        }
    }

    /// <summary>The first-person arm, which keeps painter's order. KF2's was
    /// 0x80032400; King's Field's has not been found, and 0 attaches nothing.</summary>
    const uint ArmDraw = 0;

    /// <summary>The model submitter: a blended packet built inside it is a solid
    /// surface for the occlusion pass (the secret door), where the blended map
    /// tiles (water) and the billboards (flames) are not. KF2's was 0x80032588;
    /// not found in King's Field yet, and 0 attaches nothing.</summary>
    const uint ModelSubmit = 0;

    /// <summary>The packet source needs the C# assemblers to write it.</summary>
    public static void SyncSource() =>
        GtePacketDepth.Enabled = _packetSource && PolyAssembler.Enabled && PolyAssembler.TransformEnabled;

    public static void Install()
    {
        _windowStart = Now;
        SyncSource();
        GteDepth.DepthBias = _forcedBias ?? DefaultBias;
        GteDepth.DepthSlope = _forcedSlope ?? DefaultSlope;

        // RuntimeReadyEvent is the first and only place the saved setting is
        // decided. ConfigManager only loads inside HostWindow.Initialize, which
        // is after Program.cs, so reading it here would read an empty config and
        // write it back over the real one.
        Enabled = _forced ?? true;

        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            Enabled = _forced ?? RecompOne.Runtime.Runtime.View.GetBool(OnKey, true);
            // Env only: its slider is gone, so an old saved kf2.zbuffer.threshold is not read.
            GteDepth.DepthClearThreshold = _forcedThreshold ?? 0f;
            SyncSource();
            GteDepth.DepthBias = _forcedBias ?? RecompOne.Runtime.Runtime.View.GetFloat(BiasKey, DefaultBias);
            GteDepth.DepthSlope = _forcedSlope ?? RecompOne.Runtime.Runtime.View.GetFloat(SlopeKey, DefaultSlope);
            Console.WriteLine($"[KF2] zbuffer: {(Enabled ? "on" : "off (ordering table)")}" +
                              $", depth from {(!_packetSource ? "the address map" : GtePacketDepth.Enabled ? "the assemblers" : "the assemblers, which are off")}" +
                              $", tolerance {GteDepth.DepthBias:0.##} SZ + {GteDepth.DepthSlope:0.##} px of slope" +
                              $", clear threshold {(GteDepth.DepthClearThreshold <= 0f ? "off" : GteDepth.DepthClearThreshold.ToString("0"))}");
        });

        if (ArmDraw != 0) HookAttach.OnOverlayLoad("zbuffer arm", AttachArm);
        if (ModelSubmit != 0 && Environment.GetEnvironmentVariable("KF2_AO_SOLID") != "0")
            HookAttach.OnOverlayLoad("zbuffer models", AttachModel);

        bool attached = false;
        Event.AddListener<OverlayLoadedEvent>(_ =>
        {
            if (attached || !(_toConsole || _blendProbe)) return;
            attached = true;
            Attach();
        });
    }

    static float? ParseFloat(string? s) =>
        float.TryParse(s, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float f)
            ? Math.Max(f, 0f) : null;

    static bool _armQueued;

    static bool AttachArm()
    {
        SymbolRegistry.Build();
        var target = SymbolRegistry.Resolve("game", null, ArmDraw);
        if (target == null) return false;
        if (!_armQueued)
        {
            const BindingFlags flags = BindingFlags.Public | BindingFlags.Static;
            HookManager.AddPre(_self, target, typeof(ZBuffer).GetMethod(nameof(BeforeArm), flags)!);
            HookManager.AddPost(_self, target, typeof(ZBuffer).GetMethod(nameof(AfterArm), flags)!);
            _armQueued = true;
        }
        HookManager.Commit();
        return HookAttach.Installed(target);
    }

    static bool _modelQueued;

    static bool AttachModel()
    {
        SymbolRegistry.Build();
        var target = SymbolRegistry.Resolve("game", null, ModelSubmit);
        if (target == null) return false;
        if (!_modelQueued)
        {
            const BindingFlags flags = BindingFlags.Public | BindingFlags.Static;
            HookManager.AddPre(_self, target, typeof(ZBuffer).GetMethod(nameof(BeforeModel), flags)!);
            HookManager.AddPost(_self, target, typeof(ZBuffer).GetMethod(nameof(AfterModel), flags)!);
            _modelQueued = true;
        }
        HookManager.Commit();
        return HookAttach.Installed(target);
    }

    public static void BeforeModel(CpuContext c, IMemory m) => PolyAssembler.InModel = true;
    public static void AfterModel(CpuContext c, IMemory m) => PolyAssembler.InModel = false;

    public static void BeforeArm(CpuContext c, IMemory m) => PolyAssembler.InArm = true;
    public static void AfterArm(CpuContext c, IMemory m) => PolyAssembler.InArm = false;

    /// <summary>Change the setting at run time. The next triangle starts or stops
    /// testing; a frame drawn during the change is at worst partly sorted, which
    /// is a frame of the flicker the setting is about.</summary>
    static string ZPct(long n, long total) => $"{(total == 0 ? 0.0 : 100.0 * n / total):F1}%";

    public static void SetEnabled(bool on) => Enabled = on;

    static void Attach()
    {
        SymbolRegistry.Build();
        var after = typeof(ZBuffer).GetMethod(nameof(AfterDrawOTag), BindingFlags.Public | BindingFlags.Static)!;

        int n = 0;
        foreach (var (overlay, addr) in DrawOTag)
        {
            var target = SymbolRegistry.Resolve(overlay, null, addr);
            if (target == null)
            {
                Console.Error.WriteLine($"[KF2] zbuffer: no function at {overlay}/0x{addr:X8}");
                continue;
            }
            if (HookManager.AddPost(_self, target, after)) n++;
        }

        HookManager.Commit();
        Console.WriteLine($"[KF2] zbuffer: probe on, {n} hook(s)");
    }

    public static void AfterDrawOTag(CpuContext c, IMemory m)
    {
        _frames++;
        double window = Now - _windowStart;
        if (window < 2.0) { if (_census) GteDepth.ResetCensus(); return; }

        if (_blendProbe) ReportBlendOrder(window);
        if (!_toConsole) { _frames = 0; _windowStart = Now; return; }

        if (_census) { ReportCensus(); GteDepth.ResetCensus(); }

        long tested = GteDepth.ZTris, skipped = GteDepth.ZSkipped, rejected = GteDepth.ZRejects;
        long total = tested + skipped;

        // The tested rate is the measurement: it is the only thing that says the
        // recovered depth is actually reaching the rasterizer. A rate near zero
        // would mean every triangle quietly kept painter's order. Skipped is 2D
        // and anything the map missed; rejects are software-rasterizer pixels that
        // lost, and stay at zero on the hardware path.
        Console.WriteLine($"[KF2] zbuffer: {tested / window:F0} tris/s tested, " +
                          $"{skipped / window:F0} painter's/s" +
                          $"{(total == 0 ? "" : $", {(100.0 * tested / total):F1}% of submitted")}" +
                          $"{(rejected == 0 ? "" : $", {rejected / window:F0} px rejected/s")}, " +
                          $"over {_frames / window:F0} frames/s");

        if (GtePacketDepth.Active)
        {
            long hits = GtePacketDepth.Hits, misses = GtePacketDepth.Misses;
            Console.WriteLine($"[KF2] zbuffer: {GtePacketDepth.Recorded / window:F0} packet depths recorded/s, " +
                              $"{hits / window:F0} polygons found theirs/s, {misses / window:F0} had none/s " +
                              $"({ZPct(hits, hits + misses)}), {PolyAssembler.DepthClipMismatches / window:F0} clipped packets unmatched/s, " +
                              $"{GteDepth.ZPrepasses / window:F0} depth prepasses/s, " +
                              $"blended model packets/s: object {PolyAssembler.BlendedByKind[1] / window:F0} ({PolyAssembler.SolidPackets / window:F0} solid), " +
                              $"creature {PolyAssembler.BlendedByKind[0] / window:F0}, effect {PolyAssembler.BlendedByKind[2] / window:F0}, " +
                              $"sprite {PolyAssembler.BlendedByKind[3] / window:F0}, " +
                              $"corners unrounded {PolyAssembler.DepthUnrounded / window:F0}/s, left whole {PolyAssembler.DepthWhole / window:F0}/s");
            GteDepth.ZPrepasses = 0;
            PolyAssembler.DepthUnrounded = PolyAssembler.DepthWhole = 0;
            Array.Clear(PolyAssembler.BlendedByKind);
            PolyAssembler.SolidPackets = 0;
            GtePacketDepth.ResetCounters();
            PolyAssembler.DepthClipMismatches = 0;
        }

        // The depth-clear census. The threshold is only worth a nonzero value if
        // this game has two populations of forward step — ordinary sorting inside
        // one scene, and a scene the game started again under the same projection.
        // If every drop is small and the clear never fires, the right threshold is
        // 0; if it fires in steady play, it is cutting the world's own depth
        // buffer in half mid-frame, which is worse than not having one.
        var drops = GteDepth.ZDrops;
        long dropTotal = 0;
        foreach (var n in drops) dropTotal += n;

        if (dropTotal > 0)
            Console.WriteLine($"[KF2] zbuffer: {GteDepth.ZClears / window:F1} depth clear(s)/s at threshold " +
                              $"{(GteDepth.DepthClearThreshold <= 0f ? "off" : GteDepth.DepthClearThreshold.ToString("0"))}; " +
                              $"forward steps {dropTotal / window:F0}/s: " +
                              $"{ZPct(drops[0], dropTotal)} under 10, {ZPct(drops[1], dropTotal)} under 50, " +
                              $"{ZPct(drops[2], dropTotal)} under 150, {ZPct(drops[3], dropTotal)} under 300, " +
                              $"{ZPct(drops[4], dropTotal)} under 1000, {ZPct(drops[5], dropTotal)} beyond; " +
                              $"widest {GteDepth.ZDropMax:F0}");

        GteDepth.ResetZCounters();
        _frames = 0;
        _windowStart = Now;
    }

    static long _deferred, _flushes, _passed, _walks, _overModel, _overTile, _opaque, _translucent;

    /// <summary>0079's line: what was held and how often the queue was cut, then what the
    /// GPU was sent in which order, sampled on a 4-pixel grid. An opaque sample drawn
    /// behind a nearer translucent one is geometry painting over the water.</summary>
    static void ReportBlendOrder(double window)
    {
        long walks = BlendOrder.Walks - _walks;
        long over = BlendOrder.OverModel - _overModel, overTile = BlendOrder.OverTile - _overTile;
        long opaque = BlendOrder.OpaqueSamples - _opaque, translucent = BlendOrder.TranslucentSamples - _translucent;
        double perWalk(long n) => walks == 0 ? 0 : (double)n / walks;
        Console.WriteLine($"[KF2] blend order: {(BlendOrder.Active ? "on" : BlendOrder.Enabled ? "on, idle (needs the Z-buffer and Fast geometry)" : "off")}; " +
                          $"a walk held {perWalk(BlendOrder.Deferred - _deferred):F1} packets past {perWalk(BlendOrder.Passed - _passed):F1} opaque, " +
                          $"sent in {perWalk(BlendOrder.Flushes - _flushes):F1} runs; " +
                          $"samples a walk: translucent {perWalk(translucent):F0}, opaque {perWalk(opaque):F0}, " +
                          $"opaque over a nearer translucent: models {perWalk(over):F1}, tiles {perWalk(overTile):F1}");
        _deferred = BlendOrder.Deferred; _flushes = BlendOrder.Flushes; _passed = BlendOrder.Passed; _walks = BlendOrder.Walks;
        _overModel = BlendOrder.OverModel; _overTile = BlendOrder.OverTile;
        _opaque = BlendOrder.OpaqueSamples; _translucent = BlendOrder.TranslucentSamples;
    }

    /// <summary>
    /// One frame's occlusion census, for the question a rate cannot answer: when a
    /// depth-tested surface disappears, <i>what is standing in front of it</i>.
    ///
    /// <c>DrawOTag</c> walks back to front, so a primitive submitted earlier is one
    /// the game itself sorted as farther away. An early entry whose recovered depth
    /// puts it entirely in front of a later one is therefore a disagreement between
    /// the depth this port recovered and the game's own OTZ sort — and wherever the
    /// two disagree over a wide area, the later surface vanishes and the picture
    /// shows the earlier one instead. Blockers are ranked by how much of the picture
    /// they take that way, which is the hole, in pixels.
    /// </summary>
    static void ReportCensus()
    {
        var all = GteDepth.Census.ToArray();
        int tested = 0, clamped = 0, bigCount = 0;
        float near = float.MaxValue, far = 0f;
        foreach (var t in all)
        {
            if (t.Clamped) clamped++;
            if (t.Area >= GteDepth.CensusBigArea) bigCount++;
            if (!t.Tested) continue;
            tested++;
            if (t.MinZ < near) near = t.MinZ;
            if (t.MaxZ > far) far = t.MaxZ;
        }

        Console.WriteLine($"[KF2] zbuffer census: {GteDepth.CensusSubmitted} polys submitted, " +
                          $"{all.Length} kept ({bigCount} at least {GteDepth.CensusBigArea}px, " +
                          $"{tested} depth-tested, {clamped} with a clamped corner)" +
                          $"{(GteDepth.CensusDropped == 0 ? "" : $", {GteDepth.CensusDropped} dropped")}" +
                          $"{(tested == 0 ? "" : $", z {near:F0}..{far:F0}")}" +
                          $", table {GteDepth.OtLength} entries");

        // The head of the table in submission order. A skybox lives here: linked at
        // the far end so it draws first, and projecting near because it is a small
        // box around the camera. `ot` against `z` is the disagreement, in one line.
        int heads = 0;
        foreach (var t in all)
        {
            if (t.Area < GteDepth.CensusBigArea) continue;
            Console.WriteLine("    head " + Describe(t));
            if (++heads == 3) break;
        }

        // The frame's largest surfaces, whatever their table position. A skybox is
        // by construction one of these — it is the thing behind everything, so it
        // covers whatever the world does not — and this is the line that says
        // whether it depth-tested and at what depth. Sorted by area, so it does not
        // matter where in the table it turned up.
        var byArea = new int[all.Length];
        for (int i = 0; i < byArea.Length; i++) byArea[i] = i;
        Array.Sort(byArea, (a, b) => all[b].Area.CompareTo(all[a].Area));
        for (int i = 0; i < byArea.Length && i < 6; i++)
        {
            if (all[byArea[i]].Area < GteDepth.CensusBigArea) break;
            Console.WriteLine("    largest " + Describe(all[byArea[i]]));
        }

        // Entirely in front of, and overlapping: the later polygon loses every pixel
        // the two share. Every polygon is a candidate victim regardless of size — a
        // wall at distance is a crowd of small triangles, and one large surface
        // standing in front of the crowd is exactly the shape of a hole. Partial
        // overlaps in depth are left out on purpose: those are the interpenetrations
        // the Z-buffer exists to sort out.
        var cost = new long[all.Length];
        var hides = new int[all.Length];
        long hiddenArea = 0;
        int hiddenCount = 0;
        for (int i = 0; i < all.Length; i++)
        {
            if (!all[i].Tested) continue;
            bool lost = false;
            for (int j = 0; j < all.Length; j++)
            {
                if (j == i || !all[j].Tested) continue;
                // j is earlier in the walk, so the game sorted it as farther away.
                if (j > i) continue;
                if (all[j].MaxZ >= all[i].MinZ || !all[j].Overlaps(all[i])) continue;
                long w = Math.Min(all[j].X1, all[i].X1) - Math.Max(all[j].X0, all[i].X0) + 1;
                long h = Math.Min(all[j].Y1, all[i].Y1) - Math.Max(all[j].Y0, all[i].Y0) + 1;
                cost[j] += w * h;
                hides[j]++;
                lost = true;
            }
            if (lost) { hiddenCount++; hiddenArea += all[i].Area; }
        }

        var rank = new int[all.Length];
        for (int i = 0; i < rank.Length; i++) rank[i] = i;
        Array.Sort(rank, (a, b) => cost[b].CompareTo(cost[a]));

        int shown = 0;
        foreach (int i in rank)
        {
            if (cost[i] == 0 || shown++ == 6) break;
            Console.WriteLine($"    hides {Describe(all[i])} " +
                              $"-> in front of {hides[i]} later, {cost[i]}px hidden");
        }

        if (shown == 0)
            Console.WriteLine("    nothing is entirely in front of anything the table put nearer — " +
                              "the hole is not one primitive winning the depth test");
        else
            Console.WriteLine($"    {hiddenCount} polys ({hiddenArea}px of bbox) lose ground to " +
                              "something the table sorted farther away");

        ReportDepthMap();
        GteDepth.WantDepthMap = true;
    }

    /// <summary>
    /// The depth buffer the frame actually ended up with, as a picture made of
    /// digits: the nearest surface in each cell of the screen, on a scale from the
    /// frame's own nearest to its farthest. `.` is a cell nothing depth-tested at
    /// all — 2D, or geometry that stayed on painter's order.
    ///
    /// This is the one measurement that does not depend on reasoning about what
    /// should have happened. A hole in the picture with a *near* digit sitting in it
    /// is something claiming the foreground there; a hole with `.` in it was never
    /// depth-tested and the Z-buffer is not what emptied it.
    /// </summary>
    static void ReportDepthMap()
    {
        var map = GteDepth.DepthMap;
        if (map == null) return;

        float lo = float.MaxValue, hi = 0f;
        foreach (float v in map)
        {
            if (v >= GteDepth.DepthMapEmpty) continue;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        Console.WriteLine($"    depth batches: {GteDepth.ZBatchRt} to a target with a depth buffer, " +
                          $"{GteDepth.ZBatchVram} to VRAM, which has none" +
                          $"{(GteDepth.ZRtRecreated == 0 ? "" : $"; {GteDepth.ZRtRecreated} targets rebuilt, each losing its depth")}");
        GteDepth.ZBatchRt = GteDepth.ZBatchVram = GteDepth.ZRtRecreated = 0;

        if (lo > hi) { Console.WriteLine("    depth map: nothing written — no pixel depth-tested"); return; }

        Console.WriteLine($"    depth map (0 = nearest {lo:F0}, 9 = farthest {hi:F0}, . = untested):");
        float span = Math.Max(1f, hi - lo);
        var line = new char[GteDepth.DepthMapCols];
        for (int r = 0; r < GteDepth.DepthMapRows; r++)
        {
            for (int c = 0; c < GteDepth.DepthMapCols; c++)
            {
                float v = map[r * GteDepth.DepthMapCols + c];
                line[c] = v >= GteDepth.DepthMapEmpty ? '.' : (char)('0' + (int)Math.Min(9, (v - lo) * 10f / span));
            }
            Console.WriteLine("      " + new string(line));
        }
    }

    static string Describe(in GteDepth.BigTri t) =>
        $"#{t.Order,-4} ot {t.Ot,-5} {(t.Tested ? "z" : "PAINTER'S, z")} {t.MinZ,6:F0}..{t.MaxZ,-6:F0} " +
        $"({t.X0},{t.Y0})-({t.X1},{t.Y1}) {t.Area,6}px " +
        $"{(t.Tex ? "tex " : "flat")}{(t.Semi ? " semi" : "     ")}" +
        $"{(t.Clamped ? " CLAMPED" : "        ")} rgb({t.R},{t.G},{t.B})";
}
