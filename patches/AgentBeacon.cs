using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// KF1_AGENT=1: a [KF1-AGENT] line on every overlay load and a JSON snapshot about
/// once a second, so a program can tell "at the title" from "in an area" without
/// a screenshot. Verdite3's AgentBeacon; the fields are this game's. Read-only,
/// on the game thread (the vblank). See "Driving the game without a person" in
/// docs/DEVELOPMENT.md.
/// </summary>
public static class AgentBeacon
{
    // The player. See "The player" in docs/GAME_INTERNALS.md.
    public const uint MaxHp = 0x800A0790;   // u16
    public const uint Hp    = 0x800A0792;   // u16
    public const uint MaxMp = 0x800A0794;   // u16
    public const uint Mp    = 0x800A0796;   // u16
    public const uint Area  = 0x800A078A;   // u8, n for KF/Bn
    public const uint PosX  = 0x800A0824;   // s32, the player's position
    public const uint PosY  = 0x800A0828;
    public const uint PosZ  = 0x800A082C;
    public const uint Pitch = 0x800A0838;   // s16
    public const uint Yaw   = 0x800A083A;   // s16, 0x1000 a turn

    const long PeriodMs = 1000;

    // Stage A of the main loop: seen within a second means the loop is turning,
    // not a menu's own loop or a load.
    const uint FirstStage = 0x80018880;
    static long _loopMs = -1;
    static readonly ModInfo _self = new() { Id = "kf1.beacon", Name = "Agent beacon", Version = "1.0" };

    /// <summary>Whether the main loop ran in the last second.</summary>
    public static bool LoopLive => _loopMs >= 0 && Environment.TickCount64 - _loopMs < 1000;

    public static void MainLoopTurned(CpuContext c, IMemory m) => _loopMs = Environment.TickCount64;

    static bool _on;
    static volatile string _overlay = "boot";
    static long _lastEmit;

    public static string Overlay => _overlay;

    public static void Configure(string? on)
    {
        _on = !string.IsNullOrWhiteSpace(on)
              && on.Trim().ToLowerInvariant() is "1" or "on" or "true" or "yes";
    }

    public static void Install()
    {
        Event.AddListener<OverlayLoadedEvent>(e =>
        {
            _overlay = e.Name;
            if (_on) Console.WriteLine($"[KF1-AGENT] overlay {e.Name}");
        });
        HookAttach.OnOverlayLoad("beacon", () =>
        {
            SymbolRegistry.Build();
            if (SymbolRegistry.Resolve("game", null, FirstStage) is not { } t) return false;
            HookManager.AddPre(_self, t, typeof(AgentBeacon).GetMethod(nameof(MainLoopTurned),
                                         BindingFlags.Public | BindingFlags.Static)!);
            HookManager.Commit();
            return HookAttach.Installed(t);
        });
        if (!_on) return;

        Event.AddListener<VSyncEvent>(_ =>
        {
            long now = Environment.TickCount64;
            if (now - _lastEmit < PeriodMs) return;
            _lastEmit = now;
            if (RecompOne.Runtime.Runtime.Mem != null) Console.WriteLine("[KF1-AGENT] " + Snapshot());
        });
        Console.WriteLine("[KF1-AGENT] beacon on");
    }

    /// <summary>The bare {...} JSON, shared with the command channel's state.</summary>
    public static string Snapshot()
    {
        var m = RecompOne.Runtime.Runtime.Mem;
        if (m == null) return "{\"overlay\":\"boot\",\"inGame\":false}";

        // The block is GAME.EXE's and a New Game fills it, so a max HP of zero, or
        // OPEN.EXE, is not in an area.
        if (_overlay != "game" || m.ReadU16(MaxHp) == 0)
            return $"{{\"overlay\":\"{_overlay}\",\"inGame\":false}}";

        int x = (int)m.ReadU32(PosX), y = (int)m.ReadU32(PosY), z = (int)m.ReadU32(PosZ);
        return
            $"{{\"overlay\":\"{_overlay}\",\"inGame\":true,\"loop\":{(LoopLive ? "true" : "false")}," +
            $"\"hp\":{m.ReadU16(Hp)},\"maxHp\":{m.ReadU16(MaxHp)}," +
            $"\"mp\":{m.ReadU16(Mp)},\"maxMp\":{m.ReadU16(MaxMp)}," +
            $"\"area\":{m.ReadU8(Area)}," +
            $"\"pos\":[{x},{y},{z}],\"pitch\":{(short)m.ReadU16(Pitch)},\"yaw\":{(short)m.ReadU16(Yaw)}}}";
    }
}
