namespace Kf1;

/// <summary>
/// Reads a two-level picture of text back into lines, through a table of the font's
/// ink runs. Nothing here knows the game: a picture is ink or not per pixel, a font
/// says where its lines sit and what each run of inked columns spells.
///
/// A line is cut into maximal runs of inked columns. Each run is keyed by FNV-1a 64
/// over its width and its rows (bit x from the run's left edge) and looked up; a run
/// the table does not hold, or ink outside every line, and the whole picture is
/// refused, so a picture is either read exactly or left alone. The tables are made by
/// scripts/msg_glyphs.py, which cuts the disc's pictures the same way. See "Message
/// text" in docs/GAME_INTERNALS.md.
/// </summary>
public static class PictureText
{
    /// <summary>One glyph's text (a word space, if one came before it, leads it) and
    /// the columns its run, or pair of runs, took.</summary>
    public readonly record struct Piece(int X, int Right, string Text);
    public readonly record struct Line(int Top, int Baseline, Piece[] Pieces);

    /// <summary>What <see cref="Font.Settle"/> sees between two pieces of a line.</summary>
    public const char Join = '\u001F';

    public sealed class Font
    {
        public required string Name { get; init; }
        /// <summary>Fixed lines: tops at Top + Pitch·j, Height rows each. Zero: lines
        /// found by their ink and placed by their baseline.</summary>
        public int Top { get; init; }
        public int Pitch { get; init; }
        /// <summary>Rows keyed: the band's height, or Up + 1 + Down around a baseline.</summary>
        public int Height { get; init; }
        public int Up { get; init; }
        public int Down { get; init; }
        /// <summary>Bytes a row takes in the key: 2 (u16) or 4 (u32).</summary>
        public int RowBytes { get; init; }
        /// <summary>Empty columns that make a word space, and what it reads as.</summary>
        public int SpaceGap { get; init; }
        public string Space { get; init; } = " ";
        public required IReadOnlyDictionary<ulong, string> Runs { get; init; }
        /// <summary>Glyphs that come out as two runs: key of (left, right, gap).</summary>
        public IReadOnlyDictionary<ulong, string> Pairs { get; init; } = new Dictionary<ulong, string>();
        /// <summary>A last pass over the whole text, for what one run cannot say alone:
        /// lines joined by '\n', pieces by <see cref="Join"/>; it keeps both.</summary>
        public Func<string, string>? Settle { get; init; }

        public bool Fixed => Pitch > 0;
    }

    /// <summary>Read <paramref name="ink"/> (w × h, row-major) as <paramref name="font"/>.
    /// Empty string on success, otherwise why not.</summary>
    public static string Read(bool[] ink, int w, int h, Font font, out Line[] lines)
    {
        lines = [];
        var bands = font.Fixed ? FixedBands(ink, w, h, font) : FoundBands(ink, w, h, font.Up + 1 + font.Down);
        if (bands == null) return "ink between the lines";

        var outLines = new List<Line>();
        var runs = new List<(int L, int R)>();
        var pieces = new List<Piece>();
        var sb = new System.Text.StringBuilder();
        foreach (var (y0, y1) in bands)
        {
            Runs(ink, w, y0, y1, runs);
            if (runs.Count == 0) continue;

            int top, baseline;
            if (font.Fixed) { top = y0; baseline = y0 + font.Height - 1; }
            else
            {
                baseline = Baseline(ink, w, y0, y1, runs);
                top = baseline - font.Up;
                if (y0 < top || y1 > baseline + font.Down) return $"ink outside the line at y {y0}";
            }

            pieces.Clear();
            sb.Clear();
            for (int i = 0; i < runs.Count; i++)
            {
                if (i > 0 && runs[i].L - runs[i - 1].R - 1 >= font.SpaceGap) sb.Append(font.Space);
                int left = runs[i].L;
                ulong k = Key(ink, w, runs[i], top, font);
                string? s = null;
                if (i + 1 < runs.Count && font.Pairs.Count > 0)
                {
                    int gap = runs[i + 1].L - runs[i].R - 1;
                    if (font.Pairs.TryGetValue(PairKey(k, Key(ink, w, runs[i + 1], top, font), gap), out var both)) { s = both; i++; }
                }
                if (s == null && !font.Runs.TryGetValue(k, out s))
                    return $"unknown run at line {outLines.Count}, x {runs[i].L}, width {runs[i].R - runs[i].L + 1}";
                sb.Append(s);
                pieces.Add(new Piece(left, runs[i].R + 1, sb.ToString()));
                sb.Clear();
            }
            outLines.Add(new Line(top, baseline, pieces.ToArray()));
        }
        if (outLines.Count == 0) return "blank";

        if (font.Settle != null)
        {
            var all = font.Settle(string.Join('\n', outLines.Select(l => string.Join(Join, l.Pieces.Select(p => p.Text))))).Split('\n');
            if (all.Length != outLines.Count) return "settle broke the lines";
            for (int i = 0; i < outLines.Count; i++)
            {
                var texts = all[i].Split(Join);
                var ps = outLines[i].Pieces;
                if (texts.Length != ps.Length) return "settle broke the pieces";
                for (int j = 0; j < ps.Length; j++) ps[j] = ps[j] with { Text = texts[j] };
            }
        }
        lines = outLines.ToArray();
        return "";
    }

    static List<(int, int)>? FixedBands(bool[] ink, int w, int h, Font f)
    {
        var bands = new List<(int, int)>();
        for (int y0 = f.Top; y0 + f.Height <= h; y0 += f.Pitch) bands.Add((y0, y0 + f.Height - 1));
        for (int y = 0; y < h; y++)
        {
            if (bands.Any(b => y >= b.Item1 && y <= b.Item2)) continue;
            for (int x = 0; x < w; x++) if (ink[y * w + x]) return null;
        }
        return bands;
    }

    // Maximal runs of inked rows; two split by a single empty row are one line if
    // together they still fit one, as scripts/msg_glyphs.py cuts them.
    static List<(int, int)> FoundBands(bool[] ink, int w, int h, int fit)
    {
        var bands = new List<(int, int)>();
        int start = -1;
        for (int y = 0; y <= h; y++)
        {
            bool any = false;
            if (y < h) for (int x = 0; x < w && !any; x++) any = ink[y * w + x];
            if (any && start < 0) start = y;
            else if (!any && start >= 0)
            {
                if (bands.Count > 0 && start - bands[^1].Item2 == 2 && y - bands[^1].Item1 <= fit)
                    bands[^1] = (bands[^1].Item1, y - 1);
                else bands.Add((start, y - 1));
                start = -1;
            }
        }
        return bands;
    }

    static void Runs(bool[] ink, int w, int y0, int y1, List<(int, int)> runs)
    {
        runs.Clear();
        int start = -1;
        for (int x = 0; x <= w; x++)
        {
            bool any = false;
            if (x < w) for (int y = y0; y <= y1 && !any; y++) any = ink[y * w + x];
            if (any && start < 0) start = x;
            else if (!any && start >= 0) { runs.Add((start, x - 1)); start = -1; }
        }
    }

    // The bottom row most runs end on; a tie goes to the lower row.
    static int Baseline(bool[] ink, int w, int y0, int y1, List<(int L, int R)> runs)
    {
        var votes = new Dictionary<int, int>();
        foreach (var (l, r) in runs)
        {
            int bottom = y0;
            for (int y = y0; y <= y1; y++)
                for (int x = l; x <= r; x++)
                    if (ink[y * w + x]) bottom = y;
            votes[bottom] = votes.GetValueOrDefault(bottom) + 1;
        }
        return votes.OrderByDescending(v => v.Value).ThenByDescending(v => v.Key).First().Key;
    }

    static ulong Key(bool[] ink, int w, (int L, int R) run, int top, Font f)
    {
        int h = ink.Length / w;
        ulong k = Fnv(0xCBF29CE484222325UL, (byte)(run.R - run.L + 1));
        for (int r = 0; r < f.Height; r++)
        {
            uint bits = 0;
            int y = top + r;
            if (y >= 0 && y < h)
                for (int x = run.L; x <= run.R; x++)
                    if (ink[y * w + x]) bits |= 1u << (x - run.L);
            for (int b = 0; b < f.RowBytes; b++) k = Fnv(k, (byte)(bits >> (8 * b)));
        }
        return k;
    }

    public static ulong PairKey(ulong left, ulong right, int gap)
    {
        ulong k = 0xCBF29CE484222325UL;
        for (int b = 0; b < 8; b++) k = Fnv(k, (byte)(left >> (8 * b)));
        for (int b = 0; b < 8; b++) k = Fnv(k, (byte)(right >> (8 * b)));
        return Fnv(k, (byte)gap);
    }

    static ulong Fnv(ulong h, byte b) => (h ^ b) * 0x100000001B3UL;
}
