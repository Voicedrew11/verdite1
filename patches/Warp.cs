using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// The command channel's warp verb: warp &lt;area 1-5&gt; &lt;x&gt; &lt;z&gt; [yaw]. The request is
/// queued from the vblank and run on stage A of the main loop, where the game's own
/// exits call the area transition func_80036850. See "A warp, as designed" in
/// docs/GPU_RENDERER.md.
/// </summary>
public static class Warp
{
    // Stage A of the main loop, the pre hook's place.
    const uint FirstStage = 0x80018880;

    // The player. See AgentBeacon for the same block.
    const uint PosX = 0x800A0824;       // s32
    const uint PosZ = 0x800A082C;       // s32
    const uint Yaw = 0x800A083A;        // s16
    const uint CellZ = 0x800A084A;      // u8, the current cell (x/2000, z/2000)
    const uint CellX = 0x800A084B;
    const uint LastCellZ = 0x800A084C;  // u8, the cell the exit test last saw
    const uint LastCellX = 0x800A084D;

    const int CellSize = 2000;

    // func_80036850's A1. The area 1 exits to areas 2-4 leave A1 at 0 (func_80036AF0,
    // 52901), so 0 is the plain arrival.
    const uint Entry = 0;

    sealed record Pending(int Area, int X, int Z, int? Yaw);

    static readonly object _gate = new();
    static Pending? _pending;
    static string _last = "null";
    static readonly ModInfo _self = new() { Id = "kf1.warp", Name = "Warp", Version = "1.0" };

    /// <summary>The last warp as JSON, or null before any.</summary>
    public static string Last { get { lock (_gate) return _last; } }

    public static void Install() => HookAttach.OnOverlayLoad("warp", Attach);

    static bool Attach()
    {
        SymbolRegistry.Build();
        if (SymbolRegistry.Resolve("game", null, FirstStage) is not { } t) return false;
        HookManager.AddPre(_self, t, typeof(Warp).GetMethod(nameof(BeforeStageA), BindingFlags.Public | BindingFlags.Static)!);
        HookManager.Commit();
        return HookAttach.Installed(t);
    }

    /// <summary>Queue a warp from the command channel (on the vblank).</summary>
    public static string Request(string[] args)
    {
        var m = RecompOne.Runtime.Runtime.Mem;
        if (m == null) return AgentServer.Err("not running");
        if (args.Length is < 3 or > 4) return AgentServer.Err("warp <area 1-5> <x> <z> [yaw]");
        if (!int.TryParse(args[0], out int area) || area is < 1 or > 5)
            return AgentServer.Err("area must be 1..5");
        if (!int.TryParse(args[1], out int x) || !int.TryParse(args[2], out int z)
            || x < 0 || z < 0 || x / CellSize > 255 || z / CellSize > 255)
            return AgentServer.Err("x and z must be 0..511999 (a cell of 0..255)");
        int? yaw = null;
        if (args.Length == 4)
        {
            if (!short.TryParse(args[3], out short y)) return AgentServer.Err("yaw must be -32768..32767");
            yaw = y;
        }
        if (AgentBeacon.Overlay != "game" || m.ReadU16(AgentBeacon.MaxHp) == 0)
            return AgentServer.Err("not in an area (GAME.EXE up, max HP set)");

        lock (_gate)
        {
            if (_pending != null) return AgentServer.Err("a warp is already queued");
            _pending = new Pending(area, x, z, yaw);
            _last = "{\"state\":\"queued\"}";
        }
        return "{\"ok\":true,\"cmd\":\"warp\",\"queued\":true,\"area\":" + area + ",\"pos\":[" + x + "," + z +
               "],\"yaw\":" + (yaw is { } v ? v.ToString() : "null") + "}";
    }

    /// <summary>Stage A's pre hook: runs a queued warp in the loop's own call context.</summary>
    public static void BeforeStageA(CpuContext c, IMemory m)
    {
        Pending? r;
        lock (_gate) { r = _pending; _pending = null; }
        if (r is not { } w) return;

        // The exit path polls before it calls the transition, and so does this.
        RecompOne.Runtime.Interrupts.Poll(c, m);
        var saved = c.Snapshot();
        c.A0 = (uint)w.Area;
        c.A1 = Entry;
        Recompiled.KingsField1_game.func_80036850(c, m);
        c.Restore(saved);

        // The transition snapped x and z to the cell centre; the requested ones go back in.
        m.WriteU32(PosX, (uint)w.X);
        m.WriteU32(PosZ, (uint)w.Z);
        if (w.Yaw is { } yaw) m.WriteU16(Yaw, (ushort)(short)yaw);
        byte cx = (byte)(w.X / CellSize), cz = (byte)(w.Z / CellSize);
        m.WriteU8(CellX, cx);
        m.WriteU8(CellZ, cz);
        m.WriteU8(LastCellX, cx);
        m.WriteU8(LastCellZ, cz);

        int y = (int)m.ReadU16(Yaw);
        string json = "{\"state\":\"done\",\"area\":" + w.Area + ",\"pos\":[" + w.X + "," + w.Z +
                      "],\"yaw\":" + (short)y + "}";
        lock (_gate) _last = json;
        Console.WriteLine($"[KF1-WARP] area {w.Area} at ({w.X},{w.Z}), cell ({cx},{cz}), yaw {(short)y}");
    }
}
