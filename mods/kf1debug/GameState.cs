// ModCompiler compiles mods with no implicit usings, so every namespace the
// file needs must be named here -- including System.
using System;
using RecompOne.Runtime.Memory;

namespace Kf1.Mods.Debug;

/// <summary>
/// GAME.EXE's player state, by address, with signed accessors.
///
/// The block at 0x800A0780 is the player: "The player" in docs/GAME_INTERNALS.md
/// has the map. Main-loop stage A, func_80018880, is the player's tick (pad, rates,
/// turn, look, walk, cast, regeneration), and stage B copies the player into the
/// camera after it, so a post-hook on stage A is the last word on the frame.
/// </summary>
internal static class GameState
{
    /// <summary>Main-loop stage A: the player's tick. Every feature here hooks it.</summary>
    internal const uint PlayerStage = 0x80018880;

    // ---- position and view ----
    internal const uint PosX      = 0x800A0824;   // s32
    internal const uint PosY      = 0x800A0828;   // s32, the eye; negative is up
    internal const uint PosZ      = 0x800A082C;   // s32
    internal const uint PosExtra  = 0x800A0830;   // s32, the fourth word stage B copies
    internal const uint Floor     = 0x800A0834;   // s32, the floor under the eye

    // 12-bit angles, 0x1000 a turn. Yaw is masked to 0xFFF by stage A; pitch is
    // a plain s16 held inside +-PitchLimit.
    internal const uint Pitch     = 0x800A0838;   // s16
    internal const uint Yaw       = 0x800A083A;   // s16, increases turning LEFT
    internal const uint Roll      = 0x800A083C;   // s16

    // Per-tick control velocities, applied by stage A itself.
    internal const uint StrafeVel = 0x800A0840;   // s16
    internal const uint FwdVel    = 0x800A0842;   // s16
    internal const uint WalkMag   = 0x800A0844;   // u16, |(strafe, walk)|
    internal const uint TurnVel   = 0x800A0846;   // s16
    internal const uint PitchVel  = 0x800A0848;   // s16

    // The cell the player stands in (pos / 2000), and the cell the main loop's
    // exit test last saw: an exit fires when the two differ on an 0x40 cell.
    internal const uint CellZ     = 0x800A084A;   // u8
    internal const uint CellX     = 0x800A084B;   // u8
    internal const uint LastCellZ = 0x800A084C;   // u8
    internal const uint LastCellX = 0x800A084D;   // u8

    // This tick's rates, written by stage A at 0x80018A60..80 and read only there:
    // walk 180 and turn 28, or 36 and 5 while condition bit 3 is set.
    internal const uint MoveSpeed = 0x80057E68;   // s32
    internal const uint TurnRate  = 0x80057E70;   // s32

    // ---- character ----
    internal const uint Exp       = 0x800A0780;   // u32, capped at 99999
    internal const uint ExpNext   = 0x800A0784;   // u32
    internal const uint Level     = 0x800A0788;   // u8
    internal const uint Area      = 0x800A078A;   // u8, 1..5
    internal const uint MaxHp     = 0x800A0790;   // u16
    internal const uint Hp        = 0x800A0792;   // u16
    internal const uint MaxMp     = 0x800A0794;   // u16
    internal const uint Mp        = 0x800A0796;   // u16

    // 0 standing, 1..8 the flinch after a hit, 0xFF dying (set only by the death
    // routine func_80015164), 0xFE the respawn.
    internal const uint State     = 0x800A0822;   // u8
    internal const byte StateDying   = 0xFF;
    internal const byte StateRespawn = 0xFE;

    // ---- the map ----
    internal const int CellSize = 2000;
    internal const int Grid = 100;
    // One height byte a cell; the floor is at y = -100 * height, and the eye
    // stands 1500 above it (-11500 on the New Game's 0x64 cell).
    internal const uint Heights = 0x80095900;
    internal const int EyeHeight = 1500;

    internal const int AngleMask = 0xFFF;
    internal const int AngleFull = 0x1000;

    /// <summary>The pitch limit stage A holds itself to (0x800190B4, 0x800190FC).</summary>
    internal const int PitchLimit = 0xBF;

    // ---- typed reads ----

    internal static int   ReadS32(IMemory m, uint a) => (int)m.ReadU32(a);
    internal static short ReadS16(IMemory m, uint a) => (short)m.ReadU16(a);

    internal static void WriteS32(IMemory m, uint a, int v) => m.WriteU32(a, (uint)v);
    internal static void WriteS16(IMemory m, uint a, int v) => m.WriteU16(a, (ushort)(short)v);

    /// <summary>An angle as 12-bit signed, in [-2048, 2047].</summary>
    internal static int ReadAngle12(IMemory m, uint a)
    {
        int v = m.ReadU16(a) & AngleMask;
        return v >= AngleFull / 2 ? v - AngleFull : v;
    }

    internal static (int X, int Y, int Z) Position(IMemory m) =>
        (ReadS32(m, PosX), ReadS32(m, PosY), ReadS32(m, PosZ));

    /// <summary>
    /// Write the position, the floor under it and the cell bytes. The floor goes
    /// to y + 1500 so the game sees the eye standing where it is; the last cell
    /// is written equal to the cell so a move never reads as stepping onto an exit.
    /// </summary>
    internal static void SetPosition(IMemory m, int x, int y, int z)
    {
        WriteS32(m, PosX, x);
        WriteS32(m, PosY, y);
        WriteS32(m, PosZ, z);
        WriteS32(m, Floor, y + EyeHeight);
        var (cx, cz) = Cell(x, z);
        m.WriteU8(CellX, cx);
        m.WriteU8(CellZ, cz);
        m.WriteU8(LastCellX, cx);
        m.WriteU8(LastCellZ, cz);
    }

    /// <summary>The cell for a position, clamped to the grid.</summary>
    internal static (byte X, byte Z) Cell(int x, int z) =>
        ((byte)Math.Clamp(x / CellSize, 0, Grid - 1), (byte)Math.Clamp(z / CellSize, 0, Grid - 1));

    /// <summary>The standing eye height over a position's cell.</summary>
    internal static int StandingY(IMemory m, int x, int z)
    {
        var (cx, cz) = Cell(x, z);
        return -100 * m.ReadU8(Heights + (uint)(cz * Grid + cx)) - EyeHeight;
    }

    internal static (int Pitch, int Yaw, int Roll) Angles(IMemory m) =>
        (ReadS16(m, Pitch), ReadS16(m, Yaw), ReadS16(m, Roll));

    internal static void SetAngles(IMemory m, int pitch, int yaw, int roll)
    {
        WriteS16(m, Pitch, Math.Clamp(pitch, -PitchLimit, PitchLimit));
        WriteS16(m, Yaw, yaw & AngleMask);
        WriteS16(m, Roll, roll);
    }

    /// <summary>Drop every movement velocity, so a teleport does not arrive still walking.</summary>
    internal static void StopMotion(IMemory m)
    {
        m.WriteU16(StrafeVel, 0);
        m.WriteU16(FwdVel, 0);
        m.WriteU16(WalkMag, 0);
        m.WriteU16(TurnVel, 0);
        m.WriteU16(PitchVel, 0);
    }

    /// <summary>
    /// Is there a character in an area? GAME.EXE is up and max HP is set. The
    /// opening overlay (OPEN.EXE) owns the same addresses for other things.
    /// </summary>
    internal static bool IsInGame(IMemory m) =>
        Kf1.AgentBeacon.Overlay == "game" && m.ReadU16(MaxHp) != 0;

    internal static bool IsDead(IMemory m)
    {
        byte s = m.ReadU8(State);
        return s == StateDying || s == StateRespawn;
    }

    internal static float AngleToRadians(int angle) =>
        angle * (2f * MathF.PI / AngleFull);
}
