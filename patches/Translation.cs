using System.Reflection;
using System.Security.Cryptography;
using ImGuiNET;
using RecompOne.Runtime.Cdrom;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Host.Window;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;
using Rt = RecompOne.Runtime.Runtime;

namespace Kf1;

/// <summary>
/// The English fan translation, "KF Jap to Eng v1.0" (a PPF3 patch), as a setting.
///
///     KF1_TRANSLATION=1|0       on or off for this run, over the saved setting
///     KF1_TRANSLATION_PPF=path  the patch file; by default a *.ppf beside the disc
///
/// The game's code is the same either way. The patch's disc files (images, and the
/// status screen's GPU packets) are applied to sectors as they are read, through
/// fork 0093, except GAME.EXE's: its 63 edited bytes are glyph codes inside
/// instructions, and the recompiled code is the Japanese executable's. Each of those
/// edits only changes a glyph string the code builds on the stack for a text
/// drawer, so the English codes are written into that string at the drawer's entry,
/// from the call sites in <see cref="Sites"/>. One edit in func_800286D4 also
/// stores through t5, a stray write; that prompt gets the strings it meant and no
/// write. Read when the disc opens, so a change applies at the next launch. See
/// "The English translation" in docs/GAME_INTERNALS.md.
/// </summary>
public static class Translation
{
    public const string Key = "kf1.translation";

    // The only patch the sites below were read from.
    const string Sha256 = "18643044e607397564b520f66375c91c9980a7e3fc2a65645e6119d2da0da3fb";

    /// <summary>The setting, as saved or as KF1_TRANSLATION sets it.</summary>
    public static bool Requested { get; private set; }

    /// <summary>The patch is applied this run: decided when the disc opened.</summary>
    public static bool Active { get; private set; }

    /// <summary>Why it is off when it was asked for, or null.</summary>
    public static string? Problem { get; private set; }

    static bool? _forced;
    static string? _ppfPath;
    static bool _decided;

    static readonly ModInfo _self = new() { Id = "kf1.translation", Name = "Translation", Version = "1.0" };

    public static void Configure(string? on, string? ppf)
    {
        if (!string.IsNullOrWhiteSpace(on)) _forced = on.Trim() is not ("0" or "off");
        if (!string.IsNullOrWhiteSpace(ppf)) _ppfPath = ppf.Trim();
    }

    public static void Install()
    {
        // The config loads inside the host window's start-up, after Program.cs.
        RecompOne.Runtime.Events.Event.AddListener<RecompOne.Runtime.Events.RuntimeReadyEvent>(_ =>
            Requested = _forced ?? Rt.View.GetInt(Key, 0) != 0);
        DiscImage.Decorate = Decorate;
        SettingsRegistry.Extend("interface", Draw);
        HookAttach.OnOverlayLoad("translation", Attach);
    }

    // ---- the disc

    static IDiscImage Decorate(IDiscImage image, string path)
    {
        if (!_decided)
        {
            _decided = true;
            if (Requested) Load(image, path);
            Console.WriteLine(Active
                ? $"[KF1] translation: on, {Path.GetFileName(_ppfPath)}: {_records} record(s) in {_bySector!.Count} sector(s), {_skipped} in GAME.EXE left out"
                : Requested ? $"[KF1] translation: off -- {Problem}" : "[KF1] translation: off");
        }
        return Active ? new PatchedImage(image, _bySector!) : image;
    }

    static Dictionary<int, List<(int At, byte[] Data)>>? _bySector;
    static int _records, _skipped;

    static void Load(IDiscImage image, string discPath)
    {
        string? file = _ppfPath ?? Find(Path.GetDirectoryName(Path.GetFullPath(discPath)) ?? ".");
        if (file == null || !File.Exists(file))
        {
            Problem = _ppfPath != null ? $"{_ppfPath} not found" : "no .ppf beside the disc";
            return;
        }
        _ppfPath = file;
        var ppf = File.ReadAllBytes(file);
        if (Convert.ToHexStringLower(SHA256.HashData(ppf)) != Sha256)
        {
            Problem = $"{Path.GetFileName(file)} is not KF Jap to Eng v1.0";
            return;
        }
        if (image.Tracks.Count == 0 || image.Tracks[0].SectorSize != 2352)
        {
            Problem = "the patch needs a 2352-byte-sector image";
            return;
        }

        // PPF3: a 60-byte header, then 1024 bytes of the image from 0x9320 to check
        // against (sector 16 at 32), then records: u64 offset, u8 length, the bytes.
        var raw = image.ReadRawSector(16);
        if (!ppf.AsSpan(60, 1024).SequenceEqual(raw.AsSpan(32, 1024)))
        {
            Problem = "the disc is not the one the patch was made for";
            return;
        }

        var fs = DiscFs.FromImage(image);   // not disposed: it would close the image
        if (!fs.Locate("GAME.EXE", out int exeLba, out uint exeSize))
        {
            Problem = "GAME.EXE not found on the disc";
            return;
        }
        int exeEnd = exeLba + (int)((exeSize + 2047) / 2048);

        bool undo = ppf[58] != 0;
        var bySector = new Dictionary<int, List<(int, byte[])>>();
        for (int p = 1084; p < ppf.Length;)
        {
            long offset = BitConverter.ToInt64(ppf, p);
            int n = ppf[p + 8];
            var data = ppf.AsSpan(p + 9, n).ToArray();
            p += 9 + n + (undo ? n : 0);

            int lba = (int)(offset / 2352);
            if (lba >= exeLba && lba < exeEnd) { _skipped++; continue; }
            if (!bySector.TryGetValue(lba, out var list)) bySector[lba] = list = [];
            list.Add(((int)(offset % 2352), data));
            _records++;
        }
        _bySector = bySector;
        Active = true;
    }

    static string? Find(string dir)
    {
        foreach (var f in Directory.EnumerateFiles(dir, "*.ppf"))
            if (Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(f))) == Sha256) return f;
        return Directory.EnumerateFiles(dir, "*.ppf").FirstOrDefault();
    }

    /// <summary>The image with the patch's records laid over every read.</summary>
    sealed class PatchedImage(IDiscImage inner, Dictionary<int, List<(int At, byte[] Data)>> bySector) : IDiscImage
    {
        public string Format => inner.Format;
        public int FirstTrack => inner.FirstTrack;
        public int LastTrack => inner.LastTrack;
        public bool HasTracks => inner.HasTracks;
        public int LeadoutLba => inner.LeadoutLba;
        public int DataSectors => inner.DataSectors;
        public IReadOnlyList<DiscTrack> Tracks => inner.Tracks;
        public bool TrackStartLba(int track, out int lba) => inner.TrackStartLba(track, out lba);
        public void Dispose() => inner.Dispose();

        public byte[] ReadSectorData(int lba, int size)
        {
            var buf = inner.ReadSectorData(lba, size);
            // Where the images start a read in the raw sector, by its size.
            Lay(buf, lba, size switch { >= 2340 => 12, >= 2329 => 16, _ => 24 });
            return buf;
        }

        public byte[] ReadRawSector(int lba)
        {
            var buf = inner.ReadRawSector(lba);
            Lay(buf, lba, 0);
            return buf;
        }

        void Lay(byte[] buf, int lba, int from)
        {
            if (!bySector.TryGetValue(lba, out var list)) return;
            foreach (var (at, data) in list)
                for (int i = 0; i < data.Length; i++)
                {
                    int j = at + i - from;
                    if ((uint)j < (uint)buf.Length) buf[j] = data[i];
                }
        }
    }

    // ---- the glyph strings in GAME.EXE

    // Where a string is, at the drawer's entry: an argument, or a 20-byte list entry
    // at s0 + (s1 - 1) * 20 (func_800238D8 and func_80023E9C build menus that way).
    enum At { A0, A1, Entry }

    // One halfword: what the Japanese code stores there and what the patch stores
    // instead, as pairs, and anything else that may lawfully be there.
    sealed record Slot(At Base, int Offset, (ushort Jp, ushort En)[] Map, ushort[]? Also = null);

    sealed record Site(string Function, uint Callee, uint[] Returns, Slot[] Slots);

    const uint Choice = 0x800291EC;   // two strings, a0 and a1, and the cursor
    const uint Text = 0x80029DE0;     // one string at a1: x, y, codes, -1
    const uint Menu = 0x8002AD6C;     // a menu whose entries were built before the call

    static Slot S(At b, int off, ushort jp, ushort en) => new(b, off, [(jp, en)]);

    // The status line's conditions go in slots 4..12 by the flags; three change.
    static Slot[] Status() => [.. new[] { 4, 6, 8, 10, 12 }.Select(o =>
        new Slot(At.A1, o, [(0xC9, 0x4143), (0xC7, 0x4144), (0xC8, 0x4143)], [0xFF, 0xC5, 0xC6, 0x88]))];

    static readonly Site[] Sites =
    [
        new("80021FFC", Choice, [0x800220C0, 0x800220EC, 0x80022118, 0x80022168, 0x80022278],
            [S(At.A0, 4, 0x53, 0x00), S(At.A0, 6, 0x6A, 0x06),
             S(At.A1, 4, 0x63, 0x50), S(At.A1, 6, 0x61, 0x00), S(At.A1, 8, 0x6A, 0xFFFF)]),
        new("800238D8", Menu, [0x80023A88],
            [S(At.Entry, 0, 0x59, 0xFF), S(At.Entry, 2, 0x104C, 0x414E), S(At.Entry, 4, 0x4C, 0x414F)]),
        new("80023E9C", Menu, [0x80023FB4],
            [S(At.Entry, 0, 0x59, 0xFF), S(At.Entry, 2, 0x104C, 0x414E), S(At.Entry, 4, 0x4C, 0x414F)]),

        new("80025F38", Text, [0x80025FE4], [S(At.A1, 6, 0x101C, 0x1C), S(At.A1, 8, 0x2A, 0xFF)]),
        new("80025F38", Text, [0x8002610C],
            [S(At.A1, 4, 0x1009, 0x36), S(At.A1, 6, 0x2D, 0x37), S(At.A1, 8, 0x2A, 0xFF), S(At.A1, 10, 0x1013, 0xFF)]),
        new("80025F38", Text, [0x8002647C], Status()),

        new("800264D8", Text, [0x80026590], [S(At.A1, 6, 0x101C, 0x1C)]),
        new("800264D8", Text, [0x80026688],
            [S(At.A1, 4, 0x1009, 0x36), S(At.A1, 6, 0x2D, 0x37), S(At.A1, 10, 0x1013, 0xFF)]),
        new("800264D8", Text, [0x800266B8], [S(At.A1, 6, 0x8B, 0xFF)]),
        new("800264D8", Text, [0x80026724], [S(At.A1, 12, 0x8B, 0xFF)]),
        new("800264D8", Text, [0x80026A94], Status()),
        new("800264D8", Text, [0x80026CE0, 0x80026E14], [S(At.A1, 8, 0x8B, 0xFF)]),
        new("800264D8", Text, [0x80026D1C, 0x80026E48], [S(At.A1, 6, 0xD1, 0xD2), S(At.A1, 8, 0x6A, 0x51)]),
        new("800264D8", Text, [0x80026D48, 0x80026E6C], [S(At.A1, 6, 0xD2, 0xD1), S(At.A1, 8, 0x51, 0x6A)]),
        new("800264D8", Text, [0x80026DB8],
            [S(At.A1, 8, 0x58, 0xFF), S(At.A1, 10, 0x78, 0xFF), S(At.A1, 12, 0x79, 0x78), S(At.A1, 14, 0xFFFF, 0x79)]),
        new("800264D8", Text, [0x80026EBC], [S(At.A1, 6, 0x88, 0x414C), S(At.A1, 8, 0xFFFF, 0x414D)]),
        new("800264D8", Text, [0x80026EF8],
            [S(At.A1, 6, 0x78, 0x4144), S(At.A1, 8, 0x58, 0xFF), S(At.A1, 10, 0x78, 0xFF),
             S(At.A1, 12, 0x79, 0x78), S(At.A1, 14, 0xFFFF, 0x79)]),

        new("80027B7C", Text, [0x80027D58], [S(At.A1, 4, 0x1009, 0x09), S(At.A1, 10, 0x1013, 0xFF)]),

        // Six prompts by s0; s0 == 2 (はい/いいえ) the patch leaves Japanese.
        new("80028380", Choice, [0x80028544, 0x800285CC],
            [new(At.A0, 4, [(0x72, 0x00), (0x75, 0x00), (0x74, 0x00), (0x73, 0x00), (0x70, 0x00)], [0x59]),
             new(At.A0, 6, [(0x42, 0x06), (0x52, 0x06), (0x6A, 0x06), (0x71, 0x06)], [0x41]),
             new(At.A0, 8, [(0x6A, 0xFF)], [0xFFFF]),
             new(At.A1, 4, [(0x63, 0x50)], [0x41]),
             new(At.A1, 6, [(0x61, 0x00)], [0x41]),
             new(At.A1, 8, [(0x6A, 0xFF)], [0x43])]),

        // はい/いいえ. The patch writes "Yes" and then, two bytes off, stores the
        // "No" string's first code from `at` and its second through t5; this is the
        // "No" of the prompts above, [0x50, 0x00, 0xFF], with neither store.
        new("800286D4", Choice, [0x800287DC, 0x800288D4],
            [S(At.A0, 4, 0x59, 0x00), S(At.A0, 6, 0x41, 0x06),
             S(At.A1, 4, 0x41, 0x50), S(At.A1, 6, 0x41, 0x00), S(At.A1, 8, 0x43, 0xFF)]),
    ];

    // A return address is one call, so it names the site on its own.
    static readonly Dictionary<uint, Site> _byReturn = Sites
        .SelectMany(s => s.Returns.Select(r => (r, s))).ToDictionary(x => x.r, x => x.s);

    static readonly HashSet<uint> _seen = [], _refused = [];

    static bool Attach()
    {
        if (!Active) return true;
        SymbolRegistry.Build();
        var targets = Sites.Select(s => s.Callee).Distinct()
            .Select(a => SymbolRegistry.Resolve("game", null, a)).ToList();
        if (targets.Any(t => t == null)) return false;
        var pre = typeof(Translation).GetMethod(nameof(BeforeDrawer), BindingFlags.Public | BindingFlags.Static)!;
        foreach (var t in targets) HookManager.AddPre(_self, t!, pre);
        HookManager.Commit();
        return targets.All(HookAttach.Installed);
    }

    /// <summary>Before a drawer: if the call is one the patch changed a string for,
    /// put the English codes in. Every slot is checked first, and nothing is written
    /// if one holds what neither version stores there.</summary>
    public static void BeforeDrawer(CpuContext c, IMemory m)
    {
        if (!_byReturn.TryGetValue(c.RA, out var site)) return;

        Span<uint> addr = stackalloc uint[site.Slots.Length];
        for (int i = 0; i < site.Slots.Length; i++)
        {
            var s = site.Slots[i];
            uint b = s.Base switch { At.A0 => c.A0, At.A1 => c.A1, _ => c.S0 + (c.S1 - 1) * 20 };
            addr[i] = b + (uint)s.Offset;
            ushort v = m.ReadU16(addr[i]);
            if (!s.Map.Any(p => p.Jp == v || p.En == v) && (s.Also == null || Array.IndexOf(s.Also, v) < 0))
            {
                if (_refused.Add(c.RA))
                    Console.WriteLine($"[KF1] translation: func_{site.Function} at 0x{c.RA:X8}: 0x{v:X4} at +{s.Offset}, " +
                                      "which neither version stores there; left Japanese");
                return;
            }
        }
        for (int i = 0; i < site.Slots.Length; i++)
        {
            ushort v = m.ReadU16(addr[i]);
            foreach (var (jp, en) in site.Slots[i].Map)
                if (v == jp) { m.WriteU16(addr[i], en); break; }
        }
        if (_seen.Add(c.RA))
            Console.WriteLine($"[KF1] translation: func_{site.Function} at 0x{c.RA:X8}: English");
    }

    // ---- the setting

    static void Draw()
    {
        ImGui.SeparatorText("Game text");
        bool on = Requested;
        if (_forced != null) ImGui.BeginDisabled();
        if (ImGui.Checkbox("English translation", ref on))
        {
            Requested = on;
            Rt.View.SetInt(Key, on ? 1 : 0);
            Rt.SaveView();
        }
        if (_forced != null) ImGui.EndDisabled();
        if (ImGui.IsItemHovered(ImGuiHoveredFlags.AllowWhenDisabled))
            ImGui.SetTooltip("The fan translation \"KF Jap to Eng v1.0\", from its .ppf beside the disc. " +
                             "The game plays the same either way; only its text and pictures of text change.");

        string? note =
            _forced != null ? "Set by KF1_TRANSLATION for this run." :
            Requested != Active && Problem == null ? "Applies at the next launch." :
            Requested && Problem != null ? $"Not applied: {Problem}." : null;
        if (note == null) return;
        ImGui.PushStyleColor(ImGuiCol.Text, ImGui.GetStyle().Colors[(int)ImGuiCol.TextDisabled]);
        ImGui.TextWrapped(note);
        ImGui.PopStyleColor();
    }
}
