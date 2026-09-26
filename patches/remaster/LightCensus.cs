using System.Diagnostics;
using System.Reflection;
using System.Text;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2.Remaster;

/// <summary>
/// Who reads and writes the area's light records while the game runs: the 80
/// records stage 1 copies from <c>0x800679A0</c> (0x2C bytes each) into
/// <c>0x801930F0</c> (0x68 bytes each), and the load flag at <c>0x801930EC</c>.
/// Phase 5's overrides may only go in if nothing but rendering reads them.
///
///     KF2_LIGHTCENSUS=1     on from boot; a report every 5 s
///
/// Attribution uses the runtime's RAM logger, which stamps every byte read or
/// written through the slow path with a cycle while <c>TrackReads</c> is set: at
/// each owner's entry and exit the bytes stamped since the last event are credited
/// to the innermost owner running, and the cycle is stepped. Owners are the
/// thirteen stages and the modal loops and helpers that reach the records from
/// outside stage 13. Every read goes the slow way while this is on, so it is slow.
/// See "The light records are read only by the renderer" in docs/REMASTER.md.
/// </summary>
public static class LightCensus
{
    const uint Flag = 0x801930EC, Dst = 0x801930F0, Src = 0x800679A0;
    const uint DstStride = 0x68, SrcStride = 0x2C;
    const int Records = 80;

    static readonly (uint Addr, string Label)[] Owners =
    [
        (0x8002C944, "stage 1 (the copy)"),
        (0x80037C0C, "stage 2: object state machine"),
        (0x8002A550, "stage 3: player, pad, menu"),
        (0x80040348, "stage 4: entity table"),
        (0x80046A60, "stage 5: effects and projectiles"),
        (0x8004910C, "stage 6: area module per-frame"),
        (0x8001689C, "stage 7: area loader"),
        (0x80025A1C, "stage 8"),
        (0x800140AC, "stage 9"),
        (0x8002CA74, "stage 10"),
        (0x80016FC8, "stage 11"),
        (0x80014534, "stage 12"),
        (0x800342D8, "stage 13: renderer"),
        (0x8002CAF4, "func_8002CAF4 (rotates a record)"),
        (0x8002CBD4, "func_8002CBD4 (re-derives the records)"),
        (0x80043388, "func_80043388 (NPC conversation loop)"),
        (0x80015DD4, "func_80015DD4 (area setup)"),
        (0x8002DC78, "stage 13 / func_8002DC78 (animated textures)"),
        (0x80033FBC, "stage 13 / func_80033FBC (fade stepper)"),
        (0x80031C94, "stage 13 / func_80031C94 (tile walk)"),
        (0x800331B4, "stage 13 / func_800331B4 (model walk)"),
        (0x80032400, "stage 13 / func_80032400 (the arm)"),
        (0x80031D5C, "stage 13 / func_80031D5C (the HUD)"),
        (0x8002E0FC, "stage 13 / func_8002E0FC (present)"),
    ];

    sealed class Tally
    {
        public long DstRead, DstWritten, SrcRead, SrcWritten, FlagRead, FlagWritten;
        public readonly bool[] Recs = new bool[LightCensus.Records];
        public readonly bool[] Fields = new bool[DstStride];
        public readonly bool[] SrcFields = new bool[SrcStride];
        public readonly bool[] WrittenRecords = new bool[LightCensus.Records];
        public readonly bool[] DstWrittenRecs = new bool[LightCensus.Records];
        public readonly bool[] DstWrittenFields = new bool[DstStride];
    }

    // Index Owners.Length is "no owner": main-loop glue, boot, the host.
    static readonly Tally[] _tally = new Tally[Owners.Length + 1];
    static readonly int[] _stack = new int[64];
    static int _depth;
    static uint _mark;
    static bool _on;
    static readonly Stopwatch _clock = Stopwatch.StartNew();
    static double _reportAt = 5;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.remaster.lightcensus",
        Name = "Light record census",
        Version = "1.0",
        Description = "Which code reads and writes the area's light records.",
    };

    public static void Configure(string? on) => _on = on?.Trim() is not (null or "" or "0");

    public static void Install()
    {
        if (!_on) return;
        for (int i = 0; i < _tally.Length; i++) _tally[i] = new Tally();
        HookAttach.OnOverlayLoad("light census", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var targets = new List<(int, MethodInfo)>();
        for (int i = 0; i < Owners.Length; i++)
        {
            var t = SymbolRegistry.Resolve("game", null, Owners[i].Addr);
            if (t == null) continue;
            var (pre, post) = Pair(i);
            HookManager.AddPre(_self, t, pre, int.MinValue);
            HookManager.AddPost(_self, t, post, int.MaxValue);
            targets.Add((i, t));
        }
        HookManager.Commit();
        int ok = targets.Count(t => HookAttach.Installed(t.Item2));
        Console.WriteLine($"[lightcensus] {ok}/{Owners.Length} owners hooked; every RAM access now takes the slow path");
        return ok == Owners.Length;
    }

    // HookManager takes a MethodInfo with no closure, so each owner has its own pair.
    static (MethodInfo Enter, MethodInfo Exit) Pair(int id)
    {
        var t = typeof(LightCensus).GetNestedType($"O{id}", BindingFlags.NonPublic)!;
        return (t.GetMethod("Enter", BindingFlags.Public | BindingFlags.Static)!,
                t.GetMethod("Exit", BindingFlags.Public | BindingFlags.Static)!);
    }

    static void Enter(int id)
    {
        Harvest();
        if (_depth < _stack.Length) _stack[_depth] = id;
        _depth++;
    }

    static void Exit(int id)
    {
        Harvest();
        if (_depth > 0) _depth--;
        Report();
    }

    static int Top => _depth == 0 ? Owners.Length : _stack[Math.Min(_depth, _stack.Length) - 1];

    /// <summary>Credit everything stamped since the last event to the owner running,
    /// then step the cycle so the next interval starts clean. The host resets
    /// <c>TrackReads</c> from its panels at every present, so it is set again here.</summary>
    static void Harvest()
    {
        var log = RecompOne.Runtime.Runtime.RamLog;
        uint now = log.Cycle;
        var t = _tally[Top];

        for (uint o = 0; o < 4; o++)
        {
            if (log.GetReadStamp((int)((Flag + o) & 0x1FFFFF)) > _mark) t.FlagRead++;
            if (log.GetWriteStamp((int)((Flag + o) & 0x1FFFFF)) > _mark) t.FlagWritten++;
        }
        for (uint o = 0; o < Records * DstStride; o++)
        {
            int p = (int)((Dst + o) & 0x1FFFFF);
            if (log.GetReadStamp(p) > _mark)
            {
                t.DstRead++;
                t.Recs[o / DstStride] = true;
                t.Fields[o % DstStride] = true;
            }
            if (log.GetWriteStamp(p) > _mark)
            {
                t.DstWritten++;
                t.DstWrittenRecs[o / DstStride] = true;
                t.DstWrittenFields[o % DstStride] = true;
            }
        }
        for (uint o = 0; o < Records * SrcStride; o++)
        {
            int p = (int)((Src + o) & 0x1FFFFF);
            if (log.GetReadStamp(p) > _mark) t.SrcRead++;
            if (log.GetWriteStamp(p) > _mark)
            {
                t.SrcWritten++;
                t.WrittenRecords[o / SrcStride] = true;
                t.SrcFields[o % SrcStride] = true;
            }
        }

        log.Tick();
        _mark = now;
        RamLogger.TrackReads = RamLogger.TrackWrites = true;
    }

    static void Report()
    {
        double s = _clock.Elapsed.TotalSeconds;
        if (s < _reportAt) return;
        _reportAt = s + 5;
        var sb = new StringBuilder();
        sb.Append($"[lightcensus] {s:0}s, bytes touched per interval, summed:\n");
        for (int i = 0; i < _tally.Length; i++)
        {
            var t = _tally[i];
            if (t.DstRead + t.DstWritten + t.SrcRead + t.SrcWritten + t.FlagRead + t.FlagWritten == 0) continue;
            string who = i < Owners.Length ? Owners[i].Label : "(no owner)";
            sb.Append($"[lightcensus]   {who}: records read {t.DstRead} written {t.DstWritten}; " +
                      $"source read {t.SrcRead} written {t.SrcWritten}; flag read {t.FlagRead} written {t.FlagWritten}");
            if (t.DstRead > 0) sb.Append($"; read recs {Ranges(t.Recs)} fields {Ranges(t.Fields, hex: true)}");
            if (t.DstWritten > 0 && i != 0) sb.Append($"; wrote recs {Ranges(t.DstWrittenRecs)} fields {Ranges(t.DstWrittenFields, hex: true)}");
            if (t.SrcWritten > 0) sb.Append($"; wrote source recs {Ranges(t.WrittenRecords)} fields {Ranges(t.SrcFields, hex: true)}");
            sb.Append('\n');
        }
        Console.Write(sb.ToString());
    }

    static string Ranges(bool[] set, bool hex = false)
    {
        var sb = new StringBuilder();
        for (int i = 0; i < set.Length; i++)
        {
            if (!set[i]) continue;
            int j = i;
            while (j + 1 < set.Length && set[j + 1]) j++;
            if (sb.Length > 0) sb.Append(',');
            string F(int v) => hex ? $"{v:X2}" : $"{v}";
            sb.Append(i == j ? F(i) : $"{F(i)}-{F(j)}");
            i = j;
        }
        return sb.Length == 0 ? "-" : sb.ToString();
    }

    // One static pair per owner, since a hook is a MethodInfo with no closure.
    static class O0 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(0); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(0); }
    static class O1 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(1); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(1); }
    static class O2 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(2); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(2); }
    static class O3 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(3); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(3); }
    static class O4 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(4); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(4); }
    static class O5 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(5); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(5); }
    static class O6 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(6); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(6); }
    static class O7 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(7); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(7); }
    static class O8 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(8); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(8); }
    static class O9 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(9); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(9); }
    static class O10 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(10); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(10); }
    static class O11 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(11); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(11); }
    static class O12 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(12); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(12); }
    static class O13 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(13); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(13); }
    static class O14 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(14); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(14); }
    static class O15 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(15); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(15); }
    static class O16 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(16); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(16); }
    static class O17 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(17); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(17); }
    static class O18 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(18); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(18); }
    static class O19 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(19); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(19); }
    static class O20 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(20); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(20); }
    static class O21 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(21); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(21); }
    static class O22 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(22); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(22); }
    static class O23 { public static void Enter(CpuContext c, IMemory m) => LightCensus.Enter(23); public static void Exit(CpuContext c, IMemory m) => LightCensus.Exit(23); }
}
