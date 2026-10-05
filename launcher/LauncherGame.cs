namespace Verdite.Launcher;

/// <summary>
/// Everything the shared launcher has to be told about the game it ships, handed
/// to <see cref="Launcher.Run"/> by the game's own launcher project. Nothing else
/// in launcher/ may name a game: the disc's files and their floors come from the
/// game's recompiler config, and everything below comes from here.
/// </summary>
public sealed record LauncherGame
{
    /// <summary>
    /// The port's name, as a player sees it: the window title, the console tag
    /// (<c>[Verdite2]</c>), the Windows data folder, the env prefix
    /// (<c>VERDITE2_</c>), the View key of the update setting.
    /// </summary>
    public required string Name { get; init; }

    /// <summary>
    /// The lowercase id: the Wayland app id (and so the desktop entry and the icon
    /// the compositor looks up), the XDG data folder, the shipped icon's file name.
    /// </summary>
    public required string AppId { get; init; }

    /// <summary>The game's title for a player, in the build popup: "King's Field".</summary>
    public required string GameTitle { get; init; }

    /// <summary>The disc serial this port is of, <c>SLUS-00158</c>. Its boot file is derived from it.</summary>
    public required string Serial { get; init; }

    /// <summary>
    /// Discs a player is likely to reach for instead, each with what to tell them:
    /// a sibling game renumbered for the West, say. Keyed by serial.
    /// </summary>
    public IReadOnlyDictionary<string, string> WrongDiscs { get; init; } = new Dictionary<string, string>();

    /// <summary>The recompiler config in content/config, <c>kf2.json</c>.</summary>
    public required string RecompilerConfig { get; init; }

    /// <summary>The game assembly, as its csproj names it: <c>KingsField2</c>.</summary>
    public required string GameAssembly { get; init; }

    /// <summary>The GitHub repository releases are announced from, <c>owner/name</c>.</summary>
    public required string UpdateRepository { get; init; }

    /// <summary>
    /// Whether an overlay that has just loaded starts play (true), returns to the
    /// title (false) or neither (null). The update popup is modal, so it waits out
    /// play and shows at the next title; the badge carries the news meanwhile.
    /// </summary>
    public required Func<string, bool?> PlayAfter { get; init; }

    /// <summary><c>VERDITE2_</c>: the launcher's own switches, beside the game's <c>KF2_</c>.</summary>
    public string EnvPrefix => Name.ToUpperInvariant() + "_";

    public string? Env(string name) => Environment.GetEnvironmentVariable(EnvPrefix + name);

    /// <summary><c>SLUS-00158</c> is booted as <c>SLUS_001.58</c>.</summary>
    public string BootFile => Serial.Length == 10 && Serial[4] == '-'
        ? $"{Serial[..4]}_{Serial[5..8]}.{Serial[8..]}"
        : Serial;

    /// <summary>
    /// The launcher's strings with this game's names in them: <c>%NAME%</c> is
    /// <see cref="Name"/> and <c>%GAME%</c> is <see cref="GameTitle"/>.
    /// </summary>
    public string Fill(string text) => text.Replace("%NAME%", Name).Replace("%GAME%", GameTitle);
}
