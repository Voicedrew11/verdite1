using System.Security.Cryptography;
using System.Text;
using RecompOne.Runtime.Cdrom;

namespace Verdite2.Launcher.Build;

/// <summary>
/// What identifies one built game assembly, so a second launch can skip the build
/// and a changed input cannot be served a stale one.
///
/// Three things go in, and each answers a way the cached assembly could be wrong:
///
///   - The disc's own code. Not the file's hash: an image can differ in padding,
///     track layout or the 180 MB of streamed media and still produce byte-identical
///     output. What the recompiler reads is the three executables (King's Field
///     has no code modules), so those are what is hashed -- two dumps that
///     recompile the same get one cache entry between them.
///
///   - The shipped sources. content/src is compiled into the assembly, so an
///     updated port must rebuild. Hashing the text catches that without asking
///     anyone to remember to bump anything.
///
///   - The shipped config. content/config is the OTHER half of what goes into the
///     build: the function maps decide where every function starts and the SDK
///     address map decides which of them are bound to the runtime's HLE, so a
///     corrected sweep or a newly mapped library entry point changes the emitted
///     C# with no source file having moved. Hashed for the same reason and in the
///     same way, so a config-only change cannot be served a stale assembly.
///
///   - The launcher's version, which covers a change in how the build is done
///     rather than in what goes into it.
///
/// The absolute LBAs are deliberately NOT part of the key even though they are
/// baked into the output (Dispatcher arms an overlay swap on a CD read hitting an
/// exact sector), because they are read from the disc during the recompile itself
/// -- so a differently mastered dump of the same game produces a different
/// executable payload only if the code differs, and gets its own correct LBAs
/// either way. The recompile is under a second; the cache exists for the compile.
/// </summary>
static class BuildKey
{
    public static string Compute(string discPath)
    {
        var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);

        Add(hash, "verdite1");
        Add(hash, typeof(BuildKey).Assembly.GetName().Version?.ToString() ?? "0");

        using (var fs = DiscFs.Open(discPath))
            foreach (var file in new[] { "PSX.EXE", "OPEN.EXE", "GAME.EXE" })
            {
                Add(hash, file);
                try { hash.AppendData(SHA256.HashData(fs.ReadFile(file))); }
                catch { Add(hash, "<missing>"); }
            }

        foreach (var src in Sources.All().Concat(Sources.Config()))
        {
            Add(hash, Path.GetRelativePath(Paths.Content, src).Replace('\\', '/'));
            hash.AppendData(SHA256.HashData(File.ReadAllBytes(src)));
        }

        return Convert.ToHexString(hash.GetHashAndReset())[..16].ToLowerInvariant();
    }

    /// <summary>
    /// Length-prefixed, so "ab" + "c" and "a" + "bc" cannot hash the same. Cheap
    /// here and the sort of thing that is impossible to notice once it is wrong.
    /// </summary>
    static void Add(IncrementalHash hash, string s)
    {
        var bytes = Encoding.UTF8.GetBytes(s);
        hash.AppendData(BitConverter.GetBytes(bytes.Length));
        hash.AppendData(bytes);
    }
}

/// <summary>The shipped payload the game assembly is built from, in a stable order.</summary>
static class Sources
{
    /// <summary>The port's own C#, compiled into the assembly.</summary>
    public static IEnumerable<string> All() => In(Paths.ContentSrc, "*.cs");

    /// <summary>The recompiler's inputs: kf1.json and the function maps under it.</summary>
    public static IEnumerable<string> Config() => In(Paths.ContentConfig, "*.json");

    static IEnumerable<string> In(string dir, string pattern)
    {
        if (!Directory.Exists(dir)) yield break;

        var files = Directory.GetFiles(dir, pattern, SearchOption.AllDirectories);
        // Ordinal, so the key does not move with the host's locale.
        Array.Sort(files, StringComparer.Ordinal);
        foreach (var f in files) yield return f;
    }
}
