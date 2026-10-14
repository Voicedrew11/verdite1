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

    /// <summary>u32: the ordering table the frame is being built into (FrameHead sets it).</summary>
    const uint OtPointer = 0x80090EBC;
    const int OtWords = 0x4000;
    const int Renderer = 8, Gate = 9;
    static uint _otAt;

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
        HookManager.Commit();
        int n = targets.Count(HookAttach.Installed);
        Console.WriteLine($"[KF1] stageprobe: {n}/{Stages.Length} stages hooked");
        return n == Stages.Length;
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

    /// <summary>Copies the ordering table; false when the pointer is not a table in main RAM.</summary>
    // Whether the call ran: frame pacing's pre hook skips stages A-H on a frame the
    // world did not tick, and the gate whenever it is on; the hooks still fire.
    static bool Ran(int i) => i == Renderer || (i == Gate ? !FramePacing.Enabled : FramePacing.IterationTicked);

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
        _windowStart = now;
    }
}
