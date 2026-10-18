using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hle;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// Renders the game wider than its 320-pixel screen, Verdite3's widescreen on this
/// disc. The runtime already does the work: a display buffer is drawn into a render
/// target carrying a margin of extra columns either side (<c>GpuHle.WideMargin</c>),
/// only the original columns go back to VRAM, and the widened target is presented
/// at <c>Display.WideAspect</c>. This file is the switch, the latch clear, the
/// full-screen tint stretch, the menu's background tiled across the margin and the
/// HUD at the screen edges.
///
///     KF1_WIDESCREEN=16:9       aspect for the run; "1.777" and "off" also parse
///     KF1_WIDESCREEN_PROBE=1    the margin census, on the console
///     KF1_WIDESCREEN_PROBE=2    the census plus every wide primitive, once per shape
///     KF1_WIDESCREEN_EFFECTS=0  leave the death fade and the damage flash 320 wide
///     KF1_WIDESCREEN_HUD=1      move the HUD out to the new edges (0 leaves it), over the kept choice
///     KF1_WIDESCREEN_MENU=0     leave the menu's background 320 wide
///
/// The runtime's implement is not a stretch: the projection is untouched, so pixels
/// keep their aspect and the extra picture is only there where the game submits
/// geometry it expects the GPU to clip. <see cref="Census"/> counts that.
/// <see cref="Stretch"/> widens the game's full-screen tints -- the death fade and
/// the damage flash -- which are flat, semi-transparent quads spanning the clip
/// rectangle exactly; keyed on that shape rather than a drawer address. On
/// whenever an aspect is chosen; <c>KF1_WIDESCREEN_EFFECTS=0</c> is the comparison.
///
/// <see cref="AnchorHud"/> is Verdite3's HUD at the screen edges, by the same means:
/// the records each HUD drawer reads have their screen X moved out by the margin
/// for the call and put back after it, so nothing downstream is told and a menu's
/// primitives are never touched. Two drawers: the sprites (<c>func_8001F9D4</c>, X at
/// <c>+6</c>: the gauges, their numbers and marks, the compass's dial) and the compass
/// model (<c>func_8001F8B0</c>, its translation's X at <c>+6</c>, at a Z equal to the
/// projection's H of 200, so its X is screen pixels). The selected item's display is
/// centred and left alone. Verdite2's DrawOTag replacement is not ported; it fed
/// only the older, guessed anchoring.
///
/// The menu's background is four FT4s of one tile, 160 x 120 each, prebuilt per
/// buffer and linked on slot 3000 by several page routines. Before the menu's
/// presenter <c>func_8002AC34</c>, copies of whichever of them are linked go in 320
/// px apart on the same slot until the margin is covered.
///
/// It is a setting, under Testing ▸ Picture. The saved aspect is read on
/// <see cref="RuntimeReadyEvent"/> rather than in <see cref="Configure"/>, since
/// ConfigManager only loads inside HostWindow.Initialize, after Program.cs.
///
/// See "The margin, the latches and the tints" in docs/WIDESCREEN.md.
/// </summary>
public static class Widescreen
{
    /// <summary>Where the choice is kept between runs. Named as Verdite2 named it,
    /// to follow the port's convention.</summary>
    public const string AspectKey = "kf1.widescreen.aspect";

    /// <summary>The game's own aspect, and the one that means "off".</summary>
    public const float FourThree = 4f / 3f;

    /// <summary>As wide as an aspect is allowed to get. Past 3:1 the margin is wider
    /// than the screen and the frame is mostly things the game never meant to draw.</summary>
    public const float Widest = 3f;

    /// <summary>What the settings combo offers. <c>KF1_WIDESCREEN</c> takes any
    /// ratio, and one that is none of these keeps its own entry in the combo.</summary>
    public static readonly (string Name, float Ratio)[] Presets =
    [
        ("4:3 (off)", FourThree),
        ("16:9",      16f / 9f),
        ("16:10",     16f / 10f),
        ("21:9",      64f / 27f),
    ];

    /// <summary>The target aspect; 4:3 is off.</summary>
    public static float Aspect { get; private set; } = DefaultAspect;

    public const float DefaultAspect = 16f / 9f;

    /// <summary>Widen the game's full-screen tints -- the death fade, the damage
    /// flash -- across the margin. On whenever an aspect is chosen;
    /// <c>KF1_WIDESCREEN_EFFECTS=0</c> is the comparison.</summary>
    public static bool StretchEffects { get; private set; } = true;

    /// <summary>Whether the aspect is actually widening anything.</summary>
    public static bool On => Display.WideAspect > 0f;

    /// <summary>Whether the HUD is moved out to the new edges. On by default;
    /// kept as <c>kf1.widescreen.hud</c> by the Testing tab.</summary>
    public static bool AnchorHud { get; private set; } = true;

    /// <summary>Takes effect on the next frame the HUD is drawn.</summary>
    public static void SetAnchorHud(bool on) => AnchorHud = on;

    /// <summary>Whether the menu's background is tiled across the margin.
    /// KF1_WIDESCREEN_MENU=0 is the comparison.</summary>
    public static bool TileMenu { get; private set; } = true;

    /// <summary>Extra columns a side, by the runtime's own sizing rule -- so the
    /// number the settings combo shows and the number the render target is built
    /// with are the same one. 320 is this game's display width.</summary>
    public static int Margin => Display.WideMargin(320);

    /// <summary>KF1_WIDESCREEN: an explicit aspect for the run, which wins over
    /// the saved setting.</summary>
    static float? _forced;

    /// <summary>KF1_WIDESCREEN_PROBE: count primitives reaching the margin and
    /// report to the console.</summary>
    static bool _measure;

    /// <summary>KF1_WIDESCREEN_EFFECTS: the comparison, since there is no check box.</summary>
    static bool? _forcedEffects;

    /// <summary>KF1_WIDESCREEN_PROBE=2: also list the wide primitives themselves.</summary>
    static bool _listWide;

    // How far off the screen edge a vertex may sit and still count as being on it.
    // The game's own tints land exactly on 0 and 320, so this is only insurance.
    const int EdgeSlack = 2;

    // Full-width tints stretched in the current report window.
    static long _stretched;

    // Per report window, split by whether the primitive crossed into the margin.
    static long _inside, _margin;
    static double _windowStart;

    // The census's second half: every primitive wide enough to be a screen-space
    // overlay, once per distinct shape, so a fade or a flash names itself.
    static readonly HashSet<long> _seenWide = [];

    static double Now => Environment.TickCount64 / 1000.0;

    public static void Configure(string? aspect, string? probe, string? effects = null, string? hud = null,
                                 string? menu = null)
    {
        if (!string.IsNullOrWhiteSpace(menu))
            TileMenu = !menu.Equals("0", StringComparison.Ordinal);

        if (!string.IsNullOrWhiteSpace(hud))
            AnchorHud = !hud.Equals("0", StringComparison.Ordinal);

        if (Parse(aspect) is { } ratio) _forced = ratio;

        if (!string.IsNullOrWhiteSpace(probe) && !probe.Equals("0", StringComparison.Ordinal))
        {
            _measure = true;
            // =2 additionally lists the wide primitives themselves, which is how the
            // full-screen tint was identified. It is a page of output per scene, so
            // it is not what a plain =1 asks for.
            _listWide = probe.Equals("2", StringComparison.Ordinal);
        }

        if (!string.IsNullOrWhiteSpace(effects))
            _forcedEffects = !effects.Equals("0", StringComparison.Ordinal);
    }

    /// <summary>Apply the aspect and attach the overlay listener. The saved setting
    /// is read on <c>RuntimeReadyEvent</c> -- ConfigManager only loads inside
    /// HostWindow.Initialize, which is after Program.cs, so reading it here would
    /// read an empty config and then write it back over the real one. An aspect from
    /// the environment is applied immediately, so a run started for the picture is
    /// wide from the first frame of the title screen.</summary>
    public static void Install()
    {
        _windowStart = Now;
        if (_forced is { } forced) { Aspect = forced; Apply(); }

        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            Aspect = _forced ?? RecompOne.Runtime.Runtime.View.GetFloat(AspectKey, DefaultAspect);

            // The comparison, not a saved key: a player who ticked the old option off
            // would otherwise be stuck with it with nothing left to put it back.
            StretchEffects = _forcedEffects ?? true;
            Apply();
            Console.WriteLine(On
                ? $"[KF1] widescreen: {Aspect:0.###}:1, margin {Margin} px a side, " +
                  $"screen tints {(StretchEffects ? "stretched across it" : "left 320 wide")}"
                : "[KF1] widescreen: off (4:3)");
        });

        // Attached at every aspect: the aspect and the anchoring change mid-session.
        HookAttach.OnOverlayLoad("widescreen HUD", AttachHud);

        // Attached whether or not an aspect is set: the aspect is a setting that can
        // be changed mid-session, and the latch clear must run under every overlay.
        Event.AddListener<OverlayLoadedEvent>(e =>
        {
            // The margin-content latch lasts for an *executable's* session. OPEN.EXE
            // draws pictures that never reach the margin, so a latch carried over
            // from GAME.EXE would present invented sides -- that is what the clear is for.
            if (e.Name is "open" or "game")
                (GpuHle.Backend as GlCore)?.ClearMarginLatches();
        });
    }

    /// <summary>Change the aspect at run time. Safe at any moment -- the render
    /// target is rebuilt when its margin no longer matches.</summary>
    public static void SetAspect(float aspect)
    {
        Aspect = Math.Clamp(aspect, FourThree, Widest);
        Apply();
    }

    /// <summary>The <c>aspect</c> shell verb: the state, or a change through
    /// <see cref="SetAspect"/>, which is what the settings window calls.</summary>
    public static string Shell(string arg)
    {
        if (arg.Length > 0)
        {
            if (Parse(arg) is not { } ratio) return "{\"ok\":false,\"error\":\"aspect [4:3|16:9|16:10|21:9|<ratio>]\"}";
            SetAspect(ratio);
        }
        return $"{{\"ok\":true,\"cmd\":\"aspect\",\"aspect\":{Aspect.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture)},\"margin\":{Margin}}}";
    }

    static void Apply()
    {
        // 4:3 means off, and off is 0 rather than 1.333 -- WideMargin returns no
        // margin for a non-positive aspect, and a zero-margin target presents at
        // SourceAspect, which is the untouched path.
        Display.WideAspect = Aspect > FourThree + 0.001f ? Aspect : 0f;
        Listen();
    }

    // One listener serves both jobs, so it is attached exactly when one of them
    // needs it -- the raster skips the whole dispatch when nothing is listening,
    // which is what makes 4:3 free rather than cheap.
    static void Listen()
    {
        Event.RemoveListener<RenderPrimEvent>(OnPrim);
        if (_measure || (On && StretchEffects)) Event.AddListener<RenderPrimEvent>(OnPrim);
    }

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
            ? "[KF1] widescreen: no primitives in the last window"
            : $"[KF1] widescreen: {_margin * 100.0 / total:F1}% of {total} prims reach the margin " +
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
        // pixels wide off to one side is not an overlay, and there are thousands of
        // those a second.
        if (Math.Min(hi, e.DrawRight) - Math.Max(lo, e.DrawLeft) < width * 9 / 10) return;

        lo -= e.DrawLeft; hi -= e.DrawLeft;
        top -= e.DrawTop; bottom -= e.DrawTop;

        long key = ((long)(uint)(lo & 0x3FF) << 40) | ((long)(uint)(hi & 0x3FF) << 30) |
                   ((long)(uint)(top & 0x1FF) << 21) | ((long)(uint)(bottom & 0x1FF) << 12) |
                   ((long)(uint)e.Count << 8) | (e.Textured ? 1L : 0L) | (e.SemiTransparent ? 2L : 0L) |
                   (e.Gouraud ? 4L : 0L) | (e.Raw ? 8L : 0L);
        if (!_seenWide.Add(key)) return;

        Console.WriteLine($"[KF1] widescreen: wide prim x {lo}..{hi} y {top}..{bottom} " +
                          $"verts={e.Count} tex={(e.Textured ? 1 : 0)} semi={(e.SemiTransparent ? 1 : 0)} " +
                          $"gouraud={(e.Gouraud ? 1 : 0)} raw={(e.Raw ? 1 : 0)} clut=0x{e.Clut:X4} " +
                          $"clip={width}x{e.DrawBottom - e.DrawTop + 1}");
    }

    // Stretch a full-screen tint across the margin.
    //
    // The game paints the death fade, the damage flash and every other whole-screen
    // effect the same way: one flat, textured, semi-transparent quad from (0,0) to
    // (320,240), submitted at the front of the ordering table from a colour and a
    // blend mode left in a request block. Semi-transparent and flat is what
    // separates the tint from the world -- one colour laid over the whole frame is
    // exactly what an effect is, while world geometry is Gouraud-shaded to the last
    // polygon. A 2D picture authored 320 wide -- a title, a menu -- is opaque and is
    // deliberately left where it is: stretching a picture distorts it.
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
        GpuHle.PortWidenedPrim = true;
        _stretched++;
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

    // ---- the HUD, moved by its records -----------------------------------------

    static readonly ModInfo _self = new() { Id = "kf1.widescreen", Name = "Widescreen HUD", Version = "1.0" };

    /// <summary>One table of HUD records and the routine that draws it: records of
    /// <paramref name="Stride"/> bytes from <paramref name="Table"/> until a first byte
    /// of 0xFF, a first byte of 0 hidden, the screen X a halfword at
    /// <paramref name="X"/>.</summary>
    sealed record HudTable(string Name, uint Drawer, uint Table, uint Stride, uint X, int Max)
    {
        public readonly short[] Saved = new short[Max];
        public int Moved;
        public readonly HashSet<string> Seen = [];
    }

    static readonly HudTable[] HudTables =
    [
        new("sprites", 0x8001F9D4, 0x80055C5C, 0x0E, 0x06, 16),
        new("compass", 0x8001F8B0, 0x80055D74, 0x1C, 0x06, 4),
    ];

    /// <summary>The middle of the 320-pixel screen: a record whose X is left of it
    /// belongs to the left edge. The X, not the sprite's middle: the gauges at 129
    /// are as wide as <c>u16[0x800A0798] / 100</c> each frame, so their middle can
    /// cross 160.</summary>
    const int ScreenCentre = 160;

    static bool _hudQueued;

    static bool AttachHud()
    {
        SymbolRegistry.Build();
        var targets = HudTables.Select(t => SymbolRegistry.Resolve("game", null, t.Drawer)).ToArray();
        var window = SymbolRegistry.Resolve("game", null, MenuPresenter);
        if (targets.Any(t => t == null) || window == null) return false;
        const BindingFlags flags = BindingFlags.Public | BindingFlags.Static;
        if (!_hudQueued)
        {
            for (int i = 0; i < HudTables.Length; i++)
            {
                HookManager.AddPre(_self, targets[i]!, typeof(Widescreen).GetMethod(i == 0 ? nameof(BeforeSprites) : nameof(BeforeCompass), flags)!);
                HookManager.AddPost(_self, targets[i]!, typeof(Widescreen).GetMethod(i == 0 ? nameof(AfterSprites) : nameof(AfterCompass), flags)!);
            }
            HookManager.AddPre(_self, window, typeof(Widescreen).GetMethod(nameof(BeforeMenuPresent), flags)!);
            _hudQueued = true;
        }
        HookManager.Commit();
        return targets.All(HookAttach.Installed) && HookAttach.Installed(window);
    }

    public static void BeforeSprites(CpuContext c, IMemory m) => MoveOut(HudTables[0], m);
    public static void AfterSprites(CpuContext c, IMemory m) => PutBack(HudTables[0], m);
    public static void BeforeCompass(CpuContext c, IMemory m) => MoveOut(HudTables[1], m);
    public static void AfterCompass(CpuContext c, IMemory m) => PutBack(HudTables[1], m);

    /// <summary>Move every record out to its edge for the drawer: left of the screen's
    /// middle by the margin to the left, right of it to the right. Hidden records
    /// are moved too, so the put-back needs no record of which were drawn.</summary>
    static void MoveOut(HudTable t, IMemory m)
    {
        t.Moved = 0;
        if (_measure) ProbeHud(t, m);
        if (!(AnchorHud && On)) return;
        int margin = Margin;
        if (margin <= 0) return;

        for (int i = 0; i < t.Max; i++)
        {
            uint rec = t.Table + (uint)i * t.Stride;
            if (m.ReadU8(rec) == 0xFF) break;
            short x = (short)m.ReadU16(rec + t.X);
            t.Saved[i] = x;
            m.WriteU16(rec + t.X, (ushort)(x + (x < ScreenCentre ? -margin : margin)));
            t.Moved = i + 1;
        }
    }

    /// <summary>Put every record's X back the moment the drawer has drawn it.</summary>
    static void PutBack(HudTable t, IMemory m)
    {
        for (int i = 0; i < t.Moved; i++)
            m.WriteU16(t.Table + (uint)i * t.Stride + t.X, (ushort)t.Saved[i]);
        t.Moved = 0;
    }

    /// <summary>KF1_WIDESCREEN_PROBE: each distinct set of drawn records' positions
    /// (index@x,y), once per table, up to 24 sets.</summary>
    static void ProbeHud(HudTable t, IMemory m)
    {
        var line = new System.Text.StringBuilder();
        for (int i = 0; i < t.Max; i++)
        {
            uint rec = t.Table + (uint)i * t.Stride;
            byte on = m.ReadU8(rec);
            if (on == 0xFF) break;
            if (on == 0) continue;
            line.Append($" {i}@{(short)m.ReadU16(rec + t.X)},{(short)m.ReadU16(rec + t.X + 2)}");
        }
        string s = line.ToString();
        if (t.Seen.Count < 24 && t.Seen.Add(s))
            Console.WriteLine($"[KF1] widescreen: HUD {t.Name} drawn at{(s.Length == 0 ? " (none)" : s)}" +
                              $"{(AnchorHud && On ? $", moved {Margin} px out" : "")}");
    }

    // ---- the menu's background, tiled into the margin ----------------------------

    const uint MenuPresenter = 0x8002AC34;
    const uint PacketSize = 0x28;
    const uint BackgroundEnd = MenuDraw.WindowPackets + 2 * 0xA0;
    static readonly uint[] _linked = new uint[8];
    static uint _lastTile;
    static readonly HashSet<long> _menuSeen = [];

    /// <summary>Before the menu presents: find the background's packets on slot 3000
    /// (four a buffer from <c>0x800580E8</c>, linked by the window writer
    /// <c>func_8002A510</c> or inline by <c>func_80022D7C</c>, <c>func_80025DA0</c> and
    /// <c>func_800292F8</c>), then copy each 320 px left and right, and further while
    /// the copy still reaches the margin, from the menu's own packet cursor onto the
    /// same slot. A period of 320 repeats whatever the four quads hold.</summary>
    public static void BeforeMenuPresent(CpuContext c, IMemory m)
    {
        if (!(TileMenu && On)) return;
        uint entry = m.ReadU32(MenuDraw.OrderingTable) + (uint)MenuDraw.WindowPacketSlot * 4;
        uint next = entry - 4;

        // Slot 3000's chain runs to the entry below it (ClearOTagR). A head that is
        // our own last tile means this table was already tiled.
        uint p = m.ReadU32(entry) & 0x00FFFFFF;
        if (_lastTile != 0 && p == (_lastTile & 0x00FFFFFF)) return;
        int found = 0;
        for (int steps = 0; steps < 64 && p != (next & 0x00FFFFFF) && p != 0x00FFFFFF; steps++)
        {
            uint a = 0x80000000 | p;
            if (a >= MenuDraw.WindowPackets && a < BackgroundEnd && found < _linked.Length) _linked[found++] = a;
            p = m.ReadU32(a) & 0x00FFFFFF;
        }

        int margin = Margin, tiles = 0;
        for (int shift = 320; shift - 320 < margin; shift += 320)
            foreach (int dx in (ReadOnlySpan<int>)[-shift, shift])
                for (int i = 0; i < found; i++)
                {
                    uint src = _linked[i];
                    int x0 = (short)m.ReadU16(src + 0x08) + dx, x1 = (short)m.ReadU16(src + 0x10) + dx;
                    if (Math.Max(x0, x1) <= -margin || Math.Min(x0, x1) >= 320 + margin) continue;
                    uint q = m.ReadU32(MenuDraw.Cursor);
                    for (uint o = 0; o < PacketSize; o += 4) m.WriteU32(q + o, m.ReadU32(src + o));
                    foreach (uint o in (ReadOnlySpan<uint>)[0x08, 0x10, 0x18, 0x20])
                        m.WriteU16(q + o, (ushort)(m.ReadU16(q + o) + dx));
                    MenuDraw.Link(m, MenuDraw.WindowPacketSlot);
                    _lastTile = q;
                    tiles++;
                }
        if (_measure && _menuSeen.Add(((long)c.RA << 8) | (uint)tiles))
            Console.WriteLine($"[KF1] widescreen: menu presented from {c.RA - 8:X8}, {found} background packet(s), " +
                              $"{tiles} tile(s) added across a {margin} px margin");
    }
}
