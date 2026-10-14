using System.Collections.Concurrent;
using RecompOne.Runtime.Host.Window;
using Rt = RecompOne.Runtime.Runtime;

namespace Verdite.Core;

/// <summary>
/// The only writer of the port's settings to interface.ini. See docs/SETTINGS.md in a port.
///
/// The file is the runtime's <c>ViewConfig</c>: a plain dictionary loaded once at
/// boot, and <c>Rt.SaveView</c> writes all of it back and asks ImGui for its layout
/// on the way. Both belong to the host thread. The game's menu runs on the game
/// thread, so a change made there is queued here and written by <see cref="Pump"/>,
/// an invisible panel the host draws every frame.
///
/// A write touches one key. A value the key already holds writes nothing, and a
/// setting put back to its default removes its key rather than writing the
/// default, so a player who never chose follows the default if a release changes it.
/// </summary>
public static class SettingsStore
{
    /// <summary>A setting's new value, or null to remove its key (the default).</summary>
    public readonly record struct Change(PortSetting Setting, double? Value);

    static readonly ConcurrentQueue<(Change[] Changes, string From)> _queue = new();
    static int _hostThread = -1;

    public static long Writes { get; private set; }

    static bool IsHost => Environment.CurrentManagedThreadId == _hostThread;

    public static void Install() => PanelManager.Register(Pump.Instance);

    /// <summary>The Settings window's write, when a control is let go.</summary>
    public static void Write(PortSetting s, double value) => Submit([new(s, value)], "settings window");

    /// <summary>From any thread: written now on the host thread, else on its next frame.</summary>
    public static void Submit(IReadOnlyList<Change> changes, string from)
    {
        if (changes.Count == 0) return;
        if (IsHost) Commit([.. changes], from);
        else _queue.Enqueue(([.. changes], from));
    }

    static void Commit(Change[] changes, string from)
    {
        var values = Rt.View.Values;
        bool changed = false;
        foreach (var (s, value) in changes)
            foreach (var (key, text) in s.Texts(value))
            {
                if (text is not null)
                {
                    if (values.TryGetValue(key, out var old) && old == text) continue;
                    values[key] = text;
                    Console.WriteLine($"[{Game.Tag}] settings: {key}={text} ({from})");
                }
                else
                {
                    if (!values.Remove(key)) continue;
                    Console.WriteLine($"[{Game.Tag}] settings: {key} back to its default ({from})");
                }
                changed = true;
            }
        if (!changed) return;
        Rt.SaveView();
        Writes++;
    }

    /// <summary>From any thread: <paramref name="work"/> on the host thread, now when
    /// called there, else at the start of its next frame.</summary>
    public static void OnHost(Action work)
    {
        if (IsHost) work();
        else _work.Enqueue(work);
    }

    static readonly ConcurrentQueue<Action> _work = new();

    /// <summary>The key's text, read on the host thread; null when no key is kept.</summary>
    public static string? StoredText(PortSetting s) =>
        IsHost && Rt.View.Values.TryGetValue(s.Key, out var text) ? text : null;

    /// <summary>Never shown: the host draws every open panel each frame, which is the
    /// one per-frame call a patch gets on the host thread without a runtime change.</summary>
    sealed class Pump : IFloatingPanel
    {
        public static readonly Pump Instance = new();

        public string Name => Game.Id + "settingsstore";
        public bool IsOpen { get => true; set { } }

        public void Draw()
        {
            _hostThread = Environment.CurrentManagedThreadId;
            while (_work.TryDequeue(out var work)) work();
            while (_queue.TryDequeue(out var item)) Commit(item.Changes, item.From);
            WindowMode.Watch();
        }
    }
}
