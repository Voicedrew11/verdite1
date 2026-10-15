using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001EBB8</c>, the interaction objects, transcribed literally. <c>KF1_SUBMIT_INTERACTION</c>.
/// </summary>
public static class SubmitInteraction
{
    public static readonly Literal R = new(typeof(SubmitInteraction), "interaction objects", "SUBMIT_INTERACTION", 0x8001EBB8, Run,
        "Draws one interaction record (44 bytes at 0x8006EDE0): pickups, doors, props.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x98u;
        mem.WriteU32(c.SP + 0x90u, c.RA);
        mem.WriteU32(c.SP + 0x8Cu, c.S3);
        mem.WriteU32(c.SP + 0x88u, c.S2);
        mem.WriteU32(c.SP + 0x84u, c.S1);
        c.S1 = c.A0;
        c.A0 = 0x80090000u;
        c.A0 = c.A0 + 0x56A0u;
        mem.WriteU32(c.SP + 0x80u, c.S0);
        c.RA = 0x8001EBE0u;
        KingsField1_game.SetRotMatrix_game(c, mem);
        c.A0 = 0x80090000u;
        c.A0 = c.A0 + 0x56A0u;
        c.RA = 0x8001EBF0u;
        KingsField1_game.SetTransMatrix_game(c, mem);
        c.S3 = 0x80090000u;
        c.S3 = c.S3 + 0x5744u;
        c.V0 = mem.ReadU16(c.S1 + 0x8u);
        c.V1 = mem.ReadU16(c.S3);
        c.A0 = c.SP + 0x10u;
        c.V0 = c.V0 - c.V1;
        mem.WriteU16(c.SP + 0x10u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0xCu);
        c.V1 = 0x80090000u;
        c.V1 = mem.ReadU16(c.V1 + 0x5748u);
        c.A1 = c.SP + 0x4Cu;
        c.V0 = c.V0 - c.V1;
        mem.WriteU16(c.SP + 0x12u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x10u);
        c.V1 = 0x80090000u;
        c.V1 = mem.ReadU16(c.V1 + 0x574Cu);
        c.A2 = c.SP + 0x78u;
        c.V0 = c.V0 - c.V1;
        mem.WriteU16(c.SP + 0x14u, (ushort)c.V0);
        c.RA = 0x8001EC40u;
        KingsField1_game.func_8004DADC(c, mem);
        c.S2 = c.SP + 0x18u;
        c.A0 = (uint)(short)mem.ReadU16(c.S1 + 0x18u);
        c.A1 = c.S2;
        c.RA = 0x8001EC50u;
        KingsField1_game.func_80014B7C(c, mem);
        c.S0 = c.SP + 0x38u;
        c.A0 = (uint)(short)mem.ReadU16(c.S1 + 0x1Au);
        c.A1 = c.S0;
        c.RA = 0x8001EC60u;
        KingsField1_game.func_80014BEC(c, mem);
        c.A0 = c.S0;
        c.A1 = c.S2;
        c.RA = 0x8001EC6Cu;
        KingsField1_game.func_8004CFF4(c, mem);
        c.A0 = c.S3 - 0x64u;
        c.A1 = c.S0;
        c.S2 = c.SP + 0x58u;
        c.A2 = c.S2;
        c.RA = 0x8001EC80u;
        KingsField1_game.func_8004CDD4(c, mem);
        c.A0 = c.S3 - 0xA4u;
        c.A1 = c.S0;
        c.RA = 0x8001EC8Cu;
        KingsField1_game.func_8004D11C(c, mem);
        c.A0 = c.S0;
        c.RA = 0x8001EC94u;
        KingsField1_game.SetRotMatrix_game(c, mem);
        c.A0 = c.S0;
        c.RA = 0x8001EC9Cu;
        KingsField1_game.SetTransMatrix_game(c, mem);
        c.A0 = c.S2;
        c.RA = 0x8001ECA4u;
        KingsField1_game.SetLightMatrix_game(c, mem);
        c.V0 = mem.ReadU8(c.S1);
        c.S0 = mem.ReadU8(c.S1);
        c.V0 = c.V0 << 3;
        c.At = 0x80070000u;
        c.At = c.At - 0x1720u;
        c.At = c.At + c.V0;
        c.V1 = mem.ReadU8(c.At);
        if ((int)c.V1 < 0) {
            c.V0 = (int)c.V1 < 2 ? 1u : 0u;
            goto L8001ECEC;
        }
        c.V0 = (int)c.V1 < 2 ? 1u : 0u;
        if (c.V0 != 0u) {
            c.V0 = (int)c.V1 < 4 ? 1u : 0u;
            goto L8001ECE4;
        }
        c.V0 = (int)c.V1 < 4 ? 1u : 0u;
        if (c.V0 == 0u) {
            c.S1 = 0u | 0x00B4u;
            goto L8001ECEC;
        }
        c.S1 = 0u | 0x00B4u;
        c.S0 = c.S0 & 0xFFFFu;
        goto L8001ECF4;
        L8001ECE4: ;
        c.S1 = 0u | 0x000Fu;
        goto L8001ECF0;
        L8001ECEC: ;
        c.S1 = 0u;
        L8001ECF0: ;
        c.S0 = c.S0 & 0xFFFFu;
        L8001ECF4: ;
        c.A0 = c.S0;
        c.RA = 0x8001ECFCu;
        KingsField1_game.func_8001C148(c, mem);
        c.A0 = c.S0;
        c.RA = 0x8001ED04u;
        KingsField1_game.func_8001C114(c, mem);
        c.A0 = mem.ReadU32(c.V0 + 0x4u);
        c.RA = 0x8001ED10u;
        KingsField1_game.func_8001C60C(c, mem);
        c.A0 = c.S0;
        c.A1 = c.S1;
        c.RA = 0x8001ED1Cu;
        KingsField1_game.func_8001C7F8(c, mem);
        c.RA = mem.ReadU32(c.SP + 0x90u);
        c.S3 = mem.ReadU32(c.SP + 0x8Cu);
        c.S2 = mem.ReadU32(c.SP + 0x88u);
        c.S1 = mem.ReadU32(c.SP + 0x84u);
        c.S0 = mem.ReadU32(c.SP + 0x80u);
        c.SP = c.SP + 0x98u;
        return;
    }
}
