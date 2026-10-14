using System.Reflection;
using System.Runtime.InteropServices;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;
using RecompOne.Runtime.Sdk;

namespace Kf1;

/// <summary>
/// KF1_STAGEPROBE=1: which main-loop stages submit primitives. Each stage's ordering
/// table words are diffed across its call, and its calls and presents counted; one
/// line every five seconds. Stage I is the renderer, replaced in C# by
/// <see cref="Renderer"/>; the hooks here still fire around the replacement. See
/// "GAME.EXE's set-up and main loop" in docs/GAME_INTERNALS.md.
///
/// The in-game menu does not run the stages, so it has its own line when it runs: the
/// present and the table clear, and the 2D writers it calls, with the packet-record
/// cursor (0x80070EB8 +8) and the menu's packet pointer (0x80057E88) charged per call.
/// See "Drawing contexts" in docs/GPU_RENDERER.md.
/// </summary>
public static class StageProbe
{
    // The main loop from 0x8001482C, stages A..I, then the gate (func_800149F4).
    static readonly (string Name, uint Addr)[] Stages =
    [
        ("A", 0x80018880), ("B", 0x80017E3C), ("C", 0x8003303C), ("D", 0x8002CAD4),
        ("E", 0x80030818), ("F", 0x80031CC8), ("G", 0x8003A760), ("H", 0x8003596C),
        ("I", 0x8001FDE4), ("gate", 0x800149F4),
    ];

    // The menu: its present (func_8002AC34), its table clear (func_8002ABB4), and the
    // writers. A writer is charged for the cursor it advances, not its callees'.
    static readonly (string Name, uint Addr, bool Writer)[] Menu =
    [
        ("present", 0x8002AC34, false), ("clear", 0x8002ABB4, false),
        ("80022D7C", 0x80022D7C, true), ("80025DA0", 0x80025DA0, true),
        ("80027E58", 0x80027E58, true), ("80027EA0", 0x80027EA0, true),
        ("80027EE4", 0x80027EE4, true), ("8002A510", 0x8002A510, true),
        ("8002ACCC", 0x8002ACCC, true), ("8002AD1C", 0x8002AD1C, true),
        ("800292F8", 0x800292F8, true), ("card", 0x8001B7B0, true),
    ];

    /// <summary>u32: the ordering table the frame is being built into (FrameHead sets it).</summary>
    const uint OtPointer = 0x80090EBC;
    const int OtWords = 0x4000;
    const int Renderer = 8, Gate = 9;
    static uint _otAt;

    /// <summary>u32: the packet record (start, end, cursor at +8); every writer bumps the cursor.</summary>
    const uint Record = 0x80070EB8;
    /// <summary>u32: the menu's packet pointer, bumped by its writers.</summary>
    const uint MenuPacket = 0x80057E88;

    static readonly ModInfo _self = new()
    {
        Id = "kf1.stageprobe",
        Name = "Stage probe",
        Version = "1.0",
        Description = "Counts the main loop's stages and the ordering-table words they write.",
    };

    static bool _on;
    static readonly long[] _calls = new long[Stages.Length];
    static readonly long[] _otWrites = new long[Stages.Length];
    static readonly long[] _presents = new long[Stages.Length];
    static readonly long[] _vsAt = new long[Stages.Length];
    static readonly bool[] _valid = new bool[Stages.Length];
    static readonly uint[][] _before = new uint[Stages.Length][];
    static readonly uint[] _after = new uint[OtWords];
    static readonly Stack<int> _stack = new();
    static long _windowStart = -1;

    // The menu's counters: calls, and bytes its writers advanced the record and the
    // packet pointer by, exclusive of the writers they call.
    static readonly long[] _menuCalls = new long[Menu.Length];
    static readonly long[] _menuRec = new long[Menu.Length];
    static readonly long[] _menuPkt = new long[Menu.Length];

    // The writers entered and not yet left: which one, the cursors on entry, and the
    // bytes its callees have already been charged.
    static readonly (int Slot, uint Rec, uint Cur, uint Pkt, long ChildRec, long ChildPkt)[] _menuStack =
        new (int, uint, uint, uint, long, long)[16];
    static int _menuDepth;

    public static void Configure(string? on) => _on = on?.Trim() == "1";

    public static void Install()
    {
        if (!_on) return;
        for (int i = 0; i < Stages.Length; i++) _before[i] = new uint[OtWords];
        HookAttach.OnOverlayLoad("stageprobe", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var self = typeof(StageProbe);
        var pre = self.GetMethod(nameof(Pre), BindingFlags.Public | BindingFlags.Static)!;
        var post = self.GetMethod(nameof(Post), BindingFlags.Public | BindingFlags.Static)!;
        var targets = new List<MethodInfo>();
        foreach (var (name, addr) in Stages)
        {
            var t = SymbolRegistry.Resolve("game", null, addr);
            if (t == null) { Console.Error.WriteLine($"[KF1] stageprobe: no game/0x{addr:X8} ({name})"); return false; }
            HookManager.AddPre(_self, t, pre);
            HookManager.AddPost(_self, t, post);
            targets.Add(t);
        }

        // The menu's routines. A pre-hook gets only the CpuContext, so the slot is baked
        // in per routine; the post goes first, as an orphan post is harmless.
        int menu = 0;
        for (int i = 0; i < Menu.Length; i++)
        {
            var t = SymbolRegistry.Resolve("game", null, Menu[i].Addr);
            if (t == null) { Console.Error.WriteLine($"[KF1] stageprobe: no game/0x{Menu[i].Addr:X8} (menu {Menu[i].Name})"); continue; }
            var slot = typeof(MenuSlot<>).MakeGenericType(Markers[i]);
            slot.GetField(nameof(MenuSlot<int>.Id))!.SetValue(null, i);
            if (HookManager.AddPost(_self, t, slot.GetMethod(nameof(MenuSlot<int>.Post))!) &&
                HookManager.AddPre(_self, t, slot.GetMethod(nameof(MenuSlot<int>.Pre))!)) menu++;
        }

        HookManager.Commit();
        int n = targets.Count(HookAttach.Installed);
        Console.WriteLine($"[KF1] stageprobe: {n}/{Stages.Length} stages hooked, {menu}/{Menu.Length} menu routines");
        return n == Stages.Length && menu == Menu.Length;
    }

    public static void Pre(CpuContext c, IMemory m)
    {
        int i = Index(c);
        _stack.Push(i);
        if (i < 0) return;
        _vsAt[i] = LibEtc.VSyncCalls;
        _valid[i] = Snapshot(m, _before[i]);
    }

    public static void Post(CpuContext c, IMemory m)
    {
        int i = _stack.Count > 0 ? _stack.Pop() : -1;
        if (i < 0 || !Ran(i)) { Report(); return; }
        _calls[i]++;
        _presents[i] += LibEtc.VSyncCalls - _vsAt[i];
        if (Snapshot(m, _after))
        {
            if (i == Renderer)
                // The renderer flips to the other table and clears it, so its slots
                // are counted: an empty one links to the entry below it.
                for (int k = 1; k < OtWords; k++) { if ((_after[k] & 0xFFFFFFu) != ((_otAt + (uint)(k - 1) * 4u) & 0xFFFFFFu)) _otWrites[i]++; }
            else if (_valid[i])
            {
                var before = _before[i];
                for (int k = 0; k < OtWords; k++) if (_after[k] != before[k]) _otWrites[i]++;
            }
        }
        Report();
    }

    // The menu's routines: a call is counted on exit; a writer also charges the cursors.
    static void MenuEnter(int slot, IMemory m)
    {
        if (!Menu[slot].Writer) return;
        if (_menuDepth >= _menuStack.Length) { _menuDepth++; return; }
        var (rec, cur) = Cursors(m);
        _menuStack[_menuDepth++] = (slot, rec, cur, m.ReadU32(MenuPacket), 0L, 0L);
    }

    static void MenuLeave(int slot, IMemory m)
    {
        _menuCalls[slot]++;
        if (!Menu[slot].Writer) { Report(); return; }
        if (_menuDepth <= 0) { Report(); return; }
        if (_menuDepth > _menuStack.Length) { _menuDepth--; Report(); return; }
        var f = _menuStack[--_menuDepth];

        // The record moved across the call only if the writer swapped buffers; then its
        // cursor has no meaning here and the advance is not charged.
        var (rec, cur) = Cursors(m);
        long recAdv = rec == f.Rec && rec != 0 ? (long)(uint)(cur - f.Cur) : 0L;
        long pkt = (long)(uint)(m.ReadU32(MenuPacket) - f.Pkt);
        _menuRec[slot] += Math.Max(0L, recAdv - f.ChildRec);
        _menuPkt[slot] += Math.Max(0L, pkt - f.ChildPkt);

        if (_menuDepth > 0 && _menuDepth <= _menuStack.Length)
        {
            var parent = _menuStack[_menuDepth - 1];
            _menuStack[_menuDepth - 1] = (parent.Slot, parent.Rec, parent.Cur, parent.Pkt,
                parent.ChildRec + recAdv, parent.ChildPkt + pkt);
        }
        Report();
    }

    // The packet record's cursor, and 0 when no record is set up.
    static (uint Rec, uint Cur) Cursors(IMemory m)
    {
        uint rec = m.ReadU32(Record);
        return rec < 0x80000000u ? (0u, 0u) : (rec, m.ReadU32(rec + 8u));
    }

    // Whether the call ran: frame pacing's pre hook skips stages A-H on a frame the
    // world did not tick, and the gate whenever it is on; the hooks still fire.
    static bool Ran(int i) => i == Renderer || (i == Gate ? !FramePacing.Enabled : FramePacing.IterationTicked);

    /// <summary>Copies the ordering table; false when the pointer is not a table in main RAM.</summary>
    static bool Snapshot(IMemory m, uint[] into)
    {
        if (m is not PSMemory p) return false;
        uint ot = p.ReadU32(OtPointer);
        _otAt = ot;
        if (ot < 0x80000000u || ot - 0x80000000u + OtWords * 4u > 0x200000u) return false;
        var ram = MemoryMarshal.Cast<byte, uint>(p.Ram);
        ram.Slice((int)((ot - 0x80000000u) / 4), OtWords).CopyTo(into);
        return true;
    }

    // The return address names the call site: the jal is RA - 8. The loop at
    // 0x8001482C makes the calls in order; anything else is not a stage.
    static int Index(CpuContext c)
    {
        uint pc = (uint)c.RA - 8u;
        return pc switch
        {
            0x8001482C => 0, 0x80014858 => 1, 0x80014870 => 2, 0x80014888 => 3, 0x80014890 => 4,
            0x80014898 => 5, 0x800148A0 => 6, 0x800148A8 => 7, 0x800148C0 => 8, 0x800148C8 => 9,
            _ => -1,
        };
    }

    static void Report()
    {
        long now = Environment.TickCount64;
        if (_windowStart < 0) { _windowStart = now; return; }
        double s = (now - _windowStart) / 1000.0;
        if (s < 5.0) return;
        var parts = new List<string>();
        for (int i = 0; i < Stages.Length; i++)
        {
            parts.Add($"{Stages[i].Name}:{Stages[i].Addr:X8} {_calls[i] / s:0.0}/s " +
                      $"ot {_otWrites[i] / Math.Max(1, _calls[i])} vs {_presents[i] / s:0.0}/s");
            _calls[i] = _otWrites[i] = _presents[i] = 0;
        }
        Console.WriteLine("[KF1] stageprobe: " + string.Join(" | ", parts));
        ReportMenu(s);
        _windowStart = now;
    }

    // One line when the menu ran in the window; nothing otherwise.
    static void ReportMenu(double s)
    {
        var parts = new List<string>();
        for (int i = 0; i < Menu.Length; i++)
        {
            long n = _menuCalls[i];
            if (n > 0)
            {
                string text = $"{Menu[i].Name} {n / s:0.0}/s";
                if (Menu[i].Writer)
                    // Words: a cursor step is 4 bytes; the packet pointer is the menu's own.
                    text += $", rec {_menuRec[i] / 4.0 / n:0.0} pkt {_menuPkt[i] / 4.0 / n:0.0} words/call";
                parts.Add(text);
            }
            _menuCalls[i] = _menuRec[i] = _menuPkt[i] = 0;
        }
        if (parts.Count > 0) Console.WriteLine("[KF1] stageprobe menu: " + string.Join(" | ", parts));
    }

    // One distinct pair of methods per menu routine, so the same delegate is not shared.
    struct M0; struct M1; struct M2; struct M3; struct M4; struct M5; struct M6;
    struct M7; struct M8; struct M9; struct M10; struct M11;

    static readonly Type[] Markers =
    [
        typeof(M0), typeof(M1), typeof(M2), typeof(M3), typeof(M4), typeof(M5), typeof(M6),
        typeof(M7), typeof(M8), typeof(M9), typeof(M10), typeof(M11),
    ];

    static class MenuSlot<T>
    {
        public static int Id;
        public static void Pre(CpuContext c, IMemory m) => MenuEnter(Id, m);
        public static void Post(CpuContext c, IMemory m) => MenuLeave(Id, m);
    }
}
