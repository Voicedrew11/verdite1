// ModCompiler compiles mods with no implicit usings, so every namespace the
// file needs must be named here -- including System.
using System;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1.Mods.Debug;

/// <summary>
/// Invincibility, infinite MP, enemies that ignore you and the speed multiplier.
/// Every address is this game's; "Damage and death" in docs/GAME_INTERNALS.md
/// has the routines.
///
///   func_80016324   the player-hit routine: conditions, damage, the flinch.
///                   Subtracts HP clamped at 0 and never kills; no caller reads
///                   its return. Skipped while invincible: no HP, no condition,
///                   no flinch.
///   func_8001564C   signed HP add; at 0 it calls death. The equipment drains
///                   and the poison tick call it with -1. A negative delta is
///                   clamped to 0.
///   func_80015164   death: state 0xFF and the sound. Its callers are the HP add,
///                   the 0x52 hazard cell (func_80017A80) and stage A's HP == 0
///                   test. Refused.
///
/// The end of stage A also restores HP and MP, as a backstop for HP and as the
/// whole of infinite MP (the cast at 0x800193B0 subtracts MP inline).
/// </summary>
internal static class Cheats
{
    internal static bool Invincible;
    internal static bool InfiniteMp;
    internal static bool SpeedEnabled;
    internal static bool Peaceful;

    internal static float SpeedMultiplier = 2f;

    internal static long BlockedHits;
    internal static long BlockedDeaths;
    internal static long RestoredHp;
    internal static long RestoredMp;
    internal static long IgnoredPicks;

    // ---- HP ----

    [PreHook("game", Address = 0x80016324)]
    static bool BeforePlayerHit(CpuContext c, IMemory m)
    {
        if (!Invincible || !GameState.IsInGame(m)) return true;
        BlockedHits++;
        c.V0 = 0u;
        return false;
    }

    [PreHook("game", Address = 0x8001564C)]
    static void BeforeAddHp(CpuContext c, IMemory m)
    {
        if (!Invincible || !GameState.IsInGame(m)) return;
        if ((short)c.A0 < 0)
        {
            c.A0 = 0u;
            BlockedHits++;
        }
    }

    [PreHook("game", Address = 0x80015164)]
    static bool BeforeDeath(CpuContext c, IMemory m)
    {
        if (!Invincible || !GameState.IsInGame(m)) return true;
        BlockedDeaths++;
        return false;
    }

    // ---- enemies ignoring you ----

    // The picker compares the distance against the type record's u16 ranges, so
    // anything past 65535 fails every "player is near" test.
    const uint FarAway = 0x20000;

    /// <summary>
    /// func_8002E2E8(distance), the creature behaviour picker, told the player is
    /// across the map. Its four callers are the wake/sleep machine and the think;
    /// waking and drawing are not touched.
    /// </summary>
    [PreHook("game", Address = 0x8002E2E8)]
    static void BeforeBehaviourPick(CpuContext c, IMemory m)
    {
        if (!Peaceful || !GameState.IsInGame(m)) return;
        c.A0 = FarAway;
        IgnoredPicks++;
    }

    // ---- per tick ----

    [PostHook("game", Address = GameState.PlayerStage)]
    static void AfterPlayerStage(CpuContext c, IMemory m)
    {
        if (!GameState.IsInGame(m) || GameState.IsDead(m)) return;

        if (Invincible)
        {
            ushort hp = m.ReadU16(GameState.Hp), max = m.ReadU16(GameState.MaxHp);
            if (hp < max) { m.WriteU16(GameState.Hp, max); RestoredHp++; }
        }

        if (InfiniteMp)
        {
            ushort mp = m.ReadU16(GameState.Mp), max = m.ReadU16(GameState.MaxMp);
            if (mp < max) { m.WriteU16(GameState.Mp, max); RestoredMp++; }
        }

        if (SpeedEnabled) ScaleTurn(m);
    }

    // ---- speed ----
    //
    // Stage A writes the rate words and spends them inline, with no call between,
    // so they cannot be scaled on the way in. What can: the walk goes through
    // func_800171FC(angle, distance), the player's collision move, which only
    // stage A calls (three sites, forward and strafe), so its distance is scaled;
    // the turn is `yaw += turnVel` inline at 0x80018F90, so the post-hook adds the
    // remainder of the scaled turn.

    [PreHook("game", Address = 0x800171FC)]
    static void BeforeMove(CpuContext c, IMemory m)
    {
        if (!SpeedEnabled || !GameState.IsInGame(m)) return;
        int d = (int)c.A1;
        if (d <= 0) return;
        c.A1 = (uint)Math.Clamp((int)MathF.Round(d * SpeedMultiplier), 1, 0x7FFF);
    }

    static void ScaleTurn(IMemory m)
    {
        int vel = GameState.ReadS16(m, GameState.TurnVel);
        if (vel == 0) return;
        int extra = (int)MathF.Round(vel * (SpeedMultiplier - 1f));
        int yaw = (m.ReadU16(GameState.Yaw) + extra) & GameState.AngleMask;
        m.WriteU16(GameState.Yaw, (ushort)yaw);
    }

    internal static void Reset()
    {
        Invincible = false;
        InfiniteMp = false;
        SpeedEnabled = false;
        Peaceful = false;
    }
}
