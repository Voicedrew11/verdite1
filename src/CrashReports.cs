using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Diagnostics;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Verdite.Core;

/// <summary>
/// What the port adds to the runtime's crash, fault and hang reports (runtime
/// 0107): its version, where the player is (the port's snapshot), and every switch
/// that is not the default. And a way to make each kind of report on purpose, so
/// the paths can be run without waiting for a real one.
///
///     {Tag}_HANG=seconds        the hang watchdog's limit
///     {Tag}_FAULT=hook|crash|hang[:seconds]   a fault on purpose, timed from the first area
///     {Tag}_FAULT=hook@{id}     the hook owned by the patch with that mod id (so that patch is the one turned off)
///
/// The port hands in its product name (the title), its snapshot of the player (the
/// "Game" section) and the address of a main-loop stage, which is what the fault
/// hook fires on: it runs once every main-loop pass.
/// </summary>
public static class CrashReports
{
    static uint _stage;

    // The fault's own mod. Created when the port calls Configure, after Game.Configure.
    static readonly ModInfo _self = new() { Id = $"{Game.Id}.faultinject", Name = "Fault injection", Version = "1.0" };
    static ModInfo _owner = _self;
    static string _mode = "";
    static double _after = 20;
    static readonly System.Diagnostics.Stopwatch _clock = new();

    /// <param name="hang">The port's {Tag}_HANG, or null.</param>
    /// <param name="product">The title's product name, e.g. "Verdite2".</param>
    /// <param name="snapshot">The port's bare JSON of where the player is.</param>
    /// <param name="stage">The address of a main-loop stage the main loop runs every pass, in the "game" overlay.</param>
    public static void Configure(string? hang, string product, Func<string> snapshot, uint stage)
    {
        var version = Assembly.GetEntryAssembly()?.GetCustomAttribute<AssemblyInformationalVersionAttribute>()
                          ?.InformationalVersion
                      ?? typeof(CrashReports).Assembly.GetName().Version?.ToString() ?? "?";
        CrashReport.Title = $"{product} {version}";
        if (double.TryParse(hang, System.Globalization.NumberStyles.Float,
                            System.Globalization.CultureInfo.InvariantCulture, out var seconds))
            Watchdog.HangSeconds = seconds;

        _stage = stage;
        CrashReport.AddSection("Game", snapshot);
        CrashReport.AddSection($"Settings ({Game.Id}.*)", () =>
        {
            var values = RecompOne.Runtime.Config.ConfigManager.View.Values;
            return string.Join('\n', values.Where(v => v.Key.StartsWith($"{Game.Id}.", StringComparison.OrdinalIgnoreCase))
                                           .OrderBy(v => v.Key, StringComparer.Ordinal)
                                           .Select(v => $"{v.Key}={v.Value}"));
        });
        CrashReport.AddSection($"Environment ({Game.Tag}_*)", () =>
        {
            var set = Environment.GetEnvironmentVariables().Keys.Cast<string>()
                                 .Where(k => k.StartsWith(Game.EnvPrefix, StringComparison.Ordinal))
                                 .OrderBy(k => k, StringComparer.Ordinal)
                                 .Select(k => $"{k}={Environment.GetEnvironmentVariable(k)}");
            return string.Join('\n', set) is { Length: > 0 } s ? s : "(none)";
        });
    }

    // {Tag}_FAULT=hook|crash|hang[:seconds], timed from the first fdat module: a
    // test of each report's path, nothing a player sets.
    //     hook   a hook throws: a fault report, a notice, this hook off, the game goes on
    //     crash  the game's own code throws: a crash report and the crash screen
    //     hang   the game thread spins on a hardware register: a hang report with its stack
    // The hook belongs to this file's own mod ({id}.faultinject), which a hook fault
    // turns off for the session; hook@<mod id> makes it that patch's instead, so that
    // patch is the one turned off and its settings page greys (PatchSettings.Failed).
    public static void InstallFault(string? spec)
    {
        if (string.IsNullOrWhiteSpace(spec)) return;
        var parts = spec.Trim().ToLowerInvariant().Split(':');
        _mode = parts[0];
        if (_mode.StartsWith("hook@", StringComparison.Ordinal))
        {
            var id = _mode["hook@".Length..];
            _mode = "hook";
            if (FindMod(id) is not { } owner)
            {
                Console.Error.WriteLine($"[{Game.Tag}] {Game.Tag}_FAULT: no patch with the mod id '{id}'");
                return;
            }
            _owner = owner;
        }
        if (_mode is not ("hook" or "crash" or "hang"))
        {
            Console.Error.WriteLine($"[{Game.Tag}] {Game.Tag}_FAULT: unknown '{spec}', want hook, crash or hang");
            return;
        }

        if (parts.Length > 1) double.TryParse(parts[1], System.Globalization.NumberStyles.Float,
                                               System.Globalization.CultureInfo.InvariantCulture, out _after);
        Event.AddListener<OverlayLoadedEvent>(e =>
        {
            if (!_clock.IsRunning && e.Name.StartsWith("fdat", StringComparison.Ordinal)) _clock.Start();
        });
        HookAttach.OnOverlayLoad("fault", () =>
        {
            SymbolRegistry.Build();
            if (SymbolRegistry.Resolve("game", null, _stage) is not { } t) return false;
            HookManager.AddPre(_owner, t, typeof(CrashReports).GetMethod(nameof(Fire),
                                         BindingFlags.Public | BindingFlags.Static)!);
            HookManager.Commit();
            return HookAttach.Installed(t);
        });
        Console.WriteLine($"[{Game.Tag}] fault injection: {_mode} {_after:F0}s after the first area" +
                          (_owner == _self ? "" : $", as {_owner.Id}"));
    }

    /// <summary>A patch's own ModInfo, by its id: the static <c>_self</c> each patch keeps
    /// it in. Reading one runs that type's static constructor, which a test switch may.</summary>
    static ModInfo? FindMod(string id)
    {
        foreach (var type in typeof(CrashReports).Assembly.GetTypes())
        {
            if (type.IsGenericTypeDefinition) continue;
            foreach (var f in type.GetFields(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic))
                if (f.Name == "_self" && f.FieldType == typeof(ModInfo) && f.GetValue(null) is ModInfo m && m.Id == id) return m;
        }
        return null;
    }

    public static void Fire(CpuContext c, IMemory m)
    {
        if (!_clock.IsRunning || _clock.Elapsed.TotalSeconds < _after) return;
        _clock.Reset();
        Console.WriteLine($"[{Game.Tag}] fault injection: {_mode} now");
        switch (_mode)
        {
            case "hook":
                throw new InvalidOperationException($"{Game.Tag}_FAULT=hook: a hook throwing on purpose");
            case "crash":
                throw HookManager.NotAHookFault(new InvalidOperationException($"{Game.Tag}_FAULT=crash: a crash on purpose"));
            case "hang":
                while (true) m.ReadU32(0x1F801070u);
        }
    }
}
