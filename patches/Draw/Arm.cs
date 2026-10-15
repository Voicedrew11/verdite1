using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001F798</c>, the arm, transcribed literally. <c>KF1_ARM</c>.
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
        if (c.V1 == c.V0) {
            goto L8001F89C;
        }
        c.A0 = 0x80050000u;
        c.A0 = c.A0 + 0x5FC8u;
        c.RA = 0x8001F7D0u;
        KingsField1_game.SetLightMatrix_game(c, mem);
        c.V0 = 0x800A0000u;
        c.V0 = mem.ReadU32(c.V0 + 0x7E8u);
        c.A0 = mem.ReadU16(c.V0 + 0x10u);
        c.RA = 0x8001F7E8u;
        KingsField1_game.SetGeomScreen_game(c, mem);
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
        if (c.V0 == 0u) {
            goto L8001F89C;
        }
        c.A0 = mem.ReadU32(c.S0 + 0x4u);
        c.A1 = 0u | 0x0003u;
        c.RA = 0x8001F874u;
        KingsField1_game.func_8001C6A8(c, mem);
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
        L8001F89C: ;
        c.RA = mem.ReadU32(c.SP + 0x40u);
        c.S1 = mem.ReadU32(c.SP + 0x3Cu);
        c.S0 = mem.ReadU32(c.SP + 0x38u);
        c.SP = c.SP + 0x48u;
        return;
    }
}
