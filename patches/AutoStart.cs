using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hardware;

namespace Kf1;

/// <summary>
/// Boot straight into a New Game with nobody at the pad:
///
///     KF1_AUTOSTART=new
///
/// Start is pulsed through OPEN.EXE's attract and title (through the BIOS pad
/// read, as in Verdite3) until GAME.EXE loads, then nothing more is pressed: a
/// Start that lands in the area opens the in-game menu. See "Driving the game
/// without a person" in docs/DEVELOPMENT.md.
/// </summary>
public static class AutoStart
{
    public static bool NewGame { get; private set; }

    static volatile string _overlay = "boot";
    static volatile ushort _inject;
    static bool _done;

    public static void Configure(string? spec)
    {
        if (string.IsNullOrWhiteSpace(spec)) return;
        if (!spec.Trim().Equals("new", StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException($"KF1_AUTOSTART: '{spec}' is not new");
        NewGame = true;
    }

    public static void Install()
    {
        if (!NewGame) return;

        Event.AddListener<OverlayLoadedEvent>(e =>
        {
            _overlay = e.Name;
            if (e.Name == "game") _inject = 0;
        });

        // Active-low, bytes swapped against Controller's layout (Verdite Core's Mouse).
        Event.AddListener<PadReadEvent>(e =>
        {
            if (e.Port != 0) return;
            ushort press = _inject;
            if (press != 0) e.Buttons &= (ushort)~(ushort)((press >> 8) | (press << 8));
        });

        Event.AddListener<VSyncEvent>(_ => Announce());

        new Thread(Drive) { IsBackground = true, Name = "kf1-autostart" }.Start();
        Console.WriteLine("[KF1] autostart: booting into a New Game");
    }

    /// <summary>Start through OPEN.EXE, until GAME.EXE takes over; then nothing.</summary>
    static void Drive()
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (!_done && _overlay != "game")
        {
            double t = sw.Elapsed.TotalSeconds;
            _inject = _overlay == "open" && (long)(t * 60) % 60 < 10 ? Controller.Start : (ushort)0;
            Thread.Sleep(1);
        }
        _inject = 0;
    }

    /// <summary>Once the main loop turns in an area, say where and stop.</summary>
    static void Announce()
    {
        if (_done || _overlay != "game") return;
        var m = RecompOne.Runtime.Runtime.Mem;
        if (m == null || m.ReadU16(AgentBeacon.MaxHp) == 0 || !AgentBeacon.LoopLive) return;
        _done = true;
        Console.WriteLine($"[KF1] autostart: in area {m.ReadU8(AgentBeacon.Area)}, " +
                          $"HP {m.ReadU16(AgentBeacon.Hp)}/{m.ReadU16(AgentBeacon.MaxHp)}");
    }
}
