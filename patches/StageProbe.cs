using System.Diagnostics;
using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// King's Field's main loop, measured stage by stage (KF2_STAGE_PROBE=1).
///
/// The loop at 0x8001482C calls eight routines and then the frame gate
/// (func_800149F4). Frame pacing needs to know which of them only advance the
/// world and which draw, and a static call graph cannot say: every stage reaches
/// DrawOTag through some modal loop. This counts, per stage and per second, the
/// calls, the time, the GTE projections made (GteDepth.Recorded) and the
/// DrawOTag calls made while it was the innermost stage running. See "The main
/// loop" in docs/KF1.md.
/// </summary>
public static class StageProbe
{
    public static readonly (uint Addr, string Name)[] Stages =
    [
        (0x80018880, "A 80018880"),
        (0x80017E3C, "B 80017E3C"),
        (0x8003303C, "C 8003303C"),
        (0x8002CAD4, "D 8002CAD4"),
        (0x80030818, "E 80030818"),
        (0x80031CC8, "F 80031CC8"),
        (0x8003A760, "G 8003A760"),
        (0x8003596C, "H 8003596C"),
        (0x8001FDE4, "I 8001FDE4"),
        (0x800149F4, "gate 800149F4"),
    ];

    static readonly ModInfo _self = new()
    {
        Id = "kf1.stageprobe",
        Name = "Main-loop stage probe",
        Version = "1.0",
        Description = "Which of King's Field's main-loop stages draw.",
    };

    static bool _on;
    static readonly long[] Calls = new long[Stages.Length + 1];
    static readonly long[] Ticks = new long[Stages.Length + 1];
    static readonly long[] Projected = new long[Stages.Length + 1];
    static readonly long[] Draws = new long[Stages.Length + 1];
    static readonly int[] _stack = new int[64];
    static readonly long[] _t0 = new long[64];
    static readonly long[] _p0 = new long[64];
    static int _depth;
    static long _windowStart = Stopwatch.GetTimestamp();

    static int _flipOffset = -1;
    static ushort _saved;

    public static void Configure(string? probe)
    {
        _on = !string.IsNullOrWhiteSpace(probe) && probe.Trim() != "0";
        // KF2_STAGE_PROBE=flip:N turns the halfword at 0x80065098+N by 2048 (half a
        // turn) for the length of the renderer's call, and puts it back: the test
        // of whether the renderer builds its view from that block.
        if (_on && probe!.Trim().StartsWith("flip:")) _flipOffset = int.Parse(probe.Trim()[5..]);
    }

    public static void Install()
    {
        if (!_on) return;
        HookAttach.OnOverlayLoad("stage probe", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        const BindingFlags f = BindingFlags.Public | BindingFlags.Static;
        MethodInfo? any = null;
        for (int i = 0; i < Stages.Length; i++)
        {
            var target = SymbolRegistry.Resolve("game", null, Stages[i].Addr);
            if (target == null) return false;
            HookManager.AddPre(_self, target, typeof(StageProbe).GetMethod($"Pre{i}", f)!);
            HookManager.AddPost(_self, target, typeof(StageProbe).GetMethod($"Post{i}", f)!);
            any = target;
        }
        foreach (var (ov, addr) in SdkAddr.DrawOTag)
        {
            var t = SymbolRegistry.Resolve(ov, null, addr);
            if (t != null) HookManager.AddPre(_self, t, typeof(StageProbe).GetMethod(nameof(OnDraw), f)!);
        }
        HookManager.Commit();
        Console.WriteLine($"[KF1] stage probe: {Stages.Length} stages hooked");
        return HookAttach.Installed(any);
    }

    static void Enter(int i)
    {
        if (i == 8 && _flipOffset >= 0 && RecompOne.Runtime.Runtime.Mem is { } mem)
        {
            uint a = 0x80065098u + (uint)_flipOffset;
            _saved = mem.ReadU16(a);
            mem.WriteU16(a, (ushort)(_saved + 2048));
        }
        if (_depth < _stack.Length)
        {
            _stack[_depth] = i;
            _t0[_depth] = Stopwatch.GetTimestamp();
            _p0[_depth] = GteDepth.Recorded;
        }
        _depth++;
        Calls[i]++;
    }

    static void Leave(int i)
    {
        if (i == 8 && _flipOffset >= 0 && RecompOne.Runtime.Runtime.Mem is { } mem)
            mem.WriteU16(0x80065098u + (uint)_flipOffset, _saved);
        _depth--;
        if (_depth < 0) { _depth = 0; return; }
        if (_depth < _stack.Length)
        {
            Ticks[i] += Stopwatch.GetTimestamp() - _t0[_depth];
            Projected[i] += GteDepth.Recorded - _p0[_depth];
        }
        if (i == Stages.Length - 1) Report();
    }

    public static void OnDraw(CpuContext c, IMemory m)
    {
        int who = _depth == 0 ? Stages.Length : _stack[Math.Min(_depth, _stack.Length) - 1];
        Draws[who]++;
    }

    static void Report()
    {
        double secs = (Stopwatch.GetTimestamp() - _windowStart) / (double)Stopwatch.Frequency;
        if (secs < 2.0) return;
        var line = new System.Text.StringBuilder("[KF1] stages/s:");
        for (int i = 0; i <= Stages.Length; i++)
        {
            string name = i < Stages.Length ? Stages[i].Name : "outside";
            if (Calls[i] == 0 && Draws[i] == 0) continue;
            line.Append($"\n    {name,-14} calls {Calls[i] / secs,6:0.0}  ms/call {(Calls[i] == 0 ? 0 : Ticks[i] * 1000.0 / Stopwatch.Frequency / Calls[i]),7:0.000}" +
                        $"  projected/s {Projected[i] / secs,8:0}  DrawOTag/s {Draws[i] / secs,5:0.0}");
        }
        Console.WriteLine(line.ToString());
        Array.Clear(Calls); Array.Clear(Ticks); Array.Clear(Projected); Array.Clear(Draws);
        _windowStart = Stopwatch.GetTimestamp();
    }

    public static void Pre0(CpuContext c, IMemory m) => Enter(0);
    public static void Post0(CpuContext c, IMemory m) => Leave(0);
    public static void Pre1(CpuContext c, IMemory m) => Enter(1);
    public static void Post1(CpuContext c, IMemory m) => Leave(1);
    public static void Pre2(CpuContext c, IMemory m) => Enter(2);
    public static void Post2(CpuContext c, IMemory m) => Leave(2);
    public static void Pre3(CpuContext c, IMemory m) => Enter(3);
    public static void Post3(CpuContext c, IMemory m) => Leave(3);
    public static void Pre4(CpuContext c, IMemory m) => Enter(4);
    public static void Post4(CpuContext c, IMemory m) => Leave(4);
    public static void Pre5(CpuContext c, IMemory m) => Enter(5);
    public static void Post5(CpuContext c, IMemory m) => Leave(5);
    public static void Pre6(CpuContext c, IMemory m) => Enter(6);
    public static void Post6(CpuContext c, IMemory m) => Leave(6);
    public static void Pre7(CpuContext c, IMemory m) => Enter(7);
    public static void Post7(CpuContext c, IMemory m) => Leave(7);
    public static void Pre8(CpuContext c, IMemory m) => Enter(8);
    public static void Post8(CpuContext c, IMemory m) => Leave(8);
    public static void Pre9(CpuContext c, IMemory m) => Enter(9);
    public static void Post9(CpuContext c, IMemory m) => Leave(9);
}
