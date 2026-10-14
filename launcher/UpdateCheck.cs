using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.Json.Nodes;
using RecompOne.Runtime.Config;
using Verdite.Launcher.Build;

namespace Verdite.Launcher;

/// <summary>
/// Asks GitHub whether a newer release exists. Notify only: nothing is downloaded.
///
/// Runs once per launch on a worker thread, and reaches the network at most once an
/// hour (a day kept a published release from a player who had launched that morning
/// until the next); between checks the last answer is reused from update.json in the data
/// directory, so a player who launches twice still sees the notice. See
/// "Telling the player about a new release" in Verdite Core's README.
/// </summary>
static class UpdateCheck
{
    static string Api => $"https://api.github.com/repos/{Launcher.Game.UpdateRepository}/releases/latest";

    /// <summary>The View config key behind the Interface checkbox.</summary>
    public static string SettingKey => Launcher.Game.Name + ".UpdateCheck";

    static readonly TimeSpan Interval = TimeSpan.FromHours(1);

    public sealed record Release(string Tag, string Url);

    /// <summary>A newer release the player has not skipped; null until the check finishes, or if there is none.</summary>
    public static volatile Release? Available;

    static string StatePath => Path.Combine(Paths.Data, "update.json");

    public static bool Enabled
    {
        get => ConfigManager.View.GetBool(SettingKey, true);
        set => ConfigManager.View.SetBool(SettingKey, value);
    }

    /// <summary>
    /// {prefix}UPDATE_CHECK=0 (VERDITE2_UPDATE_CHECK) turns it off, =force ignores
    /// the hourly throttle (a skipped version stays skipped).
    /// </summary>
    public static void Start()
    {
        var env = Launcher.Game.Env("UPDATE_CHECK");
        if (env == "0") return;
        var force = string.Equals(env, "force", StringComparison.OrdinalIgnoreCase);
        if (!force && !Enabled) return;

        new Thread(() => Run(force)) { IsBackground = true, Name = Launcher.Game.AppId + "-update" }.Start();
    }

    static void Run(bool force)
    {
        try
        {
            var state = Locked(LoadState);
            var age = DateTime.UtcNow - ReadTime(state, "checked");

            // A time in the future is a clock that was wrong; check again.
            if (force || age is null || age < TimeSpan.Zero || age > Interval)
            {
                // Outside the lock: the request can take ten seconds.
                if (TryFetch() is var (tag, url))
                {
                    state = Locked(() =>
                    {
                        var s = LoadState();
                        s["checked"] = DateTime.UtcNow;
                        s["tag"] = tag;
                        s["url"] = url;
                        SaveState(s);
                        return s;
                    });
                }
            }

            var latest = ReadText(state, "tag");
            var page = ReadText(state, "url");
            if (latest is null || page is null) return;
            if (ReadText(state, "skipped") == latest)
            {
                Console.WriteLine($"{Launcher.Tag} {latest} is available and was skipped");
                return;
            }

            if (IsNewer(latest, Ver.Number))
            {
                Console.WriteLine($"{Launcher.Tag} update available: {latest} (running {Ver.Number})");
                Available = new Release(latest, page);
            }
        }
        catch (Exception e)
        {
            Console.WriteLine($"{Launcher.Tag} update check failed: {e.Message}");
        }
    }

    /// <summary>Null when offline, rate-limited or GitHub is down; the cached answer then stands.</summary>
    static (string Tag, string Url)? TryFetch()
    {
        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
            http.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue(Launcher.Game.Name, Ver.Number));
            http.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));

            // /releases/latest skips drafts and prereleases, so a draft release.yml has
            // opened is not announced until it is published.
            var json = JsonNode.Parse(http.GetStringAsync(Api).GetAwaiter().GetResult())
                       ?? throw new InvalidDataException("empty response");
            var tag = ReadText(json, "tag_name") ?? throw new InvalidDataException("no tag_name");
            var url = ReadText(json, "html_url") ?? throw new InvalidDataException("no html_url");
            return (tag, url);
        }
        catch (Exception e)
        {
            Console.WriteLine($"{Launcher.Tag} update check failed: {e.Message}");
            return null;
        }
    }

    /// <summary>
    /// Compares MAJOR.MINOR.PATCH; a suffix after the numbers (v0.4.0-hotfix) is
    /// ignored, and a tag with no number to read is reported rather than dropped.
    /// </summary>
    public static bool IsNewer(string tag, string current)
    {
        if (Numeric(tag) is not { } a)
        {
            Console.WriteLine($"{Launcher.Tag} update check: cannot read a version in the tag {tag}");
            return false;
        }

        return Numeric(current) is { } b && a > b;
    }

    static Version? Numeric(string s)
    {
        s = s.TrimStart('v', 'V');
        var end = 0;
        while (end < s.Length && (char.IsAsciiDigit(s[end]) || s[end] == '.')) end++;
        return Version.TryParse(s[..end].TrimEnd('.'), out var v) ? v : null;
    }

    /// <summary>
    /// Stop announcing this release; a later one is announced again. On a worker,
    /// since it is called from the interface.
    /// </summary>
    public static void Skip(string tag)
    {
        ThreadPool.QueueUserWorkItem(_ =>
        {
            try
            {
                Locked(() =>
                {
                    var state = LoadState();
                    state["skipped"] = tag;
                    SaveState(state);
                    return state;
                });
                Console.WriteLine($"{Launcher.Tag} skipped {tag}");
            }
            catch (Exception e)
            {
                Console.WriteLine($"{Launcher.Tag} could not record the skipped version: {e.Message}");
            }
        });
    }

    // The value, or null when it is missing or of another type: update.json is a
    // cache anyone can edit, and GetValue throws on a mismatch.
    static DateTime? ReadTime(JsonNode node, string key) =>
        node[key] is JsonValue v && v.TryGetValue(out DateTime x) ? x : null;

    static string? ReadText(JsonNode node, string key) =>
        node[key] is JsonValue v && v.TryGetValue(out string? x) ? x : null;

    static readonly object _stateGate = new();

    /// <summary>A whole read-modify-write of update.json, so Run and Skip cannot lose each other's field.</summary>
    static JsonObject Locked(Func<JsonObject> body)
    {
        lock (_stateGate) return body();
    }

    static JsonObject LoadState()
    {
        try
        {
            if (File.Exists(StatePath) && JsonNode.Parse(File.ReadAllText(StatePath)) is JsonObject o) return o;
        }
        catch (JsonException)
        {
            // A damaged file is only a cache; start again.
        }

        return new JsonObject();
    }

    static void SaveState(JsonObject state) =>
        File.WriteAllText(StatePath, state.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
}
