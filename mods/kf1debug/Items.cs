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
/// The inventory and the equipment slots.
///
/// The inventory is one count byte per item id at 0x800652A8, ids 0..79; the
/// shop refuses to sell past 99. The bytes after it (80 per shop) are the shops'
/// stock and are not touched. No "give item" routine has been found, so a give
/// is a write of the count byte, capped at 99 as the shop caps it.
///
/// Names are at 0x80058E60 + 20 * id, but as ten u16 glyph-fragment codes, not
/// text, so ids are shown. An id whose record opens with two 0xFFFF codes is not
/// used.
///
/// Equipping runs the game's setters, which both end in the recompute
/// func_80015714: func_80016A30(id) for the weapon, func_80016848(id, slot) for
/// the six armour slots. The id ranges per slot are the equip menu's
/// (func_800238D8, tables 0x80012310/0x80012330). The menu also refuses slots
/// 2 and 3 while slot 1 holds id 21, which clears them; that rule is kept.
/// </summary>
internal static class Items
{
    internal const uint InvBase = 0x800652A8;
    internal const int Count = 80;
    internal const int MaxHeld = 99;

    internal const uint NameTable = 0x80058E60;
    internal const int NameStride = 20;

    internal static int Held(IMemory m, int id) =>
        (uint)id < Count ? m.ReadU8(InvBase + (uint)id) : 0;

    internal static bool Has(IMemory m, int id) => Held(m, id) != 0;

    internal static void SetHeld(IMemory m, int id, int count)
    {
        if ((uint)id >= Count) return;
        m.WriteU8(InvBase + (uint)id, (byte)Math.Clamp(count, 0, 255));
    }

    internal static int DistinctHeld(IMemory m)
    {
        int n = 0;
        for (int i = 0; i < Count; i++) if (Has(m, i)) n++;
        return n;
    }

    internal static bool IsUnused(IMemory m, int id)
    {
        if ((uint)id >= Count) return true;
        uint rec = NameTable + (uint)id * NameStride;
        return m.ReadU16(rec) == 0xFFFF && m.ReadU16(rec + 2) == 0xFFFF;
    }

    internal static string Label(IMemory m, int id) => $"item {id}";

    // ---- the categories, from the equip menu's ranges ----

    internal readonly struct Group
    {
        public readonly string Name;
        public readonly int First;
        public readonly int Last;
        public Group(string name, int first, int last) { Name = name; First = first; Last = last; }
    }

    static readonly Group[] _groups =
    [
        new Group("Weapons",            0, 12),
        new Group("Armour slot 0",     13, 18),
        new Group("Armour slot 1",     19, 25),
        new Group("Armour slot 4",     26, 31),
        new Group("Armour slot 2",     32, 34),
        new Group("Armour slot 3",     35, 38),
        new Group("Other",             39, 47),
        new Group("Armour slot 5",     48, 51),
        new Group("Items and keys",    52, 79),
    ];

    internal static Group[] Groups(IMemory m) => _groups;

    // ---- equipment ----

    /// <summary>The weapon is -1; 0..5 are func_80016848's slot argument.
    /// Which body part each is has not been read off the screen.</summary>
    internal enum EquipSlot { Weapon = -1, Slot0 = 0, Slot1 = 1, Slot2 = 2, Slot3 = 3, Slot4 = 4, Slot5 = 5 }

    internal const uint WeaponSlot = 0x800A07E4;
    internal const int EmptySlot = 0xFF;

    internal static readonly EquipSlot[] EquipSlots =
    [
        EquipSlot.Weapon, EquipSlot.Slot0, EquipSlot.Slot1, EquipSlot.Slot2,
        EquipSlot.Slot3, EquipSlot.Slot4, EquipSlot.Slot5,
    ];

    // func_80016848's jump table at 0x80012030: slot n stores 0x810 + this.
    internal static uint SlotAddress(EquipSlot slot) => slot switch
    {
        EquipSlot.Weapon => WeaponSlot,
        EquipSlot.Slot0  => 0x800A0810,
        EquipSlot.Slot1  => 0x800A0811,
        EquipSlot.Slot2  => 0x800A0813,
        EquipSlot.Slot3  => 0x800A0814,
        EquipSlot.Slot4  => 0x800A0812,
        EquipSlot.Slot5  => 0x800A0815,
        _ => 0,
    };

    internal static string SlotName(EquipSlot slot) =>
        slot == EquipSlot.Weapon ? "Weapon" : $"Armour {(int)slot}";

    internal static (int First, int Last) SlotRange(EquipSlot slot) => slot switch
    {
        EquipSlot.Weapon => (0, 12),
        EquipSlot.Slot0  => (13, 18),
        EquipSlot.Slot1  => (19, 25),
        EquipSlot.Slot2  => (32, 34),
        EquipSlot.Slot3  => (35, 38),
        EquipSlot.Slot4  => (26, 31),
        EquipSlot.Slot5  => (48, 51),
        _ => (1, 0),
    };

    internal static int Equipped(IMemory m, EquipSlot slot)
    {
        uint a = SlotAddress(slot);
        if (a == 0) return -1;
        int id = m.ReadU8(a);
        return id == EmptySlot ? -1 : id;
    }

    // ---- queued work, run on the game thread ----

    static readonly List<int> _pendingGive = [];
    static readonly List<int> _pendingRemove = [];
    static readonly List<(int Id, EquipSlot Slot)> _pendingEquip = [];
    static readonly object _gate = new();

    internal static string Status = "";

    internal static void QueueGive(int id, int times = 1)
    {
        if ((uint)id >= Count) return;
        lock (_gate) for (int i = 0; i < times; i++) _pendingGive.Add(id);
        Status = "queued a give";
    }

    internal static void QueueGiveAll(IMemory m)
    {
        int n = 0;
        lock (_gate)
            for (int id = 0; id < Count; id++)
            {
                if (IsUnused(m, id) || Has(m, id)) continue;
                _pendingGive.Add(id);
                n++;
            }
        Status = n == 0 ? "you already hold one of everything" : $"queued {n} item(s)";
    }

    internal static void QueueRemove(int id, int times = 1)
    {
        if ((uint)id >= Count) return;
        lock (_gate) for (int i = 0; i < times; i++) _pendingRemove.Add(id);
        Status = "queued a removal";
    }

    /// <summary>Queue an equip, or an unequip with id 0xFF.</summary>
    internal static void QueueEquip(int id, EquipSlot slot)
    {
        var (first, last) = SlotRange(slot);
        if (id != EmptySlot && (id < first || id > last))
        {
            Status = $"id {id} does not belong in {SlotName(slot)}";
            return;
        }
        lock (_gate) _pendingEquip.Add((id, slot));
        Status = $"queued {SlotName(slot)} = {(id == EmptySlot ? "empty" : $"id {id}")}";
    }

    internal static void ClearAll(IMemory m)
    {
        for (int id = 0; id < Count; id++) m.WriteU8(InvBase + (uint)id, 0);
        Status = "inventory cleared";
    }

    [PostHook("game", Address = GameState.PlayerStage)]
    static void AfterPlayerStage(CpuContext c, IMemory m)
    {
        if (!GameState.IsInGame(m)) return;
        RunProbe(m);

        int[] give, remove;
        (int Id, EquipSlot Slot)[] equip;
        lock (_gate)
        {
            if (_pendingGive.Count == 0 && _pendingRemove.Count == 0 && _pendingEquip.Count == 0) return;
            give = _pendingGive.ToArray(); remove = _pendingRemove.ToArray(); equip = _pendingEquip.ToArray();
            _pendingGive.Clear(); _pendingRemove.Clear(); _pendingEquip.Clear();
        }

        int given = 0, refused = 0, removed = 0, empty = 0, equipped = 0, blocked = 0;

        foreach (int id in give)
        {
            int n = Held(m, id);
            if (n < MaxHeld) { SetHeld(m, id, n + 1); given++; }
            else refused++;
        }

        foreach (int id in remove)
        {
            int n = Held(m, id);
            if (n > 0) { SetHeld(m, id, n - 1); removed++; }
            else empty++;
        }

        var saved = c.Snapshot();
        foreach (var (id, slot) in equip)
        {
            // The menu's rule: slot 1 = 21 occupies slots 2 and 3.
            if ((slot == EquipSlot.Slot2 || slot == EquipSlot.Slot3) && id != EmptySlot
                && m.ReadU8(SlotAddress(EquipSlot.Slot1)) == 21) { blocked++; continue; }

            c.A0 = (uint)id;
            if (slot == EquipSlot.Weapon) KingsField1.func_80016A30(c, m);
            else
            {
                c.A1 = (uint)(int)slot;
                KingsField1.func_80016848(c, m);
                if (slot == EquipSlot.Slot1 && id == 21)
                {
                    foreach (var s in new[] { 2u, 3u })
                    {
                        c.A0 = EmptySlot; c.A1 = s;
                        KingsField1.func_80016848(c, m);
                    }
                }
            }
            equipped++;
        }
        c.Restore(saved);

        Status = $"gave {given}, removed {removed}, equipped {equipped}"
               + (refused != 0 ? $"; {refused} already at {MaxHeld}" : "")
               + (empty != 0 ? $"; {empty} already none" : "")
               + (blocked != 0 ? $"; {blocked} refused (armour 1 holds id 21)" : "");
        Console.WriteLine($"[kf1debug] {Status}");
        if (ProbeLevel >= 2) ProbeAfterChange(m);
    }

    // ---- the probe: KF1_DEBUG_ITEMS_PROBE=1 prints the table once; 2 also gives one of each ----

    static bool _probeDone;

    internal static readonly int ProbeLevel =
        int.TryParse(Environment.GetEnvironmentVariable("KF1_DEBUG_ITEMS_PROBE"), out int lv) ? lv : 0;

    static void RunProbe(IMemory m)
    {
        if (_probeDone || ProbeLevel <= 0) return;
        _probeDone = true;

        Console.WriteLine($"[kf1debug] inventory at 0x{InvBase:X8}, {Count} ids");
        foreach (var g in _groups)
        {
            var used = new List<string>();
            for (int id = g.First; id <= g.Last; id++)
                if (!IsUnused(m, id)) used.Add(Has(m, id) ? $"{id}x{Held(m, id)}" : $"{id}");
            Console.WriteLine($"[kf1debug]   {g.First,2}-{g.Last,2} {g.Name,-15} {string.Join(' ', used)}");
        }
        Console.WriteLine($"[kf1debug] {DistinctHeld(m)} of {Count} ids held");
        foreach (var slot in EquipSlots)
            Console.WriteLine($"[kf1debug]   {SlotName(slot),-9} {(Equipped(m, slot) is var id && id >= 0 ? $"id {id}" : "(empty)")}");

        if (ProbeLevel >= 2) QueueGiveAll(m);
    }

    static void ProbeAfterChange(IMemory m)
    {
        var held = new List<string>();
        for (int id = 0; id < Count; id++) if (Has(m, id)) held.Add($"{id}x{Held(m, id)}");
        Console.WriteLine($"[kf1debug] after the change: {DistinctHeld(m)} held: {string.Join(' ', held)}");
    }

    internal static void Reset()
    {
        lock (_gate) { _pendingGive.Clear(); _pendingRemove.Clear(); _pendingEquip.Clear(); }
        _probeDone = false;
        Status = "";
    }
}
