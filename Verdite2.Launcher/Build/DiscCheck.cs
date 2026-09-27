using RecompOne.Runtime.Cdrom;

namespace Verdite2.Launcher.Build;

/// <summary>
/// The disc validator the runtime has always had a slot for and nobody ever
/// filled.
///
/// Runtime.DiscValidator is a Func&lt;string,string?&gt; consulted by
/// WaitForValidDisc; left null, every existing file passes and any dump at all is
/// accepted. A wrong disc here does not fail, it recompiles into a game that is
/// wrong in ways that surface hours later.
///
/// This port is of King's Field (JP, SLPS-00017), the first game, and the
/// failure worth naming precisely is the North American "King's Field"
/// (SLUS-00158): it is the *second* game (the series was renumbered for the
/// West), it is the disc most people own, and it is verdite2's. The first game's
/// disc has no SYSTEM.CNF at all -- the BIOS boots PSX.EXE -- so a disc that has
/// one is not this game, and its serial says which it is.
/// </summary>
static class DiscCheck
{
    public const string Serial = "SLPS-00017";

    /// <summary>Files the recompile reads, and the smallest each may be, plus one
    /// that only this game has.</summary>
    static readonly (string Path, uint MinSize)[] Required =
    [
        ("PSX.EXE", 0x800),
        ("OPEN.EXE", 0x800),
        ("GAME.EXE", 0x800),
        ("KF/COM/COM.DAT", 1),
    ];

    /// <summary>
    /// Null if the image is usable, otherwise the reason it is not.
    ///
    /// Memoised, because HostWindow.WaitForValidDisc calls this from inside its own
    /// frame loop, and keyed on the image's size and mtime as well as its path, so
    /// a player who fixes the image gets a fresh reading.
    /// </summary>
    public static string? Validate(string path)
    {
        if (string.IsNullOrWhiteSpace(path)) return "No disc image selected.";
        if (!File.Exists(path)) return $"Not found: {path}";

        string key;
        try
        {
            var info = new FileInfo(path);
            key = $"{info.FullName}|{info.Length}|{info.LastWriteTimeUtc.Ticks}";
        }
        catch { return Check(path); }

        lock (_cache)
            if (_cache.TryGetValue(key, out var known)) return known;

        var verdict = Check(path);

        lock (_cache) _cache[key] = verdict;
        return verdict;
    }

    static readonly Dictionary<string, string?> _cache = new(StringComparer.Ordinal);

    static string? Check(string path)
    {
        DiscFs fs;
        try { fs = DiscFs.Open(path); }
        catch (Exception e) { return $"Could not read this image as a cue/bin pair or a CHD: {e.Message}"; }

        using (fs)
        {
            if (fs.Exists("SYSTEM.CNF"))
            {
                string boot;
                try { boot = System.Text.Encoding.ASCII.GetString(fs.ReadFile("SYSTEM.CNF")); }
                catch { boot = ""; }
                return Wrong(boot);
            }

            foreach (var (file, min) in Required)
            {
                if (!fs.Locate(file, out _, out uint size))
                    return $"This disc is missing {file}, which King's Field ({Serial}) has. " +
                           "It is either another game or an incomplete image.";
                if (size < min)
                    return $"{file} is {size} bytes on this disc; {Serial} has at least {min}. The image may be truncated.";
            }
        }

        return null;
    }

    /// <summary>
    /// Name the disc the player actually inserted. The serial in SYSTEM.CNF is
    /// written "cdrom:\SLUS_001.58;1", i.e. the boot file name.
    /// </summary>
    static string Wrong(string systemCnf)
    {
        var found = Serials(systemCnf);
        string got = found is null ? "" : $" This one is {found}.";

        if (found is "SLUS-00158" or "SLPS-00069")
            return "This is King's Field II (in North America it was released as \"King's Field\"). " +
                   $"This port is of the first game, King's Field ({Serial}), released only in Japan. " +
                   "King's Field II has a port of its own: verdite2.";
        if (found is "SLUS-00255" or "SLPS-00377")
            return $"This is King's Field III (King's Field II in North America). This port is of the first game, King's Field ({Serial}).";

        return $"This is not King's Field ({Serial}).{got}";
    }

    static string? Serials(string systemCnf)
    {
        foreach (var raw in systemCnf.Split('\n'))
        {
            int at = raw.IndexOf("cdrom", StringComparison.OrdinalIgnoreCase);
            if (at < 0) continue;

            var name = raw[at..].Trim().TrimEnd('\r');
            int slash = name.LastIndexOfAny(['\\', '/', ':']);
            if (slash >= 0) name = name[(slash + 1)..];
            name = name.Split(';')[0].Trim();

            // SLUS_001.58 -> SLUS-00158
            if (name.Length == 11 && name[4] == '_' && name[8] == '.')
                return $"{name[..4]}-{name[5..8]}{name[9..]}".ToUpperInvariant();
        }
        return null;
    }
}
