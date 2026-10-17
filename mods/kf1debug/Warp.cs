// ModCompiler compiles mods with no implicit usings, so every namespace the
// file needs must be named here -- including System.
using System;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1.Mods.Debug;

/// <summary>
/// Position bookmarks and area warp.
///
/// A bookmark is the position and the angles plus the area byte 0x800A078A. In
/// the area it was taken in, a recall writes them; from another area it warps
/// first. The warp is the shell's (patches/Warp.cs): from a pre-hook on stage A,
/// poll the interrupts and call func_80036850(area, entry 0), the transition the
/// exits make, which loads the area synchronously and leaves x and z snapped to
/// the cell centre. The five areas share one coordinate frame, so an area warp
/// with no bookmark lands at the same x and z over the new area's floor.
/// </summary>
internal static class Warp
{
    internal const int SlotCount = 4;
    internal const int FirstArea = 1, LastArea = 5;

    const uint AreaTransition = 0x80036850;

    internal static string Status = "";

    /// <summary>Switch noclip on after an area warp with no bookmark: the cell
    /// the player arrives over may be inside the new area's walls.</summary>
    internal static bool FlyAfterWarp = true;

    struct Bookmark
    {
        public bool Set;
        public int Area;
        public int X, Y, Z, Pitch, Yaw, Roll;
    }

    static readonly Bookmark[] _slots = new Bookmark[SlotCount];

    // Queued from the UI thread, run in the stage-A pre-hook on the game thread.
    static readonly object _gate = new();
    static int _warpArea = -1;
    static Bookmark _target;         // Set = place here after the move
    static bool _movePending;
    static bool _moveAngles;

    internal static bool IsSet(int slot) => slot >= 0 && slot < SlotCount && _slots[slot].Set;

    internal static (int X, int Y, int Z) SlotPosition(int slot) =>
        IsSet(slot) ? (_slots[slot].X, _slots[slot].Y, _slots[slot].Z) : (0, 0, 0);

    internal static int SlotArea(int slot) => IsSet(slot) ? _slots[slot].Area : -1;

    internal static bool Busy { get { lock (_gate) return _warpArea >= 0 || _movePending; } }

    internal static string AreaName(int area) => $"area {area}";

    // ---- bookmarks ----

    internal static bool Save(int slot)
    {
        var mem = RecompOne.Runtime.Runtime.Mem;
        if (mem == null || !GameState.IsInGame(mem)) { Status = "nothing to bookmark -- no area running"; return false; }
        if (slot < 0 || slot >= SlotCount) return false;

        var (x, y, z) = GameState.Position(mem);
        var (pitch, yaw, roll) = GameState.Angles(mem);
        _slots[slot] = new Bookmark
        {
            Set = true, Area = mem.ReadU8(GameState.Area),
            X = x, Y = y, Z = z, Pitch = pitch, Yaw = yaw, Roll = roll,
        };
        Persist(slot);
        Status = $"bookmark {slot + 1} saved in {AreaName(_slots[slot].Area)} at {Format(x, y, z)}";
        Console.WriteLine($"[kf1debug] {Status}");
        return true;
    }

    internal static bool Restore(int slot)
    {
        var mem = RecompOne.Runtime.Runtime.Mem;
        if (mem == null || !GameState.IsInGame(mem)) { Status = "no area running"; return false; }
        if (!IsSet(slot)) { Status = $"bookmark {slot + 1} is empty"; return false; }

        Bookmark b = _slots[slot];
        lock (_gate)
        {
            if (_warpArea >= 0 || _movePending) { Status = "a move is already queued"; return false; }
            _target = b;
            _moveAngles = true;
            if (b.Area == mem.ReadU8(GameState.Area)) _movePending = true;
            else _warpArea = b.Area;
        }
        Status = $"recalling bookmark {slot + 1} in {AreaName(b.Area)} at {Format(b.X, b.Y, b.Z)}...";
        Console.WriteLine($"[kf1debug] {Status}");
        return true;
    }

    internal static bool Teleport(int x, int y, int z)
    {
        var mem = RecompOne.Runtime.Runtime.Mem;
        if (mem == null || !GameState.IsInGame(mem)) { Status = "no area running"; return false; }
        lock (_gate)
        {
            if (_warpArea >= 0 || _movePending) { Status = "a move is already queued"; return false; }
            _target = new Bookmark { Set = true, X = x, Y = y, Z = z };
            _moveAngles = false;
            _movePending = true;
        }
        Status = $"moving to {Format(x, y, z)}...";
        return true;
    }

    internal static bool ToArea(int area)
    {
        var mem = RecompOne.Runtime.Runtime.Mem;
        if (mem == null || !GameState.IsInGame(mem)) { Status = "no area running -- load a save first"; return false; }
        if (area < FirstArea || area > LastArea) { Status = $"area {area} does not exist"; return false; }
        lock (_gate)
        {
            if (_warpArea >= 0 || _movePending) { Status = "a move is already queued"; return false; }
            _warpArea = area;
            _target = default;
        }
        Status = $"warping to {AreaName(area)}...";
        Console.WriteLine($"[kf1debug] queued warp to {AreaName(area)}");
        return true;
    }

    /// <summary>
    /// Stage A's entry, where the shell's warp runs too: the transition is called
    /// here and the player placed before the tick reads the position.
    /// </summary>
    [PreHook("game", Address = GameState.PlayerStage)]
    static void BeforePlayerStage(CpuContext c, IMemory m)
    {
        int area;
        Bookmark target;
        bool move, angles;
        lock (_gate)
        {
            if (_warpArea < 0 && !_movePending) return;
            area = _warpArea; target = _target; move = _movePending; angles = _moveAngles;
            _warpArea = -1; _movePending = false; _target = default;
        }
        if (!GameState.IsInGame(m)) { Status = "no area running"; return; }

        if (area >= 0)
        {
            int from = m.ReadU8(GameState.Area);
            RecompOne.Runtime.Interrupts.Poll(c, m);
            var saved = c.Snapshot();
            c.A0 = (uint)area;
            c.A1 = 0u;
            Recompiled.KingsField1_game.func_80036850(c, m);
            c.Restore(saved);
            Console.WriteLine($"[kf1debug] warped from area {from} to area {area}");
        }

        if (target.Set)
        {
            GameState.SetPosition(m, target.X, target.Y, target.Z);
            if (angles) GameState.SetAngles(m, target.Pitch, target.Yaw, target.Roll);
            Status = (area >= 0 ? $"warped to {AreaName(area)} and " : "") + $"moved to {Format(target.X, target.Y, target.Z)}";
        }
        else
        {
            var (x, _, z) = GameState.Position(m);
            int y = GameState.StandingY(m, x, z);
            GameState.SetPosition(m, x, y, z);
            Status = $"warped to {AreaName(area)} at {Format(x, y, z)}";
            if (FlyAfterWarp && !Noclip.Enabled)
            {
                Noclip.Enabled = true;
                Status += "; flying (noclip on)";
            }
        }
        GameState.StopMotion(m);
        m.WriteU32(GameState.PosExtra, 0);
        Noclip.Resync();
        Console.WriteLine($"[kf1debug] {Status}");
    }

    static string Format(int x, int y, int z) => $"({x}, {y}, {z})";

    internal static void Reset()
    {
        lock (_gate)
        {
            _warpArea = -1;
            _movePending = false;
            _target = default;
        }
    }

    // ---- persistence ----

    static string Key(int slot, string field) => $"kf1.debug.bookmark{slot}.{field}";

    static void Persist(int slot)
    {
        var view = RecompOne.Runtime.Runtime.View;
        ref Bookmark b = ref _slots[slot];
        view.SetBool(Key(slot, "set"), b.Set);
        view.SetInt(Key(slot, "area"), b.Area);
        view.SetInt(Key(slot, "x"), b.X);
        view.SetInt(Key(slot, "y"), b.Y);
        view.SetInt(Key(slot, "z"), b.Z);
        view.SetInt(Key(slot, "pitch"), b.Pitch);
        view.SetInt(Key(slot, "yaw"), b.Yaw);
        view.SetInt(Key(slot, "roll"), b.Roll);
        RecompOne.Runtime.Runtime.SaveView();
    }

    internal static void LoadPersisted()
    {
        var view = RecompOne.Runtime.Runtime.View;
        for (int i = 0; i < SlotCount; i++)
        {
            _slots[i] = new Bookmark
            {
                Set   = view.GetBool(Key(i, "set"), false),
                Area  = view.GetInt(Key(i, "area"), 0),
                X     = view.GetInt(Key(i, "x"), 0),
                Y     = view.GetInt(Key(i, "y"), 0),
                Z     = view.GetInt(Key(i, "z"), 0),
                Pitch = view.GetInt(Key(i, "pitch"), 0),
                Yaw   = view.GetInt(Key(i, "yaw"), 0),
                Roll  = view.GetInt(Key(i, "roll"), 0),
            };
        }
    }

    internal static void Clear(int slot)
    {
        if (slot < 0 || slot >= SlotCount) return;
        _slots[slot] = default;
        Persist(slot);
        Status = $"bookmark {slot + 1} cleared";
    }
}
