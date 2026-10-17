// ModCompiler compiles mods with no implicit usings, so every namespace the
// file needs must be named here -- including System.
using System;
using System.Collections.Generic;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;
using KingsField1 = Recompiled.KingsField1_game;

namespace Kf1.Mods.Debug;

/// <summary>
/// The spell book: learn, forget, and the held spell.
///
/// The player's spells are records 0..8 of the 20-byte table at 0x8009CE60
/// (records 9 and on have no cost and are not the player's). Byte +0 is 1 when
/// known; u16 +0x10 is the MP cost. The magic menu (func_8002317C) lists records
/// 0..3 to cast from the menu; the equip menu lists 4..8 to hold. A New Game
/// knows and holds record 8.
///
/// The held spell is 0x800A07DC, 0xFF none, set by func_800167E4(id), which also
/// empties the magic charge and points 0x800A07E0 at the record. Holding one is
/// that call, queued to the game thread; learning is the byte, as the game's own
/// learn (func_80034A80, record 0) writes it.
/// </summary>
internal static class Magic
{
    internal const uint RecordBase = 0x8009CE60;
    internal const int RecordStride = 20;
    internal const int Count = 9;
    internal const int KnownOffset = 0x00;
    internal const int CostOffset = 0x10;
    internal const int FirstHoldable = 4;

    internal const uint HeldSpell = 0x800A07DC;
    internal const int NoSpell = 0xFF;

    static uint Record(int i) => RecordBase + (uint)i * RecordStride;

    internal static bool Known(IMemory m, int i) =>
        (uint)i < Count && m.ReadU8(Record(i) + KnownOffset) != 0;

    internal static int Cost(IMemory m, int i) =>
        (uint)i < Count ? m.ReadU16(Record(i) + CostOffset) : 0;

    /// <summary>The names are glyph codes the mod cannot render; the id and the
    /// menu it appears in are what can be shown.</summary>
    internal static string Name(int i) => i < FirstHoldable ? $"spell {i} (menu)" : $"spell {i} (held)";

    internal static int HeldId(IMemory m) => m.ReadU8(HeldSpell);

    internal static string Held(IMemory m)
    {
        int id = HeldId(m);
        return id == NoSpell ? "none" : (uint)id < Count ? Name(id) : $"record {id}";
    }

    readonly struct Request
    {
        public readonly int Id;
        public readonly int Kind;   // 0 forget, 1 learn, 2 hold
        public Request(int id, int kind) { Id = id; Kind = kind; }
    }

    static readonly List<Request> _pending = [];
    static readonly object _gate = new();

    internal static string Status = "";

    static void Queue(int id, int kind)
    {
        lock (_gate) _pending.Add(new Request(id, kind));
        Status = "spell change queued";
    }

    internal static void QueueLearn(int id) { if ((uint)id < Count) Queue(id, 1); }
    internal static void QueueForget(int id) { if ((uint)id < Count) Queue(id, 0); }

    /// <summary>Hold a spell from records 4..8, or none with 0xFF.</summary>
    internal static void QueueHold(int id)
    {
        if (id != NoSpell && (id < FirstHoldable || id >= Count)) return;
        Queue(id, 2);
    }

    internal static void QueueLearnAll()
    {
        for (int i = 0; i < Count; i++) Queue(i, 1);
        Status = $"queued all {Count} spells";
    }

    [PostHook("game", Address = GameState.PlayerStage)]
    static void AfterPlayerStage(CpuContext c, IMemory m)
    {
        if (!GameState.IsInGame(m)) return;
        RunProbe(m);

        Request[] work;
        lock (_gate)
        {
            if (_pending.Count == 0) return;
            work = _pending.ToArray();
            _pending.Clear();
        }

        int learned = 0, forgotten = 0, held = 0, unchanged = 0;
        foreach (var r in work)
        {
            if (r.Kind == 2)
            {
                var saved = c.Snapshot();
                c.A0 = (uint)r.Id;
                KingsField1.func_800167E4(c, m);
                c.Restore(saved);
                held++;
                continue;
            }
            uint a = Record(r.Id) + KnownOffset;
            bool known = m.ReadU8(a) != 0;
            if (known == (r.Kind == 1)) { unchanged++; continue; }
            m.WriteU8(a, (byte)r.Kind);
            if (r.Kind == 1) learned++; else forgotten++;
        }

        Status = $"learned {learned}, forgot {forgotten}" + (held != 0 ? $", held {Held(m)}" : "");
        Console.WriteLine($"[kf1debug] {Status}" + (unchanged != 0 ? $" ({unchanged} already so)" : ""));
    }

    // ---- the probe: KF1_DEBUG_MAGIC_PROBE=1 prints the book once; 2 also learns all ----

    static bool _probeDone;

    internal static readonly int ProbeLevel =
        int.TryParse(Environment.GetEnvironmentVariable("KF1_DEBUG_MAGIC_PROBE"), out int lv) ? lv : 0;

    static void RunProbe(IMemory m)
    {
        if (_probeDone || ProbeLevel <= 0) return;
        _probeDone = true;
        Console.WriteLine($"[kf1debug] spells at 0x{RecordBase:X8}, {Count} records");
        for (int i = 0; i < Count; i++)
            Console.WriteLine($"[kf1debug]   {i}  {(Known(m, i) ? "known" : "     ")}  {Cost(m, i),3} mp  {Name(i)}");
        Console.WriteLine($"[kf1debug] held: {Held(m)} (0x{HeldId(m):X2})");
        if (ProbeLevel >= 2) QueueLearnAll();
    }

    internal static void Reset()
    {
        lock (_gate) _pending.Clear();
        _probeDone = false;
        Status = "";
    }
}
