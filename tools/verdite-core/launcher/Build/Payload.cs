namespace Verdite.Launcher.Build;

/// <summary>
/// The files this release shipped under content/, as Launcher.targets listed them
/// in content/manifest.txt when it was built.
///
/// The launcher must not take content/ to be whatever is in that directory. A
/// player who unpacks a new release over an old one (the zip, or a folder copied
/// onto itself) keeps every file the new release deleted, and the build used to
/// compile every .cs it found: Verdite3 0.1.1 unpacked over 0.1.0 kept the old
/// patches/SettingsSession.cs, PortSetting.cs and MenuFont.cs, which 0.1.1 had
/// moved into Verdite Core, and the game failed to compile with seven errors
/// (CS1501, CS1503, CS0407) that looked like a broken release. Reading the
/// manifest makes a stale file inert: it is not compiled, not hashed into the
/// build key and not seeded as a mod.
///
/// An install with no manifest (one older than this, or a damaged one) falls back
/// to the directory's contents, which is what every build did before.
/// </summary>
static class Payload
{
    public static string ManifestPath { get; } = Path.Combine(Paths.Content, "manifest.txt");

    /// <summary>Paths relative to content/, '/'-separated; null with no manifest.</summary>
    static readonly HashSet<string>? Listed = Read();

    /// <summary>
    /// The manifest holds each item's Link as MSBuild staged it: relative to the
    /// install, so "content/" first, in the build host's separators, and with the
    /// few payload files outside content/ (LICENSE) among them.
    /// </summary>
    static HashSet<string>? Read()
    {
        if (!File.Exists(ManifestPath)) return null;

        return File.ReadAllLines(ManifestPath)
            .Select(l => l.Trim().Replace('\\', '/'))
            .Where(l => l.StartsWith("content/", StringComparison.Ordinal))
            .Select(l => l["content/".Length..])
            .ToHashSet(StringComparer.Ordinal);
    }

    /// <summary>
    /// The shipped files under <paramref name="dir"/> (a directory inside content/)
    /// matching <paramref name="pattern"/>, in ordinal order so the build key does
    /// not move with the host's locale.
    /// </summary>
    public static IEnumerable<string> In(string dir, string pattern)
    {
        if (!Directory.Exists(dir)) return [];

        var files = Directory.GetFiles(dir, pattern, SearchOption.AllDirectories)
            .Where(Shipped)
            .ToArray();
        Array.Sort(files, StringComparer.Ordinal);
        return files;
    }

    /// <summary>Whether this release shipped <paramref name="path"/>; true for every file with no manifest.</summary>
    public static bool Shipped(string path) =>
        Listed is null || Listed.Contains(Path.GetRelativePath(Paths.Content, path).Replace('\\', '/'));

    /// <summary>
    /// Say once, at startup, what is being ignored and why, so a build log from an
    /// over-unpacked install shows it instead of leaving it to be guessed.
    /// </summary>
    public static void Report()
    {
        if (Listed is null)
        {
            Console.WriteLine($"{Launcher.Tag} no {ManifestPath}; building from everything under content/");
            return;
        }

        if (!Directory.Exists(Paths.Content)) return;

        var stale = Directory.GetFiles(Paths.Content, "*", SearchOption.AllDirectories)
            .Where(f => f != ManifestPath && !Shipped(f))
            .Select(f => Path.GetRelativePath(Paths.Content, f).Replace('\\', '/'))
            .Order(StringComparer.Ordinal)
            .ToList();

        if (stale.Count == 0) return;

        Console.WriteLine($"{Launcher.Tag} ignoring {stale.Count} file(s) under content/ this release did not ship (left by an older one):");
        foreach (var f in stale) Console.WriteLine($"{Launcher.Tag}   {f}");
    }
}
