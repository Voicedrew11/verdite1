using System.Numerics;
using System.Reflection;
using ImGuiNET;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Diagnostics;
using RecompOne.Runtime.Host.Window;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// Dialogue (KF\TALK) as text: the event picture <c>func_8002C794</c> is read back
/// into characters through <see cref="MessageGlyphs"/>, its picture is hidden, and the
/// characters are drawn in the game's own font -- each glyph's pixels and colours
/// as the disc has them, kept by character as pictures are read -- where the game
/// put them, under the fade's brightness. A picture with a run the tables do not
/// know is left as the game drew it. Off by default (KF1_MESSAGETEXT). See "Message
/// text" in docs/GAME_INTERNALS.md.
/// </summary>
public sealed class MessageText : IFloatingPanel
{
    public static readonly MessageText Instance = new();
    MessageText() { }

    const uint EventPicture = 0x8002C794, LoadTim = 0x8001B100, DrawOTag = 0x80050CF0;
    const uint LoadTimReturn = 0x8002C87C, DrawOTagReturn = 0x8002C938;
    const uint TalkName = 0x8005606C;   // func_8002C9D4's "TALK\Cxx\Txxxxx.TIM"

    // The picture's quad: texel (u, v) lands on screen (32 + u, 112 + v); v < 128 is shown.
    const int ScreenX = 32, ScreenY = 112, PicW = 256, PicH = 128;

    static readonly PictureText.Font Japanese = new()
    {
        Name = "ja", Top = 9, Pitch = 15, Height = 12, RowBytes = 2, SpaceGap = int.MaxValue,
        Runs = MessageGlyphs.JapaneseRuns, Pairs = MessageGlyphs.JapanesePairs, Settle = SettleKana,
    };

    static readonly PictureText.Font English = new()
    {
        Name = "en", Up = 11, Down = 4, Height = 16, RowBytes = 4, SpaceGap = 4,
        Runs = MessageGlyphs.EnglishRuns, Pairs = MessageGlyphs.EnglishPairs, Settle = SettleCapitalI,
    };

    /// <summary>A glyph as the game drew it: Height rows of 15-bit colours, 0 for none.</summary>
    sealed record Glyph(int W, int H, ushort[] Px);

    // The game's font, by font, character and width (one character can come in two cuts).
    static readonly Dictionary<(string Font, string Text, int W), Glyph> _font = new();

    // This picture's runs the tables lack, by (line top, x): drawn as they are, marked.
    static readonly Dictionary<(int Top, int X), Glyph> _marked = new();
    const ushort MarkColour = 0x7C1F;   // magenta

    public static bool Enabled { get; set; }
    /// <summary>Read a picture with runs the tables lack, and draw those in magenta
    /// (KF1_MESSAGETEXT_MARK, on), instead of leaving the picture.</summary>
    public static bool Mark { get; set; } = true;
    static bool _probe, _queued;

    static string _name = "";
    static bool _inEvent;
    static PictureText.Font? _shown;
    static PictureText.Line[] _lines = [];
    static int _r = 128, _g = 128, _b = 128;
    static int _read, _refused, _drawn, _exact, _unknown;

    public string Name => "kf1message";
    public bool IsOpen { get => _lines.Length > 0 && Enabled; set { } }

    static readonly ModInfo _self = new() { Id = "kf1.messagetext", Name = "Message text", Version = "0.1" };

    // KF1_MESSAGETEXT_TEST: the next sign opens these TALK pictures instead, in turn.
    static readonly Queue<string> _tests = new();

    public static void Configure(string? on, string? probe, string? test = null, string? mark = null)
    {
        if (!string.IsNullOrWhiteSpace(mark)) Mark = mark.Trim() is not ("0" or "off");
        foreach (var t in (test ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            if (t.Length == 10) _tests.Enqueue(t.Replace('/', '\\'));
        Enabled = string.IsNullOrWhiteSpace(on) || on.Trim() is not ("0" or "off");
        if (!string.IsNullOrWhiteSpace(probe)) _probe = probe != "0";
    }

    public static void Install()
    {
        RecompOne.Runtime.Events.Event.AddListener<RecompOne.Runtime.Events.RuntimeReadyEvent>(_ =>
            PanelManager.Register(Instance));
        HookAttach.OnOverlayLoad("message text", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var show = SymbolRegistry.Resolve("game", null, EventPicture);
        var load = SymbolRegistry.Resolve("game", null, LoadTim);
        var draw = SymbolRegistry.Resolve("game", null, DrawOTag);
        if (show == null || load == null || draw == null) return false;

        MethodInfo M(string n) => typeof(MessageText).GetMethod(n, BindingFlags.Public | BindingFlags.Static)!;
        if (!_queued)
        {
            _queued = true;
            HookManager.AddPre(_self, show, M(nameof(BeforeShow)));
            HookManager.AddPost(_self, show, M(nameof(AfterShow)));
            HookManager.AddPre(_self, load, M(nameof(BeforeLoad)));
            HookManager.AddPre(_self, draw, M(nameof(BeforeDraw)));
        }
        HookManager.Commit();
        bool ok = HookAttach.Installed(show) && HookAttach.Installed(load) && HookAttach.Installed(draw);
        Console.WriteLine($"[KF1] message text: {(Enabled ? "on" : "off")}{(_probe ? ", probe" : "")}, " +
                          $"{MessageGlyphs.JapaneseRuns.Count} + {MessageGlyphs.EnglishRuns.Count} runs");
        return ok;
    }

    public static void BeforeShow(CpuContext c, IMemory m)
    {
        if (_tests.Count > 0 && c.A0 != TalkName)
        {
            var name = $"TALK\\{_tests.Dequeue()}.TIM";
            for (int i = 0; i < name.Length; i++) m.WriteU8(TalkName + (uint)i, (byte)name[i]);
            m.WriteU8(TalkName + (uint)name.Length, 0);
            c.A0 = TalkName;
        }
        var sb = new System.Text.StringBuilder();
        for (uint a = c.A0; sb.Length < 64; a++)
        {
            byte b = m.ReadU8(a);
            if (b == 0) break;
            sb.Append((char)b);
        }
        _name = sb.ToString();
        _inEvent = true;
        _lines = [];
        _r = _g = _b = 0;
    }

    public static void AfterShow(CpuContext c, IMemory m)
    {
        if (_probe && _lines.Length > 0)
            Console.WriteLine($"[KF1] message text: {_name} drawn on {_drawn} present(s)");
        _inEvent = false;
        _lines = [];
        _drawn = 0;
    }

    public static void BeforeDraw(CpuContext c, IMemory m)
    {
        if (!_inEvent || c.RA != DrawOTagReturn) return;
        _r = m.ReadU8(c.SP + 0x14u);
        _g = m.ReadU8(c.SP + 0x15u);
        _b = m.ReadU8(c.SP + 0x16u);
    }

    public static void BeforeLoad(CpuContext c, IMemory m)
    {
        if (!_inEvent || c.RA != LoadTimReturn || (!Enabled && !_probe)) return;
        if (!_name.Contains("TALK\\", StringComparison.OrdinalIgnoreCase)) return;

        string why = Decode(m, c.A0, out var font, out var lines, out var colour, out uint clut, out _unknown);
        if (why.Length > 0)
        {
            _refused++;
            if (_probe) Console.WriteLine($"[KF1] message text: {_name} left as the picture -- {why} ({_read} read, {_refused} refused)");
            return;
        }

        _read++;
        Learn(font!, lines, colour);
        bool exact = Same(font!, lines, colour);
        if (exact) _exact++;
        if (_probe)
            Console.WriteLine($"[KF1] message text: {_name} read as {font!.Name}, {lines.Length} line(s), " +
                              $"{lines.Sum(l => l.Pieces.Length)} glyph(s), {_unknown} marked, redrawn {(exact ? "exactly" : "NOT exactly")} " +
                              $"({_read} read, {_exact} exact, {_refused} refused)");
        if (!Enabled || !exact) return;

        // Hide the picture: its first CLUT row, the one the quad uses, all transparent.
        for (uint i = 0; i < 16; i++) m.WriteU16(clut + 2 * i, 0);
        _shown = font;
        _lines = lines;
    }

    // The TIM at a0: 4-bit, its first CLUT row, 256 texels wide.
    static string Decode(IMemory m, uint tim, out PictureText.Font? font, out PictureText.Line[] lines,
                         out ushort[] colour, out uint clut, out int unknown)
    {
        font = null; lines = []; colour = new ushort[PicW * PicH]; clut = 0; unknown = 0;
        uint flags = m.ReadU32(tim + 4);
        if (m.ReadU32(tim) != 0x10 || (flags & 7) != 0 || (flags & 8) == 0) return "not a 4-bit TIM with a CLUT";
        uint clutLen = m.ReadU32(tim + 8);
        clut = tim + 20;
        uint img = tim + 8 + clutLen;
        int iw = m.ReadU16(img + 8) * 4, ih = m.ReadU16(img + 10);
        if (iw != PicW || ih < PicH) return $"picture is {iw} x {ih}";

        var pal = new ushort[16];
        var bright = new bool[16];
        for (int i = 0; i < 16; i++)
        {
            ushort p = pal[i] = m.ReadU16(clut + 2 * (uint)i);
            int r = (p & 31) * 255 / 31, g = (p >> 5 & 31) * 255 / 31, b = (p >> 10 & 31) * 255 / 31;
            bright[i] = (r * 299 + g * 587 + b * 114) / 1000 > 128;
        }

        var ink = new bool[PicW * PicH];
        uint data = img + 12;
        for (int y = 0; y < PicH; y++)
            for (int x = 0; x < PicW; x += 2)
            {
                byte two = m.ReadU8(data + (uint)(y * PicW + x) / 2);
                for (int k = 0; k < 2; k++)
                {
                    int idx = k == 0 ? two & 15 : two >> 4;
                    colour[y * PicW + x + k] = pal[idx];
                    ink[y * PicW + x + k] = bright[idx];
                }
            }

        // The translation's pictures first when it is on; an untranslated one still reads as Japanese.
        var order = Translation.Active ? new[] { English, Japanese } : new[] { Japanese, English };
        string why = "";
        foreach (var f in order)
        {
            string w = PictureText.Read(ink, PicW, PicH, f, out lines);
            if (w.Length == 0) { font = f; return ""; }
            if (why.Length == 0) why = $"{f.Name}: {w}";
        }
        if (!Mark) return why;

        // Read with what the tables lack kept; the font that lacks least, and only a few.
        foreach (var f in order)
        {
            if (PictureText.Read(ink, PicW, PicH, f, true, out var got, out int n).Length > 0) continue;
            if (n * 4 > got.Sum(l => l.Pieces.Length) || (font != null && n >= unknown)) continue;
            font = f; lines = got; unknown = n;
        }
        return font != null ? "" : why;
    }

    static (int X, int Y, int W, int H) Box(PictureText.Font f, PictureText.Line l, PictureText.Piece p) =>
        (p.X, l.Top, p.Right - p.X, f.Height);

    static string Char(PictureText.Piece p) => p.Text.TrimStart(' ');

    static bool Lacked(PictureText.Piece p) => Char(p) == PictureText.Unknown;

    static Glyph? Find(PictureText.Font f, PictureText.Line l, PictureText.Piece p) =>
        Lacked(p) ? _marked.GetValueOrDefault((l.Top, p.X))
                  : _font.GetValueOrDefault((f.Name, Char(p), p.Right - p.X));

    // Keep each glyph as the game drew it, by its character; a lacked one, by its place.
    static void Learn(PictureText.Font f, PictureText.Line[] lines, ushort[] colour)
    {
        _marked.Clear();
        foreach (var l in lines)
            foreach (var p in l.Pieces)
            {
                var (x0, y0, w, h) = Box(f, l, p);
                var key = (f.Name, Char(p), w);
                if (!Lacked(p) && _font.ContainsKey(key)) continue;
                var px = new ushort[w * h];
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        if (y0 + y is >= 0 and < PicH) px[y * w + x] = colour[(y0 + y) * PicW + x0 + x];
                if (Lacked(p)) _marked[(l.Top, p.X)] = new Glyph(w, h, px);
                else _font[key] = new Glyph(w, h, px);
            }
    }

    // The text set again in the game's font gives back the picture's every pixel; a
    // coloured pixel that is not ink fails it, and that picture stays a picture.
    static bool Same(PictureText.Font f, PictureText.Line[] lines, ushort[] colour)
    {
        var again = new ushort[PicW * PicH];
        foreach (var l in lines)
            foreach (var p in l.Pieces)
            {
                var (x0, y0, _, _) = Box(f, l, p);
                if (Find(f, l, p) is not { } g) return false;
                for (int y = 0; y < g.H; y++)
                    for (int x = 0; x < g.W; x++)
                        if (y0 + y is >= 0 and < PicH && g.Px[y * g.W + x] != 0)
                            again[(y0 + y) * PicW + x0 + x] = g.Px[y * g.W + x];
            }
        return again.AsSpan().SequenceEqual(colour);
    }

    // "~XY": X (katakana) beside katakana or a long-vowel bar, else Y.
    static string SettleKana(string s)
    {
        static bool Kana(char c) => c is >= 'ァ' and <= 'ー';
        var o = new System.Text.StringBuilder(s.Length);
        for (int i = 0; i < s.Length; i++)
        {
            if (s[i] != '~' || i + 2 >= s.Length) { o.Append(s[i]); continue; }
            char prev = '\0';
            for (int k = o.Length - 1; k >= 0 && prev == '\0'; k--) if (o[k] != PictureText.Join) prev = o[k];
            char next = '\0';
            for (int k = i + 3; k < s.Length && next == '\0'; k++) if (s[k] != PictureText.Join) next = s[k] == '~' && k + 1 < s.Length ? s[k + 1] : s[k];
            o.Append(Kana(prev) || Kana(next) ? s[i + 1] : s[i + 2]);
            i += 2;
        }
        return o.ToString();
    }

    // "~lI": a capital I where the word says so -- beside a capital, alone, before an
    // apostrophe, or opening a sentence; else l.
    static string SettleCapitalI(string s)
    {
        var o = new System.Text.StringBuilder(s.Length);
        for (int i = 0; i < s.Length; i++)
        {
            if (s[i] != '~' || i + 2 >= s.Length) { o.Append(s[i]); continue; }
            char Prev(int skip)
            {
                for (int k = o.Length - 1; k >= 0; k--)
                    if (o[k] != PictureText.Join && skip-- == 0) return o[k];
                return '\0';
            }
            char next = '\0';
            for (int k = i + 3; k < s.Length && next == '\0'; k++) if (s[k] != PictureText.Join) next = s[k] == '~' ? 'l' : s[k];
            char prev = Prev(0);
            bool starts = !char.IsLetter(prev);
            bool capital = char.IsUpper(prev) || char.IsUpper(next);
            bool alone = starts && !char.IsLetter(next);
            bool sentence = starts && (prev is '\0' or '\n' || (prev == ' ' && Prev(1) is '.' or '!' or '?' or '\0' or '\n'));
            o.Append(capital || alone || (starts && next is '\'' or '’') || sentence ? s[i + 2] : s[i + 1]);
            i += 2;
        }
        return o.ToString();
    }

    public void Draw()
    {
        var lines = _lines;
        var f = _shown;
        if (!Enabled || lines.Length == 0 || f == null) return;

        Vector2 g0, g1;
        if (OutputView.Valid && OutputView.Size.X >= 32f && OutputView.Size.Y >= 32f) { g0 = OutputView.Min; g1 = OutputView.Max; }
        else { var vp = ImGui.GetMainViewport(); g0 = vp.WorkPos; g1 = vp.WorkPos + vp.WorkSize; }
        float scale = (g1.Y - g0.Y) / 240f;
        float cx = (g0.X + g1.X) * 0.5f;
        Vector2 Screen(int x, int y) => new(cx + (ScreenX + x - 160) * scale, g0.Y + (ScreenY + y) * scale);

        ImGui.SetNextWindowPos(g0);
        ImGui.SetNextWindowSize(g1 - g0);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags.NoDecoration | ImGuiWindowFlags.NoInputs | ImGuiWindowFlags.NoNav |
            ImGuiWindowFlags.NoFocusOnAppearing | ImGuiWindowFlags.NoSavedSettings |
            ImGuiWindowFlags.NoDocking | ImGuiWindowFlags.NoBackground;
        if (ImGui.Begin("##kf1message", flags))
        {
            var dl = ImGui.GetWindowDrawList();
            foreach (var l in lines)
                foreach (var p in l.Pieces)
                {
                    var (x0, y0, w, h) = Box(f, l, p);
                    if (Find(f, l, p) is not { } g) continue;
                    bool lacked = Lacked(p);
                    if (lacked)
                        dl.AddRect(Screen(x0 - 1, y0), Screen(x0 + w + 1, y0 + h), Modulate(MarkColour), 0f, ImDrawFlags.None, Math.Max(1f, scale * 0.5f));
                    for (int y = 0; y < g.H; y++)
                        for (int x = 0; x < g.W;)
                        {
                            ushort px = g.Px[y * g.W + x];
                            int end = x + 1;
                            while (end < g.W && g.Px[y * g.W + end] == px) end++;
                            if (px != 0)
                                dl.AddRectFilled(Screen(x0 + x, y0 + y), Screen(x0 + end, y0 + y + 1), Modulate(lacked ? MarkColour : px));
                            x = end;
                        }
                }
            _drawn++;
        }
        ImGui.End();
    }

    // The quad's texture blend: texel x brightness / 128, as the GPU does it.
    static uint Modulate(ushort px)
    {
        static float Ch(int t, int b) => Math.Min(255, (t * 255 / 31) * b / 128) / 255f;
        return ImGui.GetColorU32(new Vector4(Ch(px & 31, _r), Ch(px >> 5 & 31, _g), Ch(px >> 10 & 31, _b), 1f));
    }
}
