using System.Reflection;

namespace Verdite.Launcher.Build;

/// <summary>
/// Drives the recompiler over the player's disc: MIPS out of the image, C# into a
/// directory. About a second for a whole game, so it is not cached and is simply
/// re-run whenever a build is needed.
///
/// It runs IN PROCESS. RecompOne.Recompiler is an Exe with top-level statements,
/// which the compiler emits as Program.&lt;Main&gt;$(string[]) -- an ordinary method,
/// reachable through Assembly.EntryPoint. That matters because the alternative is
/// launching a second process, and a self-contained publish has no `dotnet` on the
/// player's machine to launch it with; shipping a second apphost to do it would
/// double the runtime in the package for no gain.
/// </summary>
static class Recompile
{
    /// <summary>
    /// Recompile <paramref name="discPath"/> into <paramref name="outDir"/>.
    /// Throws with the recompiler's own message on failure.
    /// </summary>
    public static void Run(string discPath, string outDir)
    {
        Directory.CreateDirectory(outDir);

        // The recompiler resolves cue, funcMap and output relative to the config
        // FILE's directory. The edited config therefore cannot live beside the
        // shipped one: an installed build's content/ is read-only -- Program Files,
        // or an AppImage's own squashfs mount, which is read-only even for root.
        //
        // So the whole config directory is staged into the data directory, which
        // keeps every relative funcMap path in the config resolving exactly as it
        // does in the repository. It is a few hundred KB and only copied when it
        // has changed.
        var staged = Stage();

        var config = Launcher.Game.RecompilerConfig;
        var cfgPath = Path.Combine(staged, Path.GetFileNameWithoutExtension(config) + ".build.json");
        File.WriteAllText(cfgPath, Rewrite(File.ReadAllText(Path.Combine(staged, config)), discPath, outDir));

        try { Invoke(cfgPath); }
        finally { try { File.Delete(cfgPath); } catch { } }
    }

    /// <summary>Mirror content/config into the data directory, skipping files already current.</summary>
    static string Stage()
    {
        var dst = Path.Combine(Paths.Data, "config");
        Directory.CreateDirectory(dst);

        foreach (var src in Directory.EnumerateFiles(Paths.ContentConfig, "*.json", SearchOption.AllDirectories))
        {
            var target = Path.Combine(dst, Path.GetRelativePath(Paths.ContentConfig, src));
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);

            // File.Copy carries the source's mtime across, so an unchanged payload
            // file compares equal here and is skipped. The test is equality rather
            // than "the copy is no older", because the staged copy being NEWER is
            // not evidence that it is current: an install rolled back, or two
            // builds sharing one data directory, hands us a payload whose files are
            // older than what is staged, and a config staged from the wrong build
            // is a recompile against the wrong addresses.
            var from = new FileInfo(src);
            var to = new FileInfo(target);
            if (to.Exists && to.Length == from.Length && to.LastWriteTimeUtc == from.LastWriteTimeUtc) continue;

            File.Copy(src, target, overwrite: true);
        }

        return dst;
    }

    /// <summary>
    /// Point "cue" at the player's image and "output" at our build directory,
    /// leaving every address, overlay and SDK patch in the shipped config alone.
    /// The key is still called "cue" because that is the recompiler's schema;
    /// DiscFs.Open dispatches on the extension, so a .chd goes in the same slot.
    ///
    /// Done as a string edit rather than by parsing and re-emitting, because the
    /// config carries comments and trailing commas -- the recompiler's loader
    /// accepts both and System.Text.Json will not write them back. Losing the
    /// comments would not break the build, but it would silently turn the one
    /// documented copy of the overlay layout into a machine-written blob the next
    /// time anyone looked at it.
    /// </summary>
    static string Rewrite(string json, string discPath, string outDir)
    {
        json = ReplaceStringValue(json, "cue", discPath);
        json = ReplaceStringValue(json, "output", outDir);
        return json;
    }

    static string ReplaceStringValue(string json, string key, string value)
    {
        var name = $"content/config/{Launcher.Game.RecompilerConfig}";
        var needle = $"\"{key}\"";
        int at = json.IndexOf(needle, StringComparison.Ordinal);
        if (at < 0) throw new InvalidOperationException($"{name} has no \"{key}\" entry.");

        int colon = json.IndexOf(':', at + needle.Length);
        if (colon < 0) throw new InvalidOperationException($"{name}: \"{key}\" has no value.");

        int open = json.IndexOf('"', colon + 1);
        if (open < 0) throw new InvalidOperationException($"{name}: \"{key}\" is not a string.");

        int close = json.IndexOf('"', open + 1);
        if (close < 0) throw new InvalidOperationException($"{name}: \"{key}\" is unterminated.");

        return json[..(open + 1)] + System.Text.Json.JsonEncodedText.Encode(value) + json[close..];
    }

    static void Invoke(string cfgPath)
    {
        var asm = typeof(RecompOne.Recompiler.Config.ConfigLoader).Assembly;
        var main = asm.EntryPoint
            ?? throw new InvalidOperationException("The recompiler assembly has no entry point.");

        object? result;
        try
        {
            result = main.Invoke(null, [new[] { cfgPath }]);
        }
        catch (TargetInvocationException e) when (e.InnerException is not null)
        {
            throw new InvalidOperationException($"The recompiler failed: {e.InnerException.Message}", e.InnerException);
        }

        // Its Main returns an exit code rather than throwing on a bad config or a
        // missing disc, and both of those are reachable from here.
        if (result is int code && code != 0)
            throw new InvalidOperationException(
                $"The recompiler exited with code {code}. See the log for what it reported.");
    }
}
