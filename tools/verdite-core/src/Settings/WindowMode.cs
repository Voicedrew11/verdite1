using RecompOne.Runtime.Config;
using RecompOne.Runtime.Host;

namespace Verdite.Core;

/// <summary>
/// Windowed (0), fullscreen (1) or borderless (2), for the game's settings page. The
/// runtime keeps it as two keys, <c>Fullscreen</c> and <c>Borderless</c>, read by F11,
/// the menu bar and the Display tab, all on the host thread. See docs/SETTINGS.md in a port.
///
/// A change from the page is applied on the host thread and the two keys put back
/// as they were, so a change not yet saved never reaches the file with some other
/// save. <see cref="Watch"/> follows a change made anywhere else (F11, the Display
/// tab, the page's own save) by the keys moving.
/// </summary>
public static class WindowMode
{
    public const string FullscreenKey = "Fullscreen", BorderlessKey = "Borderless";

    static volatile int _mode;
    static int _seen = -1;

    public static int Mode => _mode;

    static int FromKeys => !ConfigManager.View.Fullscreen ? 0 : ConfigManager.View.Borderless ? 2 : 1;

    /// <summary>From any thread.</summary>
    public static void Set(int mode)
    {
        _mode = mode = Math.Clamp(mode, 0, 2);
        SettingsStore.OnHost(() =>
        {
            var values = ConfigManager.View.Values;
            values.TryGetValue(FullscreenKey, out var fullscreen);
            values.TryGetValue(BorderlessKey, out var borderless);
            ConfigManager.View.Fullscreen = mode != 0;
            if (mode != 0) ConfigManager.View.Borderless = mode == 2;
            HostWindow.SetFullscreen(mode != 0);
            Put(values, FullscreenKey, fullscreen);
            Put(values, BorderlessKey, borderless);
        });
    }

    static void Put(Dictionary<string, string> values, string key, string? text)
    {
        if (text is null) values.Remove(key);
        else values[key] = text;
    }

    /// <summary>The keys as the page keeps them: windowed removes <c>Fullscreen</c> and
    /// leaves <c>Borderless</c>, so F11 still covers the screen the way it did.</summary>
    public static (string, string?)[] Keys(double? value) => (int)Math.Round(value ?? 0) switch
    {
        0 => [(FullscreenKey, null)],
        1 => [(FullscreenKey, "True"), (BorderlessKey, null)],
        _ => [(FullscreenKey, "True"), (BorderlessKey, "True")],
    };

    /// <summary>The host's every frame, after the settings store has written.</summary>
    public static void Watch()
    {
        int now = FromKeys;
        if (now == _seen) return;
        _seen = now;
        _mode = now;
    }
}
