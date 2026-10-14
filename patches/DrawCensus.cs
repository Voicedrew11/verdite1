using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// KF1_DRAWCENSUS=1: which routine in the renderer draws how much of the frame, in
/// bytes of primitive buffer per frame, exclusive (not counting what it calls) and
/// inclusive, every two seconds. A measurement only; it never writes to the game.
/// See "The renderer in C#" in docs/GAME_INTERNALS.md.
///
/// The frame's packets are written into one buffer, and the packet record at
/// <c>0x80070EB8</c> points at its <c>{ start, end, cursor }</c>. Each censused routine
/// is two hooks: its cursor on entry and on exit, so the bytes it wrote are the bump
/// between them. Nesting goes through a stack, so a writer called from a site is
/// charged to the writer, and the site gets only what it wrote itself.
///
/// Frames are counted at the renderer's exit, which the modal loops also reach (the
/// area load and the fades), so a frame here is a renderer call.
/// </summary>
public static class DrawCensus
{
    const uint Record = 0x80070EB8;

    // The renderer's callees and the writers they reach, in no particular order: the
    // stack sorts out the nesting.
    static readonly (uint Addr, string What)[] Routines =
    [
        (0x8001FDE4, "renderer"),
        (0x8001E83C, "map walk"),
        (0x8001E5EC, "map row"),
        (0x8001DE18, "map writer"),
        (0x8001F218, "models"),
        (0x8001D730, "object writer"),
        (0x8001C7F8, "submit writer"),
        (0x8001F8B0, "compass"),
        (0x8001F9D4, "HUD sprites"),
        (0x8001E480, "HUD sprite writer"),
        (0x8001FAFC, "selected item"),
        (0x8001E230, "sprite writer"),
        (0x8001F798, "arm"),
    ];

    static bool _on;
    static long[] _exclusive = [], _inclusive = [], _calls = [];

    // The censused routines entered and not yet left: which routine, the cursor on entry,
    // and how many bytes its callees have already been charged.
    static readonly (int Slot, uint Record, uint Entry, long Children)[] _stack =
        new (int, uint, uint, long)[32];
    static int _depth;

    static double _windowStart;
    static long _frames;

    static double Now => Environment.TickCount64 / 1000.0;

    static readonly ModInfo _self = new()
    {
        Id = "kf1.drawcensus",
        Name = "Draw census",
        Version = "1.0",
        Description = "Attributes the frame's primitives to the routine that drew them.",
    };

    public static void Configure(string? on) => _on = on?.Trim() == "1";

    public static void Install()
    {
        if (!_on) return;
        _exclusive = new long[Routines.Length];
        _inclusive = new long[Routines.Length];
        _calls = new long[Routines.Length];
        _windowStart = Now;
        HookAttach.OnOverlayLoad("census", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        int n = 0;
        for (int i = 0; i < Routines.Length; i++)
        {
            var target = SymbolRegistry.Resolve("game", null, Routines[i].Addr);
            if (target == null)
            {
                Console.Error.WriteLine($"[KF1] census: no game/0x{Routines[i].Addr:X8} ({Routines[i].What})");
                continue;
            }
            // A pre-hook gets only the CpuContext, so the slot is baked in per routine.
            // The post goes first: an orphan post is harmless, an orphan pre is not.
            var slot = typeof(Slot<>).MakeGenericType(Markers[i]);
            slot.GetField(nameof(Slot<int>.Id))!.SetValue(null, i);
            if (HookManager.AddPost(_self, target, slot.GetMethod(nameof(Slot<int>.Post))!) &&
                HookManager.AddPre(_self, target, slot.GetMethod(nameof(Slot<int>.Pre))!)) n++;
        }
        HookManager.Commit();
        Console.WriteLine($"[KF1] census: {n} of {Routines.Length} routine(s) attributed");
        return n == Routines.Length;
    }

    // This frame's record: the descriptor it points at, its start, and its cursor.
    static (uint Rec, uint Start, uint Cur) Arena(IMemory m)
    {
        uint rec = m.ReadU32(Record);
        if (rec < 0x80000000u) return (0u, 0u, 0u);
        uint start = m.ReadU32(rec), end = m.ReadU32(rec + 4u);
        return start == 0 || end <= start ? (0u, 0u, 0u) : (rec, start, m.ReadU32(rec + 8u));
    }

    static void Enter(int slot, IMemory m)
    {
        if (_depth >= _stack.Length) { _depth++; return; }
        var a = Arena(m);
        _stack[_depth++] = (slot, a.Rec, a.Cur, 0L);
    }

    static void Leave(int slot, IMemory m)
    {
        if (_depth <= 0) return;
        if (_depth > _stack.Length) { _depth--; return; }
        var frame = _stack[--_depth];

        // If the record moved across the call, the cursors are in different buffers, so
        // the honest figure is the whole of the buffer now in use.
        var (rec, start, now) = Arena(m);
        long total = now == 0 ? 0L
                   : rec != frame.Record || now < frame.Entry ? now - start
                   : now - frame.Entry;

        _inclusive[frame.Slot] += total;
        _exclusive[frame.Slot] += Math.Max(0L, total - frame.Children);
        _calls[frame.Slot]++;

        if (_depth > 0 && _depth <= _stack.Length)
            _stack[_depth - 1].Children += total;

        // Report on the renderer's exit, so a window never cuts a frame in half.
        if (frame.Slot == 0) { _frames++; Report(); }
    }

    static void Report()
    {
        double now = Now;
        if (now - _windowStart < 2.0) return;
        if (_frames == 0) { Array.Clear(_calls); _windowStart = now; return; }

        double secs = now - _windowStart;
        Console.WriteLine($"[KF1] census: {_frames} frames in {secs:0.0}s ({_frames / secs:0.0}/s), bytes per frame:");
        for (int i = 0; i < Routines.Length; i++)
        {
            if (_calls[i] == 0) continue;
            double excl = (double)_exclusive[i] / _frames;
            double incl = (double)_inclusive[i] / _frames;
            Console.WriteLine($"[KF1] census:   {Routines[i].What,-20} {excl,8:0} excl  {incl,8:0} incl  " +
                              $"({(double)_calls[i] / _frames:0.0} call/frame)");
        }

        Array.Clear(_exclusive);
        Array.Clear(_inclusive);
        Array.Clear(_calls);
        _frames = 0;
        _windowStart = now;
    }

    // One distinct pair of methods per routine, so the same delegate is not shared.
    struct M0; struct M1; struct M2; struct M3; struct M4; struct M5; struct M6;
    struct M7; struct M8; struct M9; struct M10; struct M11; struct M12;

    static readonly Type[] Markers =
    [
        typeof(M0), typeof(M1), typeof(M2), typeof(M3), typeof(M4), typeof(M5), typeof(M6),
        typeof(M7), typeof(M8), typeof(M9), typeof(M10), typeof(M11), typeof(M12),
    ];

    static class Slot<T>
    {
        public static int Id;
        public static void Pre(CpuContext c, IMemory m) => Enter(Id, m);
        public static void Post(CpuContext c, IMemory m) => Leave(Id, m);
    }
}
