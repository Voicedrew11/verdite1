using System.Reflection;

namespace Verdite.Launcher;

/// <summary>
/// What this build calls itself.
///
/// Two numbers, because they answer different questions. <see cref="Number"/> is
/// the release -- 0.1.0, the thing a tag names (or the game's VERSION) and a
/// player says they are running. <see cref="Full"/> adds the commit it was
/// published from, because a release is many commits wide and the difference
/// between them is exactly what a bug report cannot otherwise supply.
///
/// Both are read back off the assembly rather than duplicated as a literal, so
/// there is one rule for the number (Launcher.targets, and scripts/version.sh for
/// the scripts) and one mechanism that carries it (the StampBuild target).
/// </summary>
static class Ver
{
    /// <summary>The release number: MAJOR.MINOR.PATCH, matching the git tag without its v.</summary>
    public static string Number { get; } =
        typeof(Ver).Assembly.GetName().Version is { } v ? $"{v.Major}.{v.Minor}.{v.Build}" : "0.0.0";

    /// <summary>The release number plus the build it came from, e.g. 0.1.0+048ef32b3.</summary>
    public static string Full { get; } =
        typeof(Ver).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
            is { Length: > 0 } s ? s : Number;
}
