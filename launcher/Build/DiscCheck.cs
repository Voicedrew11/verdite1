using RecompOne.Runtime.Cdrom;

namespace Verdite.Launcher.Build;

/// <summary>
/// The disc validator the runtime has always had a slot for.
///
/// Runtime.DiscValidator is a Func&lt;string,string?&gt; consulted by
/// WaitForValidDisc; left null, every existing file passes and any dump at all is
/// accepted. That is survivable while the only user is a developer pointing at a
/// known-good image, and it is not survivable in a release: a wrong disc does not
/// fail, it recompiles into a game that is wrong in ways that surface hours later.
///
/// Two failures are worth naming precisely rather than generically:
///
///   - The disc a player is likely to reach for instead. The King's Field series
///     was renumbered for the West, so the US-boxed sequel of one port is the
///     other port's game. Every address in config/ is wrong for it and it would
///     build. The game names those discs (LauncherGame.WrongDiscs).
///
///   - A truncated or differently built archive. Area modules are sliced out by
///     absolute byte offset, so a short file would let the recompile pass and then
///     produce modules made of whatever bytes were at those offsets. Every file the
///     config slices must reach as far as its furthest slice (ShippedConfig).
/// </summary>
static class DiscCheck
{
    /// <summary>
    /// Null if the image is usable, otherwise the reason it is not.
    ///
    /// Memoised, because HostWindow.WaitForValidDisc calls this from inside its own
    /// frame loop: a saved CdPath that no longer validates -- a moved image, a dump
    /// replaced in place, a settings.json written before there was a validator at
    /// all -- would otherwise reopen the cue and re-parse the ISO directory sixty
    /// times a second for as long as the picker is up, which is exactly when the
    /// interface has to stay responsive.
    ///
    /// Keyed on the image's size and mtime as well as its path, and nothing is
    /// cached for a path with no file at it, so a player who puts a missing image
    /// back or re-points the cue at the right bin gets a fresh reading rather than
    /// the verdict from before they fixed it. What that does NOT see is a bin
    /// swapped under an unchanged cue -- a CHD, being one file, has no such gap --
    /// and it does not need to: a wrong disc is refused here and never saved, so
    /// the only path that reaches the loop is one that validated when it was
    /// chosen.
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
        var game = Launcher.Game;

        // DiscFs.Open dispatches on the extension, falling back to the CHD magic
        // for a file named neither .cue nor .chd, so one message covers every way
        // it can refuse: an unreadable cue/bin pair, an unsupported CHD codec (only
        // cdzl/cdlz/cdfl/zlib/lzma are decoded, so a `chdman -c cdzs` image lands
        // here rather than crashing during the build), and a file that is neither.
        DiscFs fs;
        try { fs = DiscFs.Open(path); }
        catch (Exception e) { return $"Could not read this image as a cue/bin pair or a CHD: {e.Message}"; }

        using (fs)
        {
            string boot;
            try { boot = System.Text.Encoding.ASCII.GetString(fs.ReadFile("SYSTEM.CNF")); }
            catch { return "No SYSTEM.CNF on this disc, so it is not a PlayStation game image."; }

            if (boot.IndexOf(game.BootFile, StringComparison.OrdinalIgnoreCase) < 0)
                return Wrong(boot);

            foreach (var (file, floor) in ShippedConfig.DiscFiles)
            {
                if (!fs.Locate(file, out _, out uint size))
                    return $"This disc is missing {file}, which the recompiler needs. The image may be incomplete.";
                if (size < floor)
                    return $"{file} is {size} bytes on this disc; {game.Serial} has at least {floor}. The image may be truncated.";
            }
        }

        return null;
    }

    /// <summary>
    /// Name the disc the player actually inserted, so the message is about their
    /// disc rather than about ours. The serial in SYSTEM.CNF is written
    /// "cdrom:\SLUS_002.55;1", i.e. the boot file name, so it is recovered from
    /// that rather than looked up.
    /// </summary>
    static string Wrong(string systemCnf)
    {
        var game = Launcher.Game;
        var found = Serials(systemCnf);

        if (found is not null && game.WrongDiscs.TryGetValue(found, out var message))
            return message;

        string got = found is null ? "" : $" This one is {found}.";
        return $"This is not {game.GameTitle} ({game.Serial}).{got}";
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
