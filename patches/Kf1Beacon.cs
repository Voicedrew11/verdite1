using RecompOne.Runtime.Events;

namespace Kf2;

/// <summary>
/// King's Field's state on stdout, once a second (KF2_AGENT=1): the overlay, the
/// player's position (<c>0x800A0824</c>, x/y/z int32) and rotation
/// (<c>0x800A0838</c>, pitch/yaw/roll). The KF2 port's AgentBeacon reads King's
/// Field II's addresses; this is the same line for this game, for a program that
/// has to tell "at the title" from "walking" without a screenshot.
///
///     [KF1-AGENT] {"overlay":"game","pos":[x,y,z],"rot":[pitch,yaw,roll]}
/// </summary>
public static class Kf1Beacon
{
    const uint Pos = 0x800A0824, Rot = 0x800A0838;
    // Two u16 pairs the HUD code at the head of the renderer (0x8001FE68) reads
    // to size its gauges: 30/30 and 20/20 at a New Game. Which of each pair is the
    // current value and which the maximum is not yet confirmed by taking damage.
    const uint Hp = 0x800A0790, Mp = 0x800A0794;
    static bool _on;
    static string _overlay = "boot";

    public static void Configure(string? on) => _on = !string.IsNullOrWhiteSpace(on) && on.Trim() != "0";

    /// <summary>KF2_AGENT_DUMP=path: guest RAM (2 MB) written there once, 10 s after
    /// GAME.EXE loads, for offline analysis of the game's tables.</summary>
    static readonly string? DumpPath = Environment.GetEnvironmentVariable("KF2_AGENT_DUMP");

    public static void Install()
    {
        if (!string.IsNullOrWhiteSpace(DumpPath))
            Event.AddListener<OverlayLoadedEvent>(e =>
            {
                if (e.Name != "game") return;
                new Thread(() =>
                {
                    Thread.Sleep(10000);
                    if (RecompOne.Runtime.Runtime.Mem is RecompOne.Runtime.Memory.PSMemory pm)
                    {
                        File.WriteAllBytes(DumpPath!, pm.Ram[..0x200000].ToArray());
                        Console.WriteLine($"[KF1-AGENT] RAM written to {DumpPath}");
                    }
                }) { IsBackground = true }.Start();
            });
        if (!_on) return;
        Event.AddListener<OverlayLoadedEvent>(e => { _overlay = e.Name; Emit(); });
        new Thread(() =>
        {
            while (true)
            {
                Thread.Sleep(1000);
                Emit();
            }
        }) { IsBackground = true, Name = "kf1-beacon" }.Start();
    }

    static void Emit()
    {
        var m = RecompOne.Runtime.Runtime.Mem;
        if (m == null) return;
        string pos = "null", rot = "null", hp = "null", mp = "null";
        if (_overlay == "game")
        {
            pos = $"[{(int)m.ReadU32(Pos)},{(int)m.ReadU32(Pos + 4)},{(int)m.ReadU32(Pos + 8)}]";
            rot = $"[{(short)m.ReadU16(Rot)},{(short)m.ReadU16(Rot + 2)},{(short)m.ReadU16(Rot + 4)}]";
            hp = $"[{m.ReadU16(Hp)},{m.ReadU16(Hp + 2)}]";
            mp = $"[{m.ReadU16(Mp)},{m.ReadU16(Mp + 2)}]";
        }
        Console.WriteLine($"[KF1-AGENT] {{\"overlay\":\"{_overlay}\",\"pos\":{pos},\"rot\":{rot},\"hp\":{hp},\"mp\":{mp}}}");
    }
}
