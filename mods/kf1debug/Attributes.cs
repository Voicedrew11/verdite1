// ModCompiler compiles mods with no implicit usings, so every namespace the
// file needs must be named here -- including System.
using System;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;
using KingsField1 = Recompiled.KingsField1_game;

namespace Kf1.Mods.Debug;

/// <summary>
/// Editing the character: experience, level, vitals, gold, STR and MAG, the
/// conditions, and the ratings the equipment gives.
///
/// The character (EXP, level, HP/MP and their maxima, gold, base STR and MAG,
/// the practice counters, the condition bits and timers) is plain memory and a
/// write sticks. The adjusted STR and MAG and the eleven ratings are a cache:
/// func_80015714 resets them, copies base STR and MAG in (STR -20 while
/// poisoned) and sums the weapon and armour records, and the equip setters and
/// the level-up call it. They are held by a post-hook on it when locked.
///
/// "Level up" runs func_80016058(s16 gain), the game's own: EXP += gain, and if
/// EXP reaches the threshold, one level, with max HP, max MP, STR, MAG and the
/// next threshold from the 12-byte table at 0x800650B0 (levels 1..39; past that
/// it repeats the last step). HP and MP are not refilled.
/// </summary>
internal static class Attributes
{
    internal const int ExpMax = 99_999;
    internal const int LevelMax = 0xFF;

    internal const uint Gold      = 0x800A07AC;   // u32
    internal const uint StrPractice = 0x800A079E; // u16, STR +1 at 100 (func_80015F28)
    internal const uint MagPractice = 0x800A07A0; // u16, MAG +1 at 100 (func_80015FC0)
    internal const uint BaseStr   = 0x800A07A2;   // u16
    internal const uint BaseMag   = 0x800A07A4;   // u16
    internal const uint AdjStr    = 0x800A07A6;   // u16, derived
    internal const uint AdjMag    = 0x800A07A8;   // u16, derived
    internal const uint Condition = 0x800A07AA;   // u16 bits: 0 poison, 3 slowed
    internal const uint WeaponCharge = 0x800A0798; // u16, 5000 full
    internal const uint MagicCharge  = 0x800A079C; // u16, 5000 full; a cast needs 5000

    // The recompute's two blocks, meanings unresolved.
    internal const uint RatingA = 0x800A07B0;     // 5 x u16
    internal const uint RatingB = 0x800A07BC;     // 6 x u16

    // Six s16 condition and effect timers, -1 off. 0x7C8 is poison (600 when a
    // hit poisons); 0x7CA is set by hit flag bit 2; the rest are unread.
    internal const uint Timers = 0x800A07C8;

    internal const uint RecomputeRoutine = 0x80015714;
    internal const uint LevelUpRoutine   = 0x80016058;

    internal readonly struct Field
    {
        public readonly string Name;
        public readonly uint Address;
        public readonly int Width;
        public readonly bool Signed;
        public readonly string Tip;

        public Field(string name, uint address, int width, string tip = "", bool signed = false)
        {
            Name = name; Address = address; Width = width; Tip = tip; Signed = signed;
        }
    }

    internal static readonly Field[] Progress =
    [
        new("Experience", GameState.Exp, 4, "Capped at 99999 by the game's own level-up routine."),
        new("Next level at", GameState.ExpNext, 4, "func_80016058 compares against this word and "
                                                + "rewrites it from the game's table each level."),
        new("Level", GameState.Level, 1, "The byte alone. Typing a number here does NOT grant the "
                                       + "HP, MP, STR and MAG a level gives -- use Level up for that."),
    ];

    internal static readonly Field[] Vitals =
    [
        new("HP", GameState.Hp, 2),
        new("Max HP", GameState.MaxHp, 2, "Zero here is how this mod tells \"no character\" from "
                                        + "\"a character at 0 HP\". The level-up caps it at 9999."),
        new("MP", GameState.Mp, 2),
        new("Max MP", GameState.MaxMp, 2, "The level-up caps it at 9999."),
        new("Gold", Gold, 4, "150 at a New Game."),
    ];

    internal static readonly Field[] BaseAttributes =
    [
        new("STR", BaseStr, 2, "Strength. Raised by the level table and by STR practice; the level-up "
                             + "caps it at 999. The status screen shows it as a class: under 40, under 60, above."),
        new("MAG", BaseMag, 2, "Magic power. Raised by the level table and by MAG practice; capped at 999."),
        new("STR practice", StrPractice, 2, "Counts toward the next STR point; +1 STR at 100."),
        new("MAG practice", MagPractice, 2, "Counts toward the next MAG point; +1 MAG at 100."),
        new("Weapon charge", WeaponCharge, 2, "The swing gauge; 5000 is full."),
        new("Magic charge", MagicCharge, 2, "The cast gauge; a cast needs 5000."),
    ];

    internal static readonly Field[] Conditions =
    [
        new("Condition bits", Condition, 2, "Bit 0 poison, bit 3 slows the walk to 36 and the turn "
                                          + "to 5. The game's full heal clears bits 0..3."),
        new("Timer 0 (poison)", Timers + 0, 2, "-1 off; a poisoning hit sets 600.", true),
        new("Timer 1", Timers + 2, 2, "-1 off; set by a hit's condition bit 2.", true),
        new("Timer 2", Timers + 4, 2, "-1 off. Meaning unresolved.", true),
        new("Timer 3", Timers + 6, 2, "-1 off. Meaning unresolved.", true),
        new("Timer 4", Timers + 8, 2, "-1 off. Meaning unresolved.", true),
        new("Timer 5", Timers + 10, 2, "-1 off. Meaning unresolved.", true),
    ];

    internal static readonly Field[] Adjusted =
    [
        new("Adj STR", AdjStr, 2, "Base STR, -20 while poisoned, plus equipment."),
        new("Adj MAG", AdjMag, 2, "Base MAG plus equipment."),
    ];

    internal static readonly Field[] RatingsA =
    [
        new("Rating A1", RatingA + 0, 2), new("Rating A2", RatingA + 2, 2), new("Rating A3", RatingA + 4, 2),
        new("Rating A4", RatingA + 6, 2), new("Rating A5", RatingA + 8, 2),
    ];

    internal static readonly Field[] RatingsB =
    [
        new("Rating B1", RatingB + 0, 2), new("Rating B2", RatingB + 2, 2), new("Rating B3", RatingB + 4, 2),
        new("Rating B4", RatingB + 6, 2), new("Rating B5", RatingB + 8, 2), new("Rating B6", RatingB + 10, 2),
    ];

    internal static readonly Field[] Derived = [.. Adjusted, .. RatingsA, .. RatingsB];

    internal static bool LockDerived;
    internal static readonly int[] Held = new int[Derived.Length];

    internal static int Read(IMemory m, in Field f) => f.Width switch
    {
        1 => m.ReadU8(f.Address),
        2 => f.Signed ? (short)m.ReadU16(f.Address) : m.ReadU16(f.Address),
        _ => (int)m.ReadU32(f.Address),
    };

    internal static void Write(IMemory m, in Field f, int value)
    {
        switch (f.Width)
        {
            case 1: m.WriteU8(f.Address, (byte)Math.Clamp(value, 0, 255)); break;
            case 2:
                m.WriteU16(f.Address, f.Signed
                    ? (ushort)(short)Math.Clamp(value, short.MinValue, short.MaxValue)
                    : (ushort)Math.Clamp(value, 0, ushort.MaxValue));
                break;
            default: m.WriteU32(f.Address, (uint)Math.Max(value, 0)); break;
        }
    }

    internal static int HeldIndex(uint address)
    {
        for (int i = 0; i < Derived.Length; i++)
            if (Derived[i].Address == address) return i;
        return -1;
    }

    internal static void PrimeHold(IMemory m)
    {
        for (int i = 0; i < Derived.Length; i++) Held[i] = Read(m, Derived[i]);
    }

    // ---- quick actions that are only memory ----

    internal static void FullHeal(IMemory m)
    {
        m.WriteU16(GameState.Hp, m.ReadU16(GameState.MaxHp));
        m.WriteU16(GameState.Mp, m.ReadU16(GameState.MaxMp));
    }

    /// <summary>What the game's own full heal (func_80033E10) does to the
    /// condition, plus the poison timer stopped.</summary>
    internal static void CureConditions(IMemory m)
    {
        m.WriteU16(Condition, (ushort)(m.ReadU16(Condition) & 0xFFF0));
        GameState.WriteS16(m, Timers, -1);
    }

    // ---- quick actions that run the game's own code, on the game thread ----

    static int _pendingLevelUps;
    static bool _pendingRecalc;

    internal static string Status = "";

    internal static void QueueLevelUp(int times = 1)
    {
        _pendingLevelUps += Math.Max(times, 0);
        Status = $"queued {_pendingLevelUps} level-up(s)";
    }

    internal static void QueueRecalculate()
    {
        _pendingRecalc = true;
        Status = "queued a recalculation";
    }

    [PostHook("game", Address = GameState.PlayerStage)]
    static void AfterPlayerStage(CpuContext c, IMemory m)
    {
        if (!GameState.IsInGame(m)) return;
        if (_pendingLevelUps > 0) RunLevelUps(c, m);
        if (_pendingRecalc) RunRecalculate(c, m);
        if (LockDerived) ApplyHold(m);
    }

    [PostHook("game", Address = RecomputeRoutine)]
    static void AfterRecalculate(CpuContext c, IMemory m)
    {
        if (LockDerived && GameState.IsInGame(m)) ApplyHold(m);
    }

    static void ApplyHold(IMemory m)
    {
        for (int i = 0; i < Derived.Length; i++) Write(m, Derived[i], Held[i]);
    }

    static void RunLevelUps(CpuContext c, IMemory m)
    {
        int want = _pendingLevelUps;
        _pendingLevelUps = 0;

        var saved = c.Snapshot();
        int before = m.ReadU8(GameState.Level);

        for (int i = 0; i < want; i++)
        {
            int level = m.ReadU8(GameState.Level);
            if (level >= LevelMax) break;

            // One call levels once at most: top EXP up to the threshold and pass 0.
            uint need = m.ReadU32(GameState.ExpNext);
            if (need > ExpMax) break;
            if (m.ReadU32(GameState.Exp) < need) m.WriteU32(GameState.Exp, need);

            c.A0 = 0u;
            KingsField1.func_80016058(c, m);
            if (m.ReadU8(GameState.Level) == level) break;
        }

        c.Restore(saved);

        int after = m.ReadU8(GameState.Level);
        Status = after == before
            ? $"no level gained (level {after}; the next threshold is past the EXP cap of {ExpMax})"
            : $"level {before} -> {after}";
        Console.WriteLine($"[kf1debug] {Status}");
    }

    static void RunRecalculate(CpuContext c, IMemory m)
    {
        _pendingRecalc = false;
        var saved = c.Snapshot();
        KingsField1.func_80015714(c, m);
        c.Restore(saved);
        Status = $"recalculated: adj STR {m.ReadU16(AdjStr)}, adj MAG {m.ReadU16(AdjMag)}";
    }

    internal static void Reset()
    {
        LockDerived = false;
        _pendingLevelUps = 0;
        _pendingRecalc = false;
        Status = "";
    }
}
