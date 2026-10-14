using System.Globalization;
using System.Text.Json;
using RecompOne.Runtime.Config;
using RecompOne.Runtime.Host;
using RecompOne.Runtime.Host.Window;
using Rt = RecompOne.Runtime.Runtime;

namespace Verdite.Core;

/// <summary>
/// The first-run setup: a display step, then one of three tiers (Original,
/// Original++, Modern) for each group of a game's settings, a page per category. Nothing here knows a
/// game: the groups, the keys each tier writes and every word the player reads
/// come from the game's <c>config/onboarding.json</c> (<see cref="OnboardingSpec"/>).
/// See "Onboarding" in docs/SETTINGS.md in a port.
///
/// Compiled twice, by design: into the launcher, which shows it while the game is
/// built, and into the game, whose settings page runs it again. The two copies are
/// separate types in separate assemblies, so everything is internal and nothing
/// is shared between them but the file on disk and interface.ini.
///
/// The rules are the settings page's: a value equal to its default removes the key
/// rather than writing it, so a player who chose the default follows it if a
/// release changes it, and only keys whose value changes are touched.
/// </summary>
internal static class Onboarding
{
    /// <summary>The spec's version the player last finished (or skipped) setup at.
    /// Below the spec's own, setup is due, which is also what shows it once to a
    /// player whose settings predate it.</summary>
    public const string FlagKey = "verdite.setup";

    /// <summary>The runtime's window keys, applied here rather than by the game.</summary>
    static readonly string[] WindowKeys = ["Fullscreen", "Borderless"];

    /// <summary>
    /// The game's: apply one key live, given what it now means (its text, else the
    /// spec's default for it; null only for a key the spec gives no default). False
    /// when the key takes effect only at the next launch. Unset in the launcher,
    /// where nothing has started yet and every key is read at boot.
    /// </summary>
    public static Func<string, string?, bool>? LiveApply { get; set; }

    static readonly JsonSerializerOptions Json = new()
    {
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
        // Into the dictionaries as declared, so they keep their ignore-case comparer
        // (interface.ini's keys are case-insensitive) instead of a new default one.
        PreferredObjectCreationHandling = System.Text.Json.Serialization.JsonObjectCreationHandling.Populate,
    };

    /// <summary>The first of <paramref name="paths"/> that exists, parsed, its strings
    /// merged into the runtime's; null (and a line saying why) when none does or it
    /// will not parse, in which case there is no setup to show.</summary>
    public static OnboardingSpec? Load(string tag, params string[] paths)
    {
        var path = paths.FirstOrDefault(File.Exists);
        if (path is null)
        {
            Console.WriteLine($"[{tag}] setup: no onboarding.json (looked in {string.Join(", ", paths)})");
            return null;
        }
        try
        {
            var text = File.ReadAllText(path);
            var spec = JsonSerializer.Deserialize<OnboardingSpec>(text, Json)
                       ?? throw new InvalidDataException("empty");
            if (spec.Strings is { } strings)
                Localization.Merge($"{{\"strings\":{strings.GetRawText()}}}");
            Localization.Merge(OnboardingPopup.Strings);
            foreach (var problem in spec.Check())
                Console.WriteLine($"[{tag}] setup: {path}: {problem}");
            return spec;
        }
        catch (Exception e)
        {
            Console.WriteLine($"[{tag}] setup: {path} could not be read: {e.Message}");
            return null;
        }
    }

    /// <summary>The places a game's config directory is: the working directory (a
    /// developer run, from the repository), then the shipped <c>content/config</c>
    /// beside the executable or, on the Windows package, one directory up.</summary>
    public static string[] Candidates(string file)
    {
        var dir = Path.GetFullPath(AppContext.BaseDirectory)
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var list = new List<string> { Path.Combine("config", file), Path.Combine(dir, "content", "config", file) };
        if (Path.GetDirectoryName(dir) is { } parent) list.Add(Path.Combine(parent, "content", "config", file));
        return [.. list];
    }

    /// <summary>Whether a key is one of the runtime's window keys, which setup applies itself.</summary>
    public static bool IsWindowKey(string key) => WindowKeys.Contains(key, StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// Whether setup is shown: the player has never seen it, or last finished it at
    /// an older spec. Having played (settings stored) does not excuse a player who
    /// has never seen it; it only decides where the display starts (<see cref="FirstRun"/>).
    /// </summary>
    public static bool Due(OnboardingSpec spec) => ConfigManager.View.GetInt(FlagKey, 0) < spec.Version;

    /// <summary>A player with no setup flag and no settings of the game's: the only
    /// case in which the display starts on what the monitor suggests rather than on
    /// what is set.</summary>
    public static bool FirstRun(OnboardingSpec spec) => ConfigManager.View.GetInt(FlagKey, 0) == 0 && !Played(spec);

    /// <summary>Whether interface.ini holds any key under the spec's prefix.</summary>
    static bool Played(OnboardingSpec spec) =>
        spec.Existing is { Length: > 0 } prefix
        && ConfigManager.View.Values.Keys.Any(k => k.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));

    /// <summary>Setup was seen and declined: nothing is written but the flag.</summary>
    public static void Skip(OnboardingSpec spec, string tag)
    {
        ConfigManager.View.SetInt(FlagKey, spec.Version);
        Rt.SaveView();
        Console.WriteLine($"[{tag}] setup: skipped, nothing changed");
    }

    // ---- Reading what is set ----

    static string? Stored(string key) => ConfigManager.View.Values.TryGetValue(key, out var v) ? v : null;

    /// <summary>What a key means: its text, else its declared default, else null.</summary>
    static string? Effective(OnboardingSpec spec, string key, string? text) =>
        text ?? (spec.Defaults.TryGetValue(key, out var d) ? d : null);

    /// <summary>Two texts of one key, as values: numbers by value ("60" is "60.0"),
    /// anything else ignoring case ("True" is "true").</summary>
    public static bool SameText(string? a, string? b)
    {
        if (a is null || b is null) return a is null && b is null;
        if (double.TryParse(a, NumberStyles.Float, CultureInfo.InvariantCulture, out var x)
            && double.TryParse(b, NumberStyles.Float, CultureInfo.InvariantCulture, out var y))
            return Math.Abs(x - y) < 1e-3;
        return string.Equals(a, b, StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>Whether every key a set writes already means what it would write.</summary>
    public static bool Matches(OnboardingSpec spec, Dictionary<string, string?> set) =>
        set.All(kv => SameText(Effective(spec, kv.Key, Stored(kv.Key)), Effective(spec, kv.Key, Resolve(kv.Value))));

    /// <summary>The tier a group is at now, or -1 when its keys match none (set by hand).</summary>
    public static int Detect(OnboardingSpec spec, SetupGroup group)
    {
        for (int i = 0; i < group.Tiers.Count; i++)
            if (Matches(spec, group.Tiers[i].Set)) return i;
        return -1;
    }

    /// <summary>
    /// The tier each of a category's groups is at now. Where two tiers of a group
    /// write the same (a tier that adds nothing to one group), the group matches both,
    /// so a tier every group matches is taken for all of them, the later tier
    /// first; otherwise each group is at its own first match, or -1.
    /// </summary>
    public static int[] Detect(OnboardingSpec spec, SetupCategory category)
    {
        var matches = category.Groups
            .Select(g => Enumerable.Range(0, g.Tiers.Count).Where(t => Matches(spec, g.Tiers[t].Set)).ToList())
            .ToArray();
        for (int t = OnboardingPopup.TierCount - 1; t >= 0; t--)
            if (matches.All(m => m.Contains(t)))
                return [.. matches.Select(_ => t)];
        return [.. matches.Select(m => m.Count > 0 ? m[0] : -1)];
    }

    /// <summary>The option a display field is at now, or -1.</summary>
    public static int Detect(OnboardingSpec spec, SetupField field)
    {
        for (int i = 0; i < field.Options.Count; i++)
            if (Matches(spec, field.Options[i].Set)) return i;
        return field.Default is { } d && d >= 0 && d < field.Options.Count ? d : -1;
    }

    /// <summary>
    /// The option to start a first run on: the monitor's own aspect, the largest
    /// render scale its height holds, its refresh rate. A field with no suggestion,
    /// or one the monitor cannot answer, starts where it is.
    /// </summary>
    public static int Suggest(OnboardingSpec spec, SetupField field)
    {
        var options = field.Options;
        int Best(Func<SetupOption, double?> score)
        {
            int best = -1;
            double bestScore = double.MinValue;
            for (int i = 0; i < options.Count; i++)
                if (score(options[i]) is { } s && s > bestScore) { best = i; bestScore = s; }
            return best;
        }

        int pick = -1;
        var monitor = Monitor();
        switch (field.Suggest)
        {
            case "aspect" when monitor is { } m && m.H > 0:
                double aspect = m.W / (double)m.H;
                pick = Best(o => o.Match is { } r ? -Math.Abs(r - aspect) : null);
                break;
            case "height" when monitor is { } m:
                pick = Best(o => o.Match is { } h && h <= m.H ? h : null);
                break;
            case "refresh":
                // An option writing the display's own rate, else the nearest one.
                pick = options.FindIndex(o => o.Set.Values.Any(v => v == RefreshToken));
                if (pick < 0) pick = Best(o => o.Match is { } r ? -Math.Abs(r - Refresh()) : null);
                break;
        }
        return pick >= 0 ? pick : Detect(spec, field);
    }

    /// <summary>A key's number now: its stored value, else its declared default; null when neither parses.</summary>
    public static double? Level(OnboardingSpec spec, string key)
    {
        if (Effective(spec, key, Stored(key)) is { } text
            && double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var v))
            return v;
        return null;
    }

    /// <summary>
    /// The option a choice page is at now (or -1), and its slider's value now: the
    /// key's effective value, else its minimum.
    /// </summary>
    public static SetupPageChoice Detect(OnboardingSpec spec, SetupPage page)
    {
        int option = page.Options.FindIndex(o => Matches(spec, o.Set));
        double? value = page.Slider is { } s ? Level(spec, s.Key) ?? s.Min : null;
        return new SetupPageChoice { Option = option, Value = value };
    }

    /// <summary>
    /// The choices a preset makes: every group at <paramref name="tier"/> (0 Original,
    /// 1 Original++, 2 Modern). Tier 0 takes the Original option of each display field
    /// (or where it starts, when it has none); the other tiers take the Recommended
    /// display (<see cref="Suggest"/>). Each choice page takes its preset for the tier,
    /// or keeps the player's choice when it has none.
    /// </summary>
    public static (int[] Display, int[][] Tiers, SetupPageChoice[] Pages) Preset(OnboardingSpec spec, int tier)
    {
        var display = spec.Display.Select(f => tier == 0
            ? (f.Original is { } o && o >= 0 && o < f.Options.Count ? o : Detect(spec, f))
            : Suggest(spec, f)).ToArray();
        var tiers = spec.Categories
            .Select(c => c.Groups.Select(_ => tier).ToArray()).ToArray();
        var pages = spec.Pages
            .Select(p => tier < p.Presets.Count ? p.Presets[tier].Clone() : new SetupPageChoice()).ToArray();
        return (display, tiers, pages);
    }

    /// <summary>What a display field's first key means now: its text, else its default; null when its options write no key.</summary>
    public static string? Current(OnboardingSpec spec, SetupField field)
    {
        if (field.Options.Count == 0 || field.Options[0].Set.Keys.FirstOrDefault() is not { } key) return null;
        return Effective(spec, key, Stored(key));
    }

    // ---- The monitor ----

    const string RefreshToken = "$refresh";

    /// <summary>The display's refresh rate, as the runtime measured it; 60 when it could not.</summary>
    public static int Refresh() =>
        RecompOne.Runtime.Interp.Interp.RefreshRate > 0 ? RecompOne.Runtime.Interp.Interp.RefreshRate : 60;

    /// <summary>A value as it is written: <c>$refresh</c> is the display's rate.</summary>
    public static string? Resolve(string? text) =>
        text == RefreshToken ? Refresh().ToString(CultureInfo.InvariantCulture) : text;

    static (int W, int H)? _monitor;
    static bool _monitorRead;

    /// <summary>The primary monitor's current mode, or null when GLFW cannot say.</summary>
    public static unsafe (int W, int H)? Monitor()
    {
        if (_monitorRead) return _monitor;
        _monitorRead = true;
        try
        {
            var glfw = Silk.NET.GLFW.Glfw.GetApi();
            var monitor = glfw.GetPrimaryMonitor();
            if (monitor == null) return null;
            var mode = glfw.GetVideoMode(monitor);
            if (mode == null) return null;
            _monitor = (mode->Width, mode->Height);
        }
        catch { /* headless, or no GLFW: no suggestion */ }
        return _monitor;
    }

    // ---- Writing ----

    /// <summary>The sets a choice writes: the option chosen for each display field and
    /// the tier for each group, by index; -1 leaves a field or group as it is. Then, per
    /// choice page, its chosen option's set and its slider's value (when set).</summary>
    public static List<Dictionary<string, string?>> Sets(OnboardingSpec spec, int[] display, int[][] tiers, SetupPageChoice[]? pages = null)
    {
        var sets = new List<Dictionary<string, string?>>();
        for (int i = 0; i < spec.Display.Count && i < display.Length; i++)
            if (display[i] >= 0 && display[i] < spec.Display[i].Options.Count) sets.Add(spec.Display[i].Options[display[i]].Set);
        for (int c = 0; c < spec.Categories.Count && c < tiers.Length; c++)
            for (int g = 0; g < spec.Categories[c].Groups.Count && g < tiers[c].Length; g++)
                if (tiers[c][g] is var t && t >= 0 && t < spec.Categories[c].Groups[g].Tiers.Count)
                    sets.Add(spec.Categories[c].Groups[g].Tiers[t].Set);
        for (int p = 0; pages is not null && p < spec.Pages.Count && p < pages.Length; p++)
        {
            var page = spec.Pages[p];
            var choice = pages[p];
            if (choice.Option >= 0 && choice.Option < page.Options.Count) sets.Add(page.Options[choice.Option].Set);
            if (choice.Value is { } v && page.Slider is { } slider)
                sets.Add(new() { [slider.Key] = v.ToString("0.##", CultureInfo.InvariantCulture) });
        }
        return sets;
    }

    /// <summary>What <see cref="Apply"/> did: the keys whose meaning changed, and of
    /// those the ones that wait for the next launch.</summary>
    public sealed record Result(IReadOnlyList<string> Changed, IReadOnlyList<string> NextLaunch);

    /// <summary>
    /// Write <paramref name="sets"/>, later ones over earlier, and mark setup done.
    /// On the host thread: interface.ini is the host's. A key whose value is its
    /// default is removed; a key already meaning the value is left alone. The
    /// window's keys are applied here; every other changed key goes to
    /// <see cref="LiveApply"/> when the game has set it.
    /// </summary>
    public static Result Apply(OnboardingSpec spec, IEnumerable<Dictionary<string, string?>> sets, string tag)
    {
        var want = new Dictionary<string, string?>(StringComparer.OrdinalIgnoreCase);
        foreach (var set in sets)
            foreach (var (key, value) in set)
                want[key] = Resolve(value);

        var values = ConfigManager.View.Values;
        var changed = new List<string>();
        foreach (var (key, value) in want)
        {
            var text = value is not null && spec.Defaults.TryGetValue(key, out var d) && SameText(value, d) ? null : value;
            var old = Stored(key);
            bool differs = !SameText(Effective(spec, key, old), Effective(spec, key, text));

            if (text is null) values.Remove(key);
            else if (!SameText(old, text)) values[key] = text;
            else continue;

            if (!differs) continue;
            changed.Add(key);
            Console.WriteLine(text is null
                ? $"[{tag}] setup: {key} back to its default"
                : $"[{tag}] setup: {key}={text}");
        }

        ConfigManager.View.SetInt(FlagKey, spec.Version);
        Rt.SaveView();

        if (changed.Any(k => WindowKeys.Contains(k, StringComparer.OrdinalIgnoreCase)))
            HostWindow.SetFullscreen(ConfigManager.View.Fullscreen);

        var later = new List<string>();
        if (LiveApply is { } live)
            foreach (var key in changed)
            {
                if (WindowKeys.Contains(key, StringComparer.OrdinalIgnoreCase)) continue;
                bool applied;
                try { applied = live(key, Effective(spec, key, Stored(key))); }
                catch (Exception e)
                {
                    Console.WriteLine($"[{tag}] setup: {key} could not be applied now: {e.Message}");
                    applied = false;
                }
                if (!applied) later.Add(key);
            }

        Console.WriteLine($"[{tag}] setup: done, {changed.Count} setting(s) changed" +
                          (later.Count > 0 ? $", {later.Count} at the next launch" : ""));
        return new Result(changed, later);
    }
}

/// <summary>
/// A game's <c>config/onboarding.json</c>. Labels and descriptions are localisation
/// keys, merged from <see cref="Strings"/> (the runtime's own format, every key in
/// en, pt-BR and es-419); a label that is not a key ("60", "4:3") shows as itself.
/// </summary>
internal sealed class OnboardingSpec
{
    /// <summary>Raised when a release changes setup enough to ask everyone again.</summary>
    public int Version { get; set; } = 1;

    /// <summary>What each key means when it is absent: the feature's own fallback.
    /// A value equal to it is written as no key at all.</summary>
    public Dictionary<string, string> Defaults { get; set; } = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>A key prefix the game writes on its first boot (<c>kf2.</c>): a player
    /// with any stored key under it already has settings, and is not asked. Absent,
    /// every player without the flag is asked once.</summary>
    public string? Existing { get; set; }

    /// <summary>The welcome's text, a localisation key; the setup's own when absent.</summary>
    public string? Welcome { get; set; }

    /// <summary>The welcome's text when the game runs setup again (<c>welcomeAgain</c> in the JSON,
    /// read case-insensitively); the setup's own re-run text when absent.</summary>
    public string? WelcomeAgain { get; set; }

    /// <summary>The display step, asked before the tiers and outside them.</summary>
    public List<SetupField> Display { get; set; } = [];

    public List<SetupCategory> Categories { get; set; } = [];

    /// <summary>Choice pages after the tiers and before the summary: a game's own
    /// choices (an option and a slider each), with presets per tier.</summary>
    public List<SetupPage> Pages { get; set; } = [];

    public JsonElement? Strings { get; set; }

    /// <summary>What a loaded spec gets wrong, for the log: tiers of a group writing
    /// different keys (so detecting the tier depends on which is tried first).</summary>
    public IEnumerable<string> Check()
    {
        foreach (var f in Display)
            if (f.Original is { } o && (o < 0 || o >= f.Options.Count))
                yield return $"display.{f.Id}: original {o} is not an option";

        foreach (var c in Categories)
        {
            foreach (var g in c.Groups)
            {
                if (g.Tiers.Count != OnboardingPopup.TierCount)
                    yield return $"{c.Id}.{g.Id}: {g.Tiers.Count} tiers, not {OnboardingPopup.TierCount}";
                var keys = g.Tiers.Select(t => string.Join(",", t.Set.Keys.Order(StringComparer.OrdinalIgnoreCase))).Distinct().Count();
                if (keys > 1) yield return $"{c.Id}.{g.Id}: its tiers write different keys";
                // A key every tier writes alike is not the group's choice to make: null
                // in all of them would put a player's own value back to the default
                // whichever tier they chose, and read the group as Custom until then.
                if (g.Tiers.Count > 1)
                    foreach (var key in g.Tiers[0].Set.Keys)
                        if (g.Tiers.All(t => t.Set.TryGetValue(key, out var v) && Onboarding.SameText(v, g.Tiers[0].Set[key])))
                            yield return $"{c.Id}.{g.Id}: every tier writes {key} alike";
            }
        }

        foreach (var p in Pages)
        {
            if (p.Presets.Count != OnboardingPopup.TierCount)
                yield return $"page {p.Id}: {p.Presets.Count} presets, not {OnboardingPopup.TierCount}";
            var keys = p.Options.Select(o => string.Join(",", o.Set.Keys.Order(StringComparer.OrdinalIgnoreCase))).Distinct().Count();
            if (keys > 1) yield return $"page {p.Id}: its options write different keys";
            if (p.Slider is { } s && p.Options.Any(o => o.Set.ContainsKey(s.Key)))
                yield return $"page {p.Id}: an option writes the slider's key {s.Key}";
        }
    }
}

internal sealed class SetupField
{
    public string Id { get; set; } = "";
    public string Label { get; set; } = "";
    public string? Tip { get; set; }

    /// <summary><c>aspect</c>, <c>height</c> or <c>refresh</c>: how a first run picks
    /// from the monitor. Anything else starts where the setting is.</summary>
    public string? Suggest { get; set; }

    public List<SetupOption> Options { get; set; } = [];

    /// <summary>Drawn as a slider whose positions are the options, in order, as the
    /// Settings window's frame rate is; a list otherwise.</summary>
    public bool Slider { get; set; }

    /// <summary>The option to start on when the keys match none: a key the runtime
    /// fills at boot, say, and so has not written yet.</summary>
    public int? Default { get; set; }

    /// <summary>The option the Original preset picks; absent, the preset leaves the field where it starts (window mode, say).</summary>
    public int? Original { get; set; }
}

internal sealed class SetupOption
{
    public string Label { get; set; } = "";

    /// <summary>What <see cref="SetupField.Suggest"/> compares: an aspect ratio, a height in pixels, or a refresh rate.</summary>
    public double? Match { get; set; }

    public Dictionary<string, string?> Set { get; set; } = new(StringComparer.OrdinalIgnoreCase);
}

internal sealed class SetupCategory
{
    public string Id { get; set; } = "";
    /// <summary>A localisation key, the heading of the category's own tiers page.</summary>
    public string Label { get; set; } = "";

    public List<SetupGroup> Groups { get; set; } = [];
}

internal sealed class SetupGroup
{
    public string Id { get; set; } = "";
    public string Label { get; set; } = "";
    public List<SetupTier> Tiers { get; set; } = [];
}

internal sealed class SetupTier
{
    /// <summary>A localisation key, the tier's short name on a tiers page's selector (Original, Balanced);
    /// absent, the selector names the run of identical tiers by their names joined.</summary>
    public string? Label { get; set; }

    public string Desc { get; set; } = "";

    /// <summary>Shows the setup's higher-GPU-cost note after the description.</summary>
    public bool Cost { get; set; }

    /// <summary>Every key the group owns, in every tier: null is the key removed,
    /// the feature's default.</summary>
    public Dictionary<string, string?> Set { get; set; } = new(StringComparer.OrdinalIgnoreCase);
}

/// <summary>A choice page: an option (a button each) and, under them, a slider.</summary>
internal sealed class SetupPage
{
    public string Id { get; set; } = "";

    /// <summary>A localisation key, the page's heading.</summary>
    public string Title { get; set; } = "";

    /// <summary>Localisation keys, one wrapped paragraph each.</summary>
    public List<string> Body { get; set; } = [];

    /// <summary>A localisation key over the options.</summary>
    public string Label { get; set; } = "";

    public List<SetupPageOption> Options { get; set; } = [];

    public SetupSlider? Slider { get; set; }

    /// <summary>One per tier (three): what a preset at that tier picks.</summary>
    public List<SetupPageChoice> Presets { get; set; } = [];

    /// <summary>A category id: while the player has not touched this page, it takes the
    /// preset of the tier that category's groups are at when the page is entered.</summary>
    public string? Follows { get; set; }
}

internal sealed class SetupPageOption
{
    public string Label { get; set; } = "";
    public string? Desc { get; set; }

    public Dictionary<string, string?> Set { get; set; } = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Picking this option moves the slider here; null leaves it where it is.</summary>
    public double? Value { get; set; }
}

internal sealed class SetupSlider
{
    public string Key { get; set; } = "";
    public string Label { get; set; } = "";
    public string? Tip { get; set; }
    public double Min { get; set; }
    public double Max { get; set; }
    public double Step { get; set; } = 0.25;

    /// <summary>An ImGui printf format for the value.</summary>
    public string Format { get; set; } = "%.2f";

    /// <summary>A localisation key shown instead of the number at Min.</summary>
    public string? MinLabel { get; set; }
}

/// <summary>A page's choice: an option index (-1 keeps the player's current one) and
/// a slider value (null keeps the slider's key as it is).</summary>
internal sealed class SetupPageChoice
{
    public int Option { get; set; } = -1;
    public double? Value { get; set; }

    public SetupPageChoice Clone() => new() { Option = Option, Value = Value };
}
