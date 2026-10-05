namespace Verdite.Core;

/// The game this build is: the one thing core knows about it, set first thing in
/// Program.cs. The tag gives the log prefix (<c>[KF2]</c>), the env prefix
/// (<c>KF2_</c>) and the lowercase id for panel names (<c>kf2mouseind</c>).
public static class Game
{
    public static string Tag { get; private set; } = "VERDITE";
    public static string Id => Tag.ToLowerInvariant();
    public static string EnvPrefix => Tag + "_";

    public static void Configure(string tag) => Tag = tag;

    /// <summary>The game's env var <c>{Tag}_{name}</c>.</summary>
    public static string? Env(string name) => Environment.GetEnvironmentVariable(EnvPrefix + name);
}
