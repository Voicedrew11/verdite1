using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001F0C4</c>, the people, transcribed literally. <c>KF1_SUBMIT_PERSON</c>.
/// </summary>
public static class SubmitPerson
{
    public static readonly Literal R = new(typeof(SubmitPerson), "people", "SUBMIT_PERSON", 0x8001F0C4, Run,
        "Draws the people (68-byte records at 0x8009DB88).");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x78u;
        mem.WriteU32(c.SP + 0x74u, c.RA);
        mem.WriteU32(c.SP + 0x70u, c.S2);
        mem.WriteU32(c.SP + 0x6Cu, c.S1);
        c.S1 = c.A0;
        c.A0 = 0x80090000u;
        c.A0 = c.A0 + 0x56A0u;
        mem.WriteU32(c.SP + 0x68u, c.S0);
        c.RA = 0x8001F0E8u;
        KingsField1_game.SetRotMatrix_game(c, mem);
        c.A0 = 0x80090000u;
        c.A0 = c.A0 + 0x56A0u;
        c.RA = 0x8001F0F8u;
        KingsField1_game.SetTransMatrix_game(c, mem);
        c.S2 = 0x80090000u;
        c.S2 = c.S2 + 0x5744u;
        c.V0 = mem.ReadU16(c.S1 + 0x24u);
        c.V1 = mem.ReadU16(c.S2);
        c.A0 = c.SP + 0x18u;
        c.V0 = c.V0 - c.V1;
        mem.WriteU16(c.SP + 0x18u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x28u);
        c.V1 = 0x80090000u;
        c.V1 = mem.ReadU16(c.V1 + 0x5748u);
        c.A1 = c.SP + 0x54u;
        c.V0 = c.V0 - c.V1;
        mem.WriteU16(c.SP + 0x1Au, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x2Cu);
        c.V1 = 0x80090000u;
        c.V1 = mem.ReadU16(c.V1 + 0x574Cu);
        c.A2 = c.SP + 0x60u;
        c.V0 = c.V0 - c.V1;
        mem.WriteU16(c.SP + 0x1Cu, (ushort)c.V0);
        c.RA = 0x8001F148u;
        KingsField1_game.func_8004DADC(c, mem);
        c.A0 = c.S1 + 0x34u;
        c.S0 = c.SP + 0x20u;
        c.A1 = c.S0;
        c.RA = 0x8001F158u;
        KingsField1_game.func_8004E9B8(c, mem);
        c.A0 = c.S2 - 0xA4u;
        c.A1 = c.S0;
        c.S0 = c.SP + 0x40u;
        c.A2 = c.S0;
        c.RA = 0x8001F16Cu;
        KingsField1_game.func_8004CDD4(c, mem);
        c.A0 = c.S0;
        c.RA = 0x8001F174u;
        KingsField1_game.SetRotMatrix_game(c, mem);
        c.A0 = c.S0;
        c.RA = 0x8001F17Cu;
        KingsField1_game.SetTransMatrix_game(c, mem);
        c.S0 = mem.ReadU8(c.S1 + 0x2u);
        c.S0 = c.S0 + 0xAu;
        c.S0 = c.S0 & 0xFFFFu;
        c.A0 = c.S0;
        c.RA = 0x8001F194u;
        KingsField1_game.func_8002059C(c, mem);
        c.A0 = 0u;
        c.RA = 0x8001F19Cu;
        KingsField1_game.func_8001C114(c, mem);
        c.A0 = c.S1 + 0x3Cu;
        c.S2 = c.V0;
        c.A2 = mem.ReadU8(c.S1 + 0xFu);
        c.A3 = mem.ReadU16(c.S1 + 0x12u);
        c.V0 = mem.ReadU32(c.S2 + 0x4u);
        c.A1 = c.S0;
        mem.WriteU32(c.SP + 0x10u, c.V0);
        c.RA = 0x8001F1BCu;
        KingsField1_game.func_800205D4(c, mem);
        if (c.V0 != 0u) {
            goto L8001F1E8;
        }
        c.A0 = 0u;
        c.RA = 0x8001F1CCu;
        KingsField1_game.func_8001C148(c, mem);
        c.A0 = 0u;
        c.RA = 0x8001F1D4u;
        KingsField1_game.func_8001C114(c, mem);
        c.A0 = mem.ReadU32(c.V0 + 0x4u);
        c.RA = 0x8001F1E0u;
        KingsField1_game.func_8001C60C(c, mem);
        c.A0 = 0u;
        goto L8001F1F8;
        L8001F1E8: ;
        c.A0 = mem.ReadU32(c.S2 + 0x4u);
        c.RA = 0x8001F1F4u;
        KingsField1_game.func_8001C60C(c, mem);
        c.A0 = 0u;
        L8001F1F8: ;
        c.A1 = 0u;
        c.RA = 0x8001F200u;
        KingsField1_game.func_8001C7F8(c, mem);
        c.RA = mem.ReadU32(c.SP + 0x74u);
        c.S2 = mem.ReadU32(c.SP + 0x70u);
        c.S1 = mem.ReadU32(c.SP + 0x6Cu);
        c.S0 = mem.ReadU32(c.SP + 0x68u);
        c.SP = c.SP + 0x78u;
        return;
    }
}
