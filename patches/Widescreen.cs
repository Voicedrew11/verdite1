using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hle;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// Renders the game wider than its 320-pixel screen.
///
///     KF2_WIDESCREEN=16:9       aspect for the run; "1.777" and "off" also parse
///     KF2_WIDESCREEN_PROBE=1    the margin census, on the console
///     KF2_WIDESCREEN_PROBE=2    the census plus every wide primitive, once per shape
///     KF2_WIDESCREEN_EFFECTS=0  leave the death fade and the damage flash 320 wide
///
/// It is a setting, under Video — see Kf2.Settings.WidescreenPage. The saved
/// aspect is read on RuntimeReadyEvent rather than in <see cref="Configure"/>,
/// since ConfigManager only loads inside HostWindow.Initialize, after Program.cs.
///
/// The runtime already knows how to do this: a display buffer is drawn into a
/// render target that carries a margin of extra columns either side
/// (<c>GpuHle.WideMargin</c>), only the original 320 are written back to VRAM, and
/// the whole widened target is presented at <c>Display.WideAspect</c>. Setting
/// that one number is the whole hookup — 16:9 gives 54 columns a side.
///
/// What that buys depends entirely on the game, and this is the distinction worth
/// keeping straight:
///
///   * It is *not* a stretch. The projection is untouched, so pixels keep their
///     aspect and the HUD is drawn at its authored size, sitting inset from the
///     new edges rather than smeared 33% wide across them.
///   * The extra picture is real only where the game submits geometry it expects
///     the GPU to clip. Anything the game culls itself, against its own idea of
///     the screen, never reaches the margin however wide the target is.
///
/// So this carries its own evidence: <c>KF2_WIDESCREEN_PROBE=1</c> classifies
/// every primitive by whether any vertex falls outside the game's own clip
/// rectangle. Measured in the first area (`fdat02`), it is **a quarter** — 25.4%
/// and 25.1% of about 1,000 primitives a frame in consecutive windows, and 22-25%
/// on the title screen. King's Field culls per object and by depth and leaves the
/// screen edge to the GPU, so a quarter of every frame it draws was being thrown
/// away at x=0 and x=319. That is the picture the margin recovers, and it is why
/// this is worth doing on this game specifically.
///
/// The census is the thing to look at if a scene looks wrong at the sides. The
/// GAME.EXE menus measure 0.0% — they are 2D, authored 320 wide, and no aspect
/// will widen them; what the sides show there is whatever the frame's background
/// clear covered, since PutDrawEnv extends an `isbg` clear across the margin but
/// nothing extends a full-screen rectangle the game drew itself.
///
/// ## The screen-space tints
///
/// That last sentence was a real defect, not a footnote: the death fade and the
/// damage flash blacked out and reddened the middle of the picture only, with the
/// world carrying on brightly in the margins. The game draws every whole-screen
/// effect the same way — <c>func_8003220C</c> leaves a blend mode and a colour in a
/// request block, <c>func_8003214C</c> reads it once a frame and submits
/// <c>func_80031EE8(0, 0, 320, 240)</c>, a flat semi-transparent POLY_FT4 at the
/// front of the ordering table whose tpage carries the mode (subtractive for the
/// fade, additive for a flash). <see cref="Stretch"/> widens that quad, keyed on
/// its shape rather than on the drawer's address so that OPEN.EXE's and END.EXE's
/// own links of it need no addresses of their own. See "The screen-space effects
/// are 320 wide too" in NOTES.md.
///
/// ## Why a patch, and why 16:9 is the default
///
/// It is a patch for the reason the dither switch is one: an aspect ratio is a
/// picture the port should be able to offer without a package having to load, and
/// a player looking for it will look in Video, not in a Mods popup. That is an
/// argument about *where the switch lives*. The default is 16:9: the census said
/// a quarter of the frame was there to recover, and that is now the picture a
/// first run presents. 4:3 remains the off setting, which is <c>WideAspect = 0</c>
/// and the untouched path. See "Widescreen" in NOTES.md.
///
/// ## Anchoring the HUD
///
/// The world gets wider; the HUD does not, because it is drawn in screen space
/// and screen space is still 320 wide. Left alone it sits inset from the new
/// edges, hugging the 4:3 box it was authored in. Anchoring moves each element to
/// the edge it belongs to — which needs a way to tell a HUD primitive from a
/// world one, and the primitive stream carries no such flag.
///
/// Two measurements, both in `fdat02`, say what it is and what it is not:
///
///   * **Where the HUD is.** Whole-frame dumps put it in two fixed clusters —
///     x 5..91 for the HP/MP panel, x 269..310 for the equipment icons, both at
///     y 11..60 — and always in the **last ordering-table entries**, from about
///     65 from the end. That is the front of the OT, which is where a painter's
///     algorithm has to put anything that goes on top.
///   * **What it is not: the palette.** Every HUD primitive in that first dump
///     used a CLUT in VRAM column 0, and no world primitive in that frame did,
///     which looked like a clean test and is not one: walking further into the
///     area finds world geometry using column-0 palettes all the way back through
///     the OT. Anchoring on that rule shifted half the world sideways by exactly
///     the margin — 54-pixel wedges of missing floor and ceiling, which is what
///     the artefact looks like when a rule like this is wrong.
///
/// So the test is structural and positional, not palette-based: a primitive is
/// HUD if it is in the last <see cref="HudTailEntries"/> ordering-table entries
/// *and* its box falls inside one of the two clusters. The OT gate is why this
/// replaces <c>DrawOTag</c> and walks the table itself — the primitive event
/// cannot say which entry a primitive came from. Being wrong now costs one small
/// triangle in a corner rather than the whole frame after it.
///
/// Each HUD primitive then moves to its own side: entirely left of screen centre
/// moves out by the margin, entirely right moves out the other way, and anything
/// straddling the middle is left where it is, which is where centred text and
/// full-width elements want to be.
///
/// ## What it costs at 4:3
///
/// Nothing that runs per primitive, which is what makes the default-off case free
/// rather than merely cheap. The replacement's two-pass walk and the
/// <c>RenderPrimEvent</c> listener are both gated on the margin actually being
/// non-zero: with no margin the replacement is a straight call to the original and
/// no listener is attached at all, so <c>GpuRaster</c> skips the whole dispatch on
/// <c>HasAnyListeners</c>. The hooks are still installed, because a hook cannot be
/// added once the game is running past its overlay loads and the aspect is a
/// setting that can be changed mid-session.
/// </summary>
public static class Widescreen
{
    // libgpu DrawOTag, per overlay -- the same three addresses NoDither,
    // Perspective and Subpixel hook. This is the one owner of the *replacement*;
    // those three are pre/post hooks and compose with it.
    static readonly (string Overlay, uint Addr)[] DrawOTagSites =
    [
        ("open", 0x80016078), ("game", 0x80060818), ("end", 0x80013D80),
    ];

    /// <summary>Where the choices are kept between runs. Named as the mod named
    /// them, so a player who had the mod on keeps the picture they had.</summary>
    public const string AspectKey = "kf2.widescreen.aspect";

    // The HUD anchoring and the tint stretching had a saved key each and a
    // checkbox each, and both are gone. They are not choices: a wide picture whose
    // death fade covers the middle 320 pixels is a defect, not a preference, and
    // the HUD anchoring is the one part of widescreen that moves something the
    // game drew where it meant to draw it. So the tints follow the aspect always
    // and the HUD never moves, with the console keeping both as comparisons.

    /// <summary>The game's own aspect, and the one that means "off".</summary>
    public const float FourThree = 4f / 3f;

    /// <summary>The aspect a first run presents at.</summary>
    public const float SixteenNine = 16f / 9f;

    /// <summary>As wide as an aspect is allowed to get. Past 3:1 the margin is
    /// wider than the screen and the frame is mostly things the game never meant
    /// to draw.</summary>
    public const float Widest = 3f;

    /// <summary>What the settings page offers. <c>KF2_WIDESCREEN</c> takes any
    /// ratio, and one that is none of these keeps its own entry in the combo.</summary>
    public static readonly (string Name, float Ratio)[] Presets =
    [
        ("4:3 (off)", FourThree),
        ("16:10",     16f / 10f),
        ("16:9",      SixteenNine),
        ("21:9",      64f / 27f),
    ];

    /// <summary>The target aspect. 4:3 is off; 16:9 is the default.</summary>
    public static float Aspect { get; private set; } = SixteenNine;

    /// <summary>Move the HP/MP panel and the equipment icons out to the new edges.
    /// **Off, and no longer a setting** (<c>KF2_WIDESCREEN_HUD=1</c> is the
    /// comparison): everything else widescreen does presents geometry the game
    /// submitted and the GPU clipped, and this alone *moves* something the game
    /// placed deliberately. Where it lands has never been looked at by eye, which
    /// is the port's usual reason for a default, and offering it as a tick asked
    /// the player to judge that instead. Costs nothing while
    /// <see cref="Aspect"/> is 4:3.</summary>
    public static bool AnchorHud { get; private set; }

    /// <summary>Widen the game's full-screen tints — the death fade, the damage
    /// flash — across the margin. **On whenever an aspect is chosen, and no longer
    /// a setting** (<c>KF2_WIDESCREEN_EFFECTS=0</c> is the comparison): a death
    /// fade that blacks out the middle of the screen and leaves the sides showing
    /// the dungeon is a defect rather than a preference. Costs nothing while
    /// <see cref="Aspect"/> is 4:3.</summary>
    public static bool StretchEffects { get; private set; } = true;

    /// <summary>Whether the aspect is actually widening anything.</summary>
    public static bool On => Display.WideAspect > 0f;

    /// <summary>Extra columns a side, by the runtime's own sizing rule — so the
    /// number the settings page shows and the number the render target is built
    /// with are the same one. 320 is this game's display width.</summary>
    public static int Margin => Display.WideMargin(320);

    /// <summary>KF2_WIDESCREEN: an explicit aspect for the run, which wins over
    /// the saved setting.</summary>
    static float? _forced;

    /// <summary>KF2_WIDESCREEN_PROBE: count primitives reaching the margin and
    /// report to the console.</summary>
    static bool _measure;

    /// <summary>KF2_WIDESCREEN_EFFECTS: the comparison, since there is no longer a
    /// checkbox.</summary>
    static bool? _forcedEffects;

    /// <summary>KF2_WIDESCREEN_HUD: the comparison for the anchoring, which is
    /// otherwise unreachable.</summary>
    static bool? _forcedHud;

    /// <summary>KF2_WIDESCREEN_PROBE=2: also list the wide primitives themselves.</summary>
    static bool _listWide;

    // The HUD measured 65 entries from the end of the ordering table; this is that
    // with room to spare, and still a thousandth of the ~9,400-entry table, so
    // world geometry has to be practically touching the camera to reach it.
    const int HudTailEntries = 128;

    // Screen-space boxes the HUD was measured in, padded. Clip-relative, so they
    // do not care where in VRAM the frame buffer sits.
    const int HudBottom = 70;
    const int HudLeftEdge = 110;      // left cluster: x 5..91
    const int HudRightEdge = 250;     // right cluster: x 269..310

    // How far off the screen edge a vertex may sit and still count as being on it.
    // The game's own tints land exactly on 0 and 320, so this is only insurance.
    const int EdgeSlack = 2;

    // Full-width tints stretched in the current report window.
    static long _stretched;

    // How far the current primitive is from the end of the OT walk; -1 outside it.
    static int _fromEnd = -1;

    // Per report window, split by whether the primitive crossed into the margin.
    static long _inside, _margin;
    static double _windowStart;

    // The census's second half: every primitive wide enough to be a screen-space
    // overlay, once per distinct shape, so a fade or a flash names itself.
    static readonly HashSet<long> _seenWide = [];

    static double Now => Environment.TickCount64 / 1000.0;

    // HookManager attributes hooks to a mod so they can be removed again. This is
    // in-project rather than a loaded package, so it declares its own identity.
    static readonly ModInfo _self = new()
    {
        Id = "kf2.widescreen",
        Name = "Widescreen",
        Version = "1.0",
        Description = "Renders a margin either side of the game's 320-pixel screen.",
    };

    public static void Configure(string? aspect, string? probe, string? effects = null,
                                 string? hud = null)
    {
        if (Parse(aspect) is { } ratio) _forced = ratio;

        if (!string.IsNullOrWhiteSpace(probe) && !probe.Equals("0", StringComparison.Ordinal))
        {
            _measure = true;
            // =2 additionally lists the wide primitives themselves, which is how
            // the full-screen tint was identified. It is a page of output per
            // scene, so it is not what a plain =1 asks for.
            _listWide = probe.Equals("2", StringComparison.Ordinal);
        }

        if (!string.IsNullOrWhiteSpace(effects))
            _forcedEffects = !effects.Equals("0", StringComparison.Ordinal);

        if (!string.IsNullOrWhiteSpace(hud))
            _forcedHud = !hud.Equals("0", StringComparison.Ordinal);
    }

    /// <summary>
    /// Apply the aspect and attach the hook. The saved settings are read on
    /// <c>RuntimeReadyEvent</c> — ConfigManager only loads inside
    /// HostWindow.Initialize, which is after Program.cs, so reading them here
    /// would read an empty config and then write it back over the real one. An
    /// aspect from the environment is applied immediately, so a run started for
    /// the picture is wide from the first frame of the title screen.
    /// </summary>
    public static void Install()
    {
        _windowStart = Now;
        if (_forced is { } forced) { Aspect = forced; Apply(); }

        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            Aspect = _forced ?? RecompOne.Runtime.Runtime.View.GetFloat(AspectKey, SixteenNine);

            // Neither of these reads the saved config any more, deliberately: a
            // player who ticked one off before it stopped being a tick would
            // otherwise be stuck with that forever, with nothing in the settings
            // window to put it back.
            AnchorHud = _forcedHud ?? false;
            StretchEffects = _forcedEffects ?? true;
            Apply();
            Console.WriteLine(On
                ? $"[KF2] widescreen: {Aspect:0.###}:1, margin {Margin} px a side, " +
                  $"HUD {(AnchorHud ? "anchored" : "left in its 4:3 box")}, " +
                  $"screen tints {(StretchEffects ? "stretched across it" : "left 320 wide")}"
                : "[KF2] widescreen: off (4:3)");
        });

        // Attached whether or not an aspect is set, for the same reason the dither
        // hooks are: the aspect is a setting that can be changed mid-session, and
        // hooks cannot be added once the game is running past the overlay loads.
        bool attached = false;
        Event.AddListener<OverlayLoadedEvent>(e =>
        {
            // The margin-content latch lasts for an *executable's* session, not an
            // area's. OPEN.EXE and END.EXE draw pictures that never reach the
            // margin, so a latch carried over from GAME.EXE would present invented
            // sides -- that is what the clear is for. The nine fdat area modules
            // are GAME.EXE still running, and clearing on those dropped the picture
            // to the 320-wide 4:3 fallback for the length of every area load: the
            // black bars came back each time the player crossed between areas.
            if (e.Name is "open" or "game" or "end")
                (GpuHle.Backend as GlCore)?.ClearMarginLatches();

            if (attached) return;
            attached = true;
            Attach();
        });
    }

    /// <summary>Change the aspect at run time. Safe at any moment — the render
    /// target is rebuilt when its margin no longer matches.</summary>
    public static void SetAspect(float aspect)
    {
        Aspect = Math.Clamp(aspect, FourThree, Widest);
        Apply();
    }

    static void Apply()
    {
        // 4:3 means off, and off is 0 rather than 1.333 -- WideMargin returns no
        // margin for a non-positive aspect, and a zero-margin target presents at
        // SourceAspect, which is the untouched path.
        Display.WideAspect = Aspect > FourThree + 0.001f ? Aspect : 0f;
        Listen();

        // The margin only shows what the game submitted, and the game submits
        // against a top-down trapezoid sized for a 4:3 screen. Opening that to the
        // new aspect is what puts anything in the margin at the sides rather than
        // only in the corners the game happened to overdraw. See Kf2.CullCone.
        CullCone.Apply();

        // And the game's own view-space clip volume, which cuts the near floor and
        // ceiling with a straight edge 320 pixels from centre -- outside the picture
        // until about 8:3, inside it after. See Kf2.ViewClip.
        ViewClip.Apply();
    }

    // One listener serves both jobs, so it is attached exactly when one of them
    // needs it -- the raster skips the whole dispatch when nothing is listening,
    // which is what makes 4:3 free rather than cheap.
    static void Listen()
    {
        Event.RemoveListener<RenderPrimEvent>(OnPrim);
        if (_measure || (On && (AnchorHud || StretchEffects))) Event.AddListener<RenderPrimEvent>(OnPrim);
    }

    static void Attach()
    {
        SymbolRegistry.Build();
        var replace = typeof(Widescreen).GetMethod(nameof(DrawOTag), BindingFlags.Public | BindingFlags.Static)!;

        int n = 0;
        foreach (var (overlay, addr) in DrawOTagSites)
        {
            var target = SymbolRegistry.Resolve(overlay, null, addr);
            if (target == null)
            {
                Console.Error.WriteLine($"[KF2] widescreen: no function at {overlay}/0x{addr:X8}");
                continue;
            }
            if (HookManager.AddReplace(_self, target, replace)) n++;
        }

        HookManager.Commit();
        Console.WriteLine($"[KF2] widescreen: {n} hook(s)");

        if (n == 0)
            Console.Error.WriteLine("[KF2] widescreen: nothing hooked — the picture still widens, " +
                                    "but the HUD will stay in its 4:3 box. See \"Widescreen\" in NOTES.md.");
    }

    /// <summary>
    /// libgpu's DrawOTag with the entries numbered. Two passes: the first only
    /// follows the `next` pointers to learn the length, which is what makes "how far
    /// from the end" answerable while the primitives are being emitted; the second
    /// is the runtime's own walk (<c>LibGpu.WalkOTag</c>).
    ///
    /// <c>HookManager.Invoke</c> runs pre-hooks, then the replacement, then
    /// post-hooks, so the dither patch's pre/post pair and the frame pacing's,
    /// perspective's and sub-pixel's post-hooks on this same function all still run.
    /// </summary>
    public static void DrawOTag(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        var gpu = RecompOne.Runtime.Runtime.Gpu;
        // The listing wants the entry number too, so it walks for that as well as
        // for the anchoring -- at 4:3 included, since "where is this drawn from" is
        // a question asked before the aspect is changed. The plain census does not
        // need it, and the tint stretch does not either: a screen-space tint is
        // recognised by its shape, not by where in the table it sits.
        if (!((AnchorHud && On) || _listWide) || gpu == null) { orig(c, m); return; }

        uint mask = RecompOne.Runtime.Runtime.RamWordMask;
        uint addr = c.A0 & mask;
        int entries = 0;
        for (int guard = 0; guard < 0x100000; guard++)
        {
            uint header = m.ReadU32(addr);
            entries++;
            uint next = header & 0xFFFFFFu;
            if (next == 0xFFFFFFu || (next & 0x800000u) != 0) break;
            addr = next & mask;
        }

        // The runtime's own walk, told each primitive's entry before it is sent:
        // 0079 can send a blended packet after entries that follow it, and it is
        // anchored by where it was linked, not by where the walk had got to.
        _entries = entries;
        RecompOne.Runtime.Sdk.LibGpu.WalkOTag(m, c.A0, _onEntry);
        _fromEnd = -1;
    }

    static int _entries;
    static readonly Action<int> _onEntry = entry => _fromEnd = _entries - 1 - entry;

    // The draw area is the game's clip rect in VRAM coordinates and the primitive's
    // are already offset into the same space, so "reaches the margin" is just a
    // vertex outside the clip -- no assumption about where the display buffer sits.
    static void OnPrim(RenderPrimEvent e)
    {
        // Measured first, before anything below moves a vertex: the census is about
        // what the game submitted and the listing is about recognising it, and
        // neither is a question about where this patch then put it.
        if (_measure)
        {
            if (_listWide) ProbeWide(e);
            Census(e);
        }

        if (StretchEffects && On) Stretch(e);

        if (AnchorHud && On && _fromEnd >= 0 && _fromEnd < HudTailEntries) Anchor(e);
    }

    static void Census(RenderPrimEvent e)
    {
        bool outside = false;
        for (int i = 0; i < e.Count; i++)
            if (e.X[i] < e.DrawLeft || e.X[i] > e.DrawRight) { outside = true; break; }

        if (outside) _margin++; else _inside++;

        double window = Now - _windowStart;
        if (window < 2.0) return;

        long total = _inside + _margin;

        // A scene with no margin primitives is one the game clipped itself; widening
        // the target cannot recover it.
        Console.WriteLine(total == 0
            ? "[KF2] widescreen: no primitives in the last window"
            : $"[KF2] widescreen: {_margin * 100.0 / total:F1}% of {total} prims reach the margin " +
              $"({total / window:F0}/s)" +
              (_stretched > 0 ? $"; {_stretched} full-screen tint(s) stretched" : ""));

        _inside = _margin = _stretched = 0;
        _windowStart = Now;
    }

    // A screen-space overlay -- a fade, a flash, a letterbox bar -- is a primitive
    // as wide as the screen, so this lists every primitive that covers most of the
    // clip rectangle, once per distinct shape. What comes out is the rule for
    // widening them.
    static void ProbeWide(RenderPrimEvent e)
    {
        int lo = int.MaxValue, hi = int.MinValue, top = int.MaxValue, bottom = int.MinValue;
        for (int i = 0; i < e.Count; i++)
        {
            lo = Math.Min(lo, e.X[i]); hi = Math.Max(hi, e.X[i]);
            top = Math.Min(top, e.Y[i]); bottom = Math.Max(bottom, e.Y[i]);
        }

        int width = e.DrawRight - e.DrawLeft + 1;
        // The part of it that is actually on screen: a world polygon a thousand
        // pixels wide off to one side is not an overlay, and there are thousands
        // of those a second.
        if (Math.Min(hi, e.DrawRight) - Math.Max(lo, e.DrawLeft) < width * 9 / 10) return;

        lo -= e.DrawLeft; hi -= e.DrawLeft;
        top -= e.DrawTop; bottom -= e.DrawTop;

        long key = ((long)(lo & 0x3FF) << 40) | ((long)(hi & 0x3FF) << 30) |
                   ((long)(top & 0x1FF) << 21) | ((long)(bottom & 0x1FF) << 12) |
                   ((long)e.Count << 8) | (e.Textured ? 1 : 0) | (e.SemiTransparent ? 2 : 0) |
                   (e.Gouraud ? 4 : 0) | (e.Raw ? 8 : 0);
        if (!_seenWide.Add(key)) return;

        Console.WriteLine($"[KF2] widescreen: wide prim x {lo}..{hi} y {top}..{bottom} " +
                          $"verts={e.Count} tex={(e.Textured ? 1 : 0)} semi={(e.SemiTransparent ? 1 : 0)} " +
                          $"gouraud={(e.Gouraud ? 1 : 0)} raw={(e.Raw ? 1 : 0)} clut=0x{e.Clut:X4} " +
                          $"ot={_fromEnd} clip={width}x{e.DrawBottom - e.DrawTop + 1}");
    }

    // Stretch a full-screen tint across the margin.
    //
    // The game paints the death fade, the damage flash and every other whole-screen
    // effect the same way: one flat, textured, semi-transparent quad from (0,0) to
    // (320,240), submitted at the front of the ordering table by func_80031EE8 from
    // a colour and a blend mode left in the request block at 0x80192D45. The
    // blend mode is the effect -- subtractive for the fade to black, additive for a
    // flash -- and the texture is a fifteen-pixel patch of flat colour already
    // stretched over the whole screen, so widening the quad costs nothing and shows
    // nothing.
    //
    // The test is that shape rather than that address: OPEN.EXE and END.EXE hold
    // their own links of the same drawer, and a primitive is checked here anyway.
    // Semi-transparent and flat is what separates the tint from the world -- one
    // colour laid over the whole frame is exactly what an effect is, while world
    // geometry is Gouraud-shaded to the last polygon (measured: every wide
    // primitive in an area, semi-transparent or not, is Gouraud). A 2D picture
    // authored 320 wide -- a title, a menu -- is opaque and is deliberately left
    // where it is: stretching a picture distorts it, and pillar-boxing one does not.
    static void Stretch(RenderPrimEvent e)
    {
        if (!e.SemiTransparent || e.Gouraud) return;

        int width = e.DrawRight - e.DrawLeft + 1;
        int margin = Display.WideMargin(width);
        if (margin <= 0) return;

        // Right is exclusive here: the game's own tint runs 0..320 over a 320-pixel
        // screen, which is the last column plus one, and a rectangle's second point
        // is x + w for the same reason.
        int left = e.DrawLeft, right = e.DrawRight + 1;

        int lo = int.MaxValue, hi = int.MinValue;
        for (int i = 0; i < e.Count; i++) { lo = Math.Min(lo, e.X[i]); hi = Math.Max(hi, e.X[i]); }
        if (lo > left + EdgeSlack || hi < right - EdgeSlack) return;

        // Snapped to the new edges rather than shifted by the margin, so a tint that
        // was a pixel short of the screen still covers all of it.
        for (int i = 0; i < e.Count; i++)
        {
            if (e.X[i] <= left + EdgeSlack) e.X[i] = left - margin;
            else if (e.X[i] >= right - EdgeSlack) e.X[i] = right + margin;
        }

        // The latch in the backend must not mistake this manufactured width for
        // margin content the game drew -- a stretched splash fade would otherwise
        // latch its buffer and the present would flap between widths again.
        RecompOne.Runtime.Hle.GpuHle.PortWidenedPrim = true;
        _stretched++;
    }

    // Move a HUD element out to the side of the screen it sits on. The clip
    // rectangle is the screen, so everything here is relative to it: the boxes, the
    // dividing line, and the width the margin was sized from.
    static void Anchor(RenderPrimEvent e)
    {
        int width = e.DrawRight - e.DrawLeft + 1;
        int margin = Display.WideMargin(width);
        if (margin <= 0) return;

        int lo = int.MaxValue, hi = int.MinValue, top = int.MaxValue, bottom = int.MinValue;
        for (int i = 0; i < e.Count; i++)
        {
            lo = Math.Min(lo, e.X[i]); hi = Math.Max(hi, e.X[i]);
            top = Math.Min(top, e.Y[i]); bottom = Math.Max(bottom, e.Y[i]);
        }

        lo -= e.DrawLeft; hi -= e.DrawLeft;
        top -= e.DrawTop; bottom -= e.DrawTop;
        if (top < 0 || bottom > HudBottom) return;

        // A box that crosses the middle -- centred text, a full-width bar -- has no
        // side to move to.
        int shift = hi <= HudLeftEdge ? -margin : lo >= HudRightEdge ? margin : 0;
        if (shift == 0) return;

        // Same rule as Stretch: width this patch manufactured is not margin
        // content the game drew, and the backend latch must not count it.
        RecompOne.Runtime.Hle.GpuHle.PortWidenedPrim = true;
        for (int i = 0; i < e.Count; i++) e.X[i] += shift;
    }

    /// <summary>"16:9" and "1.777" both, since the environment variable is typed by
    /// hand; "off" is 4:3. Null for anything else, which leaves the saved setting
    /// in charge rather than silently picking a ratio.</summary>
    static float? Parse(string? value)
    {
        if (string.IsNullOrWhiteSpace(value)) return null;
        value = value.Trim();

        if (value.Equals("off", StringComparison.OrdinalIgnoreCase)) return FourThree;

        int colon = value.IndexOf(':');
        if (colon > 0)
        {
            if (float.TryParse(value[..colon], out float w) &&
                float.TryParse(value[(colon + 1)..], out float h) && h > 0f)
                return w / h;
            return null;
        }

        return float.TryParse(value, out float ratio) && ratio > 0f ? ratio : null;
    }
}
