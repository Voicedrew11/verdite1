using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001F798</c>, the first-person arm during a swing. Takes no arguments: it
/// reads the arm state from <c>0x800A07F0</c> (a s16, <c>-1</c> skips the draw) and the
/// pose record pointer at <c>0x800A07E8</c>. Sets the light and geometry, takes the
/// rotation and translation of the pose into the matrices, poses the selected TMD
/// object, then runs its vertex cache (<c>func_8001C6A8</c>) and polygon writer
/// (<c>func_8001C7F8</c>). <c>KF1_ARM</c>.
/// </summary>
public static class Arm
{
    public static readonly Literal R = new(typeof(Arm), "arm", "ARM", 0x8001F798, Run,
        "Draws the first-person arm during a swing.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x48u;
        mem.WriteU32(c.SP + 0x40u, c.RA);
        mem.WriteU32(c.SP + 0x3Cu, c.S1);
        mem.WriteU32(c.SP + 0x38u, c.S0);
        c.S1 = 0x800A0000u;
        c.S1 = c.S1 + 0x7F0u;
        c.V1 = (uint)(short)mem.ReadU16(c.S1);
        c.V0 = 0xFFFFFFFFu;
        if (c.V1 != c.V0) {
            // light matrix from the table at 0x80055FC8
            c.A0 = 0x80050000u;
            c.A0 = c.A0 + 0x5FC8u;
            c.RA = 0x8001F7D0u;
            KingsField1_game.SetLightMatrix_game(c, mem);
            // screen geometry from the pose record's u16 at +0x10
            c.V0 = 0x800A0000u;
            c.V0 = mem.ReadU32(c.V0 + 0x7E8u);
            c.A0 = mem.ReadU16(c.V0 + 0x10u);
            c.RA = 0x8001F7E8u;
            KingsField1_game.SetGeomScreen_game(c, mem);
            // the record's s16 at +0x1C, +0x1E and +0x20 are kept at SP+0x2C..0x34
            c.A0 = 0x800A0000u;
            c.A0 = mem.ReadU32(c.A0 + 0x7E8u);
            c.V0 = (uint)(short)mem.ReadU16(c.A0 + 0x1Cu);
            mem.WriteU32(c.SP + 0x2Cu, c.V0);
            c.V0 = (uint)(short)mem.ReadU16(c.A0 + 0x1Eu);
            mem.WriteU32(c.SP + 0x30u, c.V0);
            c.V0 = (uint)(short)mem.ReadU16(c.A0 + 0x20u);
            c.A1 = c.SP + 0x18u;
            c.A0 = c.A0 + 0x24u;
            mem.WriteU32(c.SP + 0x34u, c.V0);
            // the rotation matrix into SP+0x18, then the rotation and translation are set
            c.RA = 0x8001F820u;
            KingsField1_game.func_8004E9B8(c, mem);
            c.A0 = c.SP + 0x18u;
            c.RA = 0x8001F828u;
            KingsField1_game.SetRotMatrix_game(c, mem);
            c.A0 = c.SP + 0x18u;
            c.RA = 0x8001F830u;
            KingsField1_game.SetTransMatrix_game(c, mem);
            c.A0 = 0u | 0x0014u;
            c.RA = 0x8001F838u;
            KingsField1_game.func_8002059C(c, mem);
            // the selected TMD object (func_8001C114 with 0), posed by the record at 0x800A07F0
            c.A0 = 0u;
            c.RA = 0x8001F840u;
            KingsField1_game.func_8001C114(c, mem);
            c.A0 = c.S1 + 0x4u;
            c.S0 = c.V0;
            c.V0 = mem.ReadU32(c.S0 + 0x4u);
            c.A1 = 0u | 0x0014u;
            mem.WriteU32(c.SP + 0x10u, c.V0);
            c.A3 = (uint)(short)mem.ReadU16(c.S1);
            c.A2 = 0u;
            c.RA = 0x8001F860u;
            KingsField1_game.func_800205D4(c, mem);
            // the pose blender returns 0 when there is nothing to draw
            if (c.V0 != 0u) {
                // the object's vertices into the cache, with a1 = 3 as the depth shift
                c.A0 = mem.ReadU32(c.S0 + 0x4u);
                c.A1 = 0u | 0x0003u;
                c.RA = 0x8001F874u;
                KingsField1_game.func_8001C6A8(c, mem);
                // polygon writer: a1 = 0x32 minus the record's s16 at +0x20 shifted right by 5
                c.V0 = 0x800A0000u;
                c.V0 = mem.ReadU32(c.V0 + 0x7E8u);
                c.A1 = mem.ReadU16(c.V0 + 0x20u);
                c.A0 = 0u;
                c.A1 = c.A1 << 16;
                c.A1 = (uint)((int)c.A1 >> 21);
                c.A1 = 0u - c.A1;
                c.A1 = c.A1 + 0x32u;
                c.RA = 0x8001F89Cu;
                KingsField1_game.func_8001C7F8(c, mem);
            }
        }
        c.RA = mem.ReadU32(c.SP + 0x40u);
        c.S1 = mem.ReadU32(c.SP + 0x3Cu);
        c.S0 = mem.ReadU32(c.SP + 0x38u);
        c.SP = c.SP + 0x48u;
    }
}
