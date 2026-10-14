namespace Verdite.Launcher.Build;

/// <summary>
/// Where the launcher reads its payload from, and where everything it writes goes.
///
/// The runtime addresses every file it owns with a bare relative path --
/// ConfigManager's "settings.json" and "interface.ini", Runtime's "carda.sav" and
/// "cardb.sav", a game's own files beside them, and ModLoader's
/// Path.GetFullPath("mods") with its .cache of compiled mod assemblies. All of
/// those resolve against the process working directory, which for a shortcut, a
/// desktop entry or an AppImage is wherever the launcher happened to be started
/// from, and for an installed build is a directory the player cannot write to.
///
/// So the launcher chdirs into the data directory before touching the runtime at
/// all. That is the entire fix, and it is why none of the paths above needed a
/// patch: they keep resolving relatively and land in the right place.
/// </summary>
static class Paths
{
    /// <summary>
    /// The shipped tree: read-only, and the only thing shipped. On Linux (and
    /// a developer run) this is the directory the executable sits in. On the
    /// Windows package the runtime lives in bin/ next to the real apphost, so
    /// this is the parent -- content/ stays beside the stub the player launches
    /// rather than mixed in with a hundred DLLs.
    /// </summary>
    public static string Install { get; } = ResolveInstall();

    public static string Content { get; } = Path.Combine(Install, "content");
    public static string ContentConfig { get; } = Path.Combine(Install, "content", "config");
    public static string ContentSrc { get; } = Path.Combine(Install, "content", "src");
    public static string ContentMods { get; } = Path.Combine(Install, "content", "mods");

    /// <summary>
    /// Per-user, writable, and stable across updates:
    ///   $VERDITE2_DATA (the game's launcher prefix), if set
    ///   %LOCALAPPDATA%\Verdite2
    ///   $XDG_DATA_HOME/verdite2, else ~/.local/share/verdite2
    /// Saves live here, so it deliberately does not move when the install does.
    /// </summary>
    public static string Data { get; } = ResolveData();

    /// <summary>Built game assemblies, one directory per cache key.</summary>
    public static string Builds => Path.Combine(Data, "builds");

    public static string BuildLog => Path.Combine(Data, "build.log");

    static string ResolveInstall()
    {
        var dir = Path.GetFullPath(AppContext.BaseDirectory)
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

        if (Directory.Exists(Path.Combine(dir, "content")))
            return dir;

        // Windows package: we were started as bin\Verdite2.exe, payload is one
        // directory up. TrimEnd above is load-bearing -- BaseDirectory usually
        // carries a trailing slash, and GetDirectoryName of a slash-terminated
        // path is the path itself, not the parent.
        var parent = Path.GetDirectoryName(dir);
        if (!string.IsNullOrEmpty(parent) && Directory.Exists(Path.Combine(parent, "content")))
            return parent;

        return dir;
    }

    static string ResolveData()
    {
        var game = Launcher.Game;

        // An explicit override, for a second install, a save directory on another
        // drive, or a test that must not write into the player's real one.
        var pinned = game.Env("DATA");
        if (!string.IsNullOrWhiteSpace(pinned))
            return Path.GetFullPath(pinned);

        if (OperatingSystem.IsWindows())
        {
            var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            if (!string.IsNullOrWhiteSpace(local)) return Path.Combine(local, game.Name);
        }

        var xdg = Environment.GetEnvironmentVariable("XDG_DATA_HOME");
        if (!string.IsNullOrWhiteSpace(xdg) && Path.IsPathRooted(xdg))
            return Path.Combine(xdg, game.AppId);

        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (string.IsNullOrWhiteSpace(home)) home = Directory.GetCurrentDirectory();
        return Path.Combine(home, ".local", "share", game.AppId);
    }

    /// <summary>
    /// Make the data directory, move into it, and seed the mods the port ships.
    ///
    /// Seeding copies rather than links, and only files that are not there yet, so
    /// a player who has edited a shipped mod keeps their edit. mods/.cache is the
    /// ModLoader's own and is never seeded.
    ///
    /// "Not there yet" is not enough on its own, because a player who DELETES a
    /// shipped mod would get it back on the next launch. So the record is of what
    /// has ever been seeded, one relative path a line, rather than a single marker
    /// saying seeding has happened: a deleted mod stays deleted because its path is
    /// in the record, and a mod added by a later release is still seeded because
    /// its path is not. A bare marker gets the first of those right and the second
    /// wrong -- silently, since nothing reports a mod that never arrived.
    /// </summary>
    public static void Prepare()
    {
        Directory.CreateDirectory(Data);
        Directory.CreateDirectory(Builds);
        Directory.SetCurrentDirectory(Data);

        if (!Directory.Exists(ContentMods)) return;

        var record = Path.Combine(Data, ".mods-seeded");
        var already = File.Exists(record)
            ? new HashSet<string>(File.ReadAllLines(record), StringComparer.Ordinal)
            : new HashSet<string>(StringComparer.Ordinal);

        var added = new List<string>();
        foreach (var src in Directory.EnumerateFiles(ContentMods, "*", SearchOption.AllDirectories))
        {
            // A mod an older release shipped and this one dropped is not seeded.
            if (!Payload.Shipped(src)) continue;

            var rel = Path.GetRelativePath(ContentMods, src).Replace('\\', '/');
            if (already.Contains(rel)) continue;

            added.Add(rel);

            var dst = Path.Combine(Data, "mods", rel);
            if (File.Exists(dst)) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
            File.Copy(src, dst);
        }

        if (added.Count > 0) File.AppendAllLines(record, added);
    }
}
