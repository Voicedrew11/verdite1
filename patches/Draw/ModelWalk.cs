using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001F218</c>, the model walk, transcribed literally. <c>KF1_MODELWALK</c>.
/// </summary>
public static class ModelWalk
{
    public static readonly Literal R = new(typeof(ModelWalk), "model walk", "MODELWALK", 0x8001F218, Run,
        "Walks the model tables around the eye and calls their submitters.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x30u;
        mem.WriteU32(c.SP + 0x2Cu, c.RA);
        mem.WriteU32(c.SP + 0x28u, c.S6);
        mem.WriteU32(c.SP + 0x24u, c.S5);
        mem.WriteU32(c.SP + 0x20u, c.S4);
        mem.WriteU32(c.SP + 0x1Cu, c.S3);
        mem.WriteU32(c.SP + 0x18u, c.S2);
        mem.WriteU32(c.SP + 0x14u, c.S1);
        mem.WriteU32(c.SP + 0x10u, c.S0);
        c.A0 = 0u | 0x0001u;
        c.S1 = 0x80070000u;
        c.S1 = c.S1 - 0x1220u;
        c.S2 = 0u | 0x00BDu;
        c.A1 = 0x80090000u;
        c.A1 = mem.ReadU32(c.A1 + 0x5860u);
        c.S0 = c.S1 + 0x2u;
        c.V0 = mem.ReadU16(c.A1 + 0x6u);
        c.V1 = 0x80090000u;
        c.V1 = mem.ReadU16(c.V1 + 0x575Eu);
        c.A1 = mem.ReadU16(c.A1 + 0x4u);
        c.V1 = c.V1 - c.V0;
        c.V0 = 0x80090000u;
        c.V0 = mem.ReadU16(c.V0 + 0x575Cu);
        c.S6 = c.V1;
        c.V0 = c.V0 - c.A1;
        c.S5 = c.V0;
        c.RA = 0x8001F284u;
        KingsField1_game.func_8001C0E8(c, mem);
        L8001F284: ;
        Poll(c, mem);
        c.V0 = mem.ReadU8(c.S1);
        c.V0 = c.V0 < 0x00000085u ? 1u : 0u;
        if (c.V0 == 0u) {
            goto L8001F304;
        }
        c.V1 = mem.ReadU16(c.S0 + 0x2u);
        c.A2 = 0x80090000u;
        c.A2 = mem.ReadU32(c.A2 + 0x5860u);
        c.V1 = c.V1 - c.S6;
        c.V0 = mem.ReadU16(c.A2 + 0x2u);
        c.V1 = c.V1 & 0xFFFFu;
        c.V0 = c.V1 < c.V0 ? 1u : 0u;
        if (c.V0 == 0u) {
            goto L8001F304;
        }
        c.V0 = mem.ReadU16(c.S0);
        c.A0 = mem.ReadU16(c.A2);
        c.V0 = c.V0 - c.S5;
        c.A1 = c.V0 & 0xFFFFu;
        c.V0 = c.A1 < c.A0 ? 1u : 0u;
        if (c.V0 == 0u) {
            Mult(c, c.V1, c.A0);
            goto L8001F304;
        }
        Mult(c, c.V1, c.A0);
        c.V0 = c.LO;
        c.V0 = c.V0 + c.A1;
        c.V0 = c.A2 + c.V0;
        c.V0 = mem.ReadU8(c.V0 + 0x8u);
        if (c.V0 == 0u) {
            goto L8001F304;
        }
        c.A0 = c.S1;
        c.RA = 0x8001F304u;
        KingsField1_game.func_8001EBB8(c, mem);
        L8001F304: ;
        c.S0 = c.S0 + 0x2Cu;
        c.V0 = c.S2 - 0x1u;
        c.S2 = c.V0;
        c.V0 = c.V0 << 16;
        c.V0 = (uint)((int)c.V0 >> 16);
        c.V1 = 0xFFFFFFFFu;
        if (c.V0 != c.V1) {
            c.S1 = c.S1 + 0x2Cu;
            goto L8001F284;
        }
        c.S1 = c.S1 + 0x2Cu;
        c.S1 = 0x80070000u;
        c.S1 = c.S1 - 0x3B48u;
        c.S2 = 0u | 0x007Fu;
        c.S4 = 0x80090000u;
        c.S4 = c.S4 + 0x575Eu;
        c.S0 = c.S1 + 0x16u;
        L8001F33C: ;
        Poll(c, mem);
        c.V1 = mem.ReadU8(c.S0 - 0x10u);
        c.V0 = 0u | 0x0001u;
        if (c.V1 != c.V0) {
            goto L8001F404;
        }
        c.V0 = mem.ReadU8(c.S0 - 0x14u);
        if (c.V0 != 0u) {
            goto L8001F3BC;
        }
        c.V1 = mem.ReadU16(c.S0 + 0x2u);
        c.A2 = 0x80090000u;
        c.A2 = mem.ReadU32(c.A2 + 0x5860u);
        c.V1 = c.V1 - c.S6;
        c.V0 = mem.ReadU16(c.A2 + 0x2u);
        c.V1 = c.V1 & 0xFFFFu;
        c.V0 = c.V1 < c.V0 ? 1u : 0u;
        if (c.V0 == 0u) {
            goto L8001F404;
        }
        c.V0 = mem.ReadU16(c.S0);
        c.A0 = mem.ReadU16(c.A2);
        c.V0 = c.V0 - c.S5;
        c.A1 = c.V0 & 0xFFFFu;
        c.V0 = c.A1 < c.A0 ? 1u : 0u;
        if (c.V0 == 0u) {
            Mult(c, c.V1, c.A0);
            goto L8001F404;
        }
        Mult(c, c.V1, c.A0);
        c.V0 = c.LO;
        c.V0 = c.V0 + c.A1;
        c.V0 = c.A2 + c.V0;
        c.V0 = mem.ReadU8(c.V0 + 0x8u);
        goto L8001F3F4;
        L8001F3BC: ;
        c.V0 = mem.ReadU16(c.S0 + 0x2u);
        c.V1 = mem.ReadU16(c.S4);
        c.V0 = c.V0 + 0xCu;
        c.V1 = c.V0 - c.V1;
        c.V0 = c.V1 & 0xFFFFu;
        c.V0 = c.V0 < 0x00000018u ? 1u : 0u;
        if (c.V0 == 0u) {
            goto L8001F404;
        }
        c.V0 = mem.ReadU16(c.S0);
        c.V1 = mem.ReadU16(c.S4 - 0x2u);
        c.V0 = c.V0 + 0xCu;
        c.V0 = c.V0 - c.V1;
        c.V0 = c.V0 & 0xFFFFu;
        c.V0 = c.V0 < 0x00000018u ? 1u : 0u;
        L8001F3F4: ;
        if (c.V0 == 0u) {
            goto L8001F404;
        }
        c.A0 = c.S1;
        c.RA = 0x8001F404u;
        KingsField1_game.func_8001E9A4(c, mem);
        L8001F404: ;
        c.S0 = c.S0 + 0x48u;
        c.V0 = c.S2 - 0x1u;
        c.S2 = c.V0;
        c.V0 = c.V0 << 16;
        c.S3 = (uint)((int)c.V0 >> 16);
        c.V0 = 0xFFFFFFFFu;
        if (c.S3 != c.V0) {
            c.S1 = c.S1 + 0x48u;
            goto L8001F33C;
        }
        c.S1 = c.S1 + 0x48u;
        c.A0 = 0x80050000u;
        c.A0 = c.A0 + 0x5F88u;
        c.RA = 0x8001F434u;
        KingsField1_game.SetLightMatrix_game(c, mem);
        c.V0 = 0u | 0x00B4u;
        c.At = 0x80090000u;
        mem.WriteU8(c.At + 0x505Eu, (byte)c.V0);
        c.At = 0x80090000u;
        mem.WriteU8(c.At + 0x505Du, (byte)c.V0);
        c.At = 0x80090000u;
        mem.WriteU8(c.At + 0x505Cu, (byte)c.V0);
        c.A0 = 0x80090000u;
        c.A0 = c.A0 + 0x505Au;
        c.S2 = 0x80090000u;
        c.S2 = mem.ReadU16(c.S2 + 0x5090u);
        c.V0 = 0x80090000u;
        c.V0 = mem.ReadU16(c.V0 + 0x508Eu);
        c.V1 = 0x80090000u;
        c.V1 = mem.ReadU16(c.V1 + 0x508Cu);
        mem.WriteU16(c.A0, (ushort)c.V0);
        c.At = 0x80090000u;
        mem.WriteU16(c.At + 0x5058u, (ushort)c.V1);
        c.V0 = c.S2 - 0x1u;
        c.S2 = c.V0;
        c.V0 = c.V0 << 16;
        c.V0 = (uint)((int)c.V0 >> 16);
        if (c.V0 == c.S3) {
            c.S4 = c.A0 + 0x3Eu;
            goto L8001F588;
        }
        c.S4 = c.A0 + 0x3Eu;
        c.S1 = 0u | 0x07D0u;
        c.S0 = c.A0 + 0x42u;
        L8001F49C: ;
        Poll(c, mem);
        c.V0 = mem.ReadU32(c.S0 + 0x8u);
        Div(c, c.V0, c.S1); // at = 0x80000000 after
        c.V0 = c.LO;
        c.A1 = 0x80090000u;
        c.A1 = mem.ReadU32(c.A1 + 0x5860u);
        c.V1 = c.V0 - c.S6;
        c.V0 = mem.ReadU16(c.A1 + 0x2u);
        c.A2 = c.V1 & 0xFFFFu;
        c.V0 = c.A2 < c.V0 ? 1u : 0u;
        if (c.V0 == 0u) {
            goto L8001F568;
        }
        c.V0 = mem.ReadU32(c.S0);
        Div(c, c.V0, c.S1); // at = 0x80000000 after
        c.V0 = c.LO;
        c.V1 = mem.ReadU16(c.A1);
        c.V0 = c.V0 - c.S5;
        c.A0 = c.V0 & 0xFFFFu;
        c.V0 = c.A0 < c.V1 ? 1u : 0u;
        if (c.V0 == 0u) {
            Mult(c, c.A2, c.V1);
            goto L8001F568;
        }
        Mult(c, c.A2, c.V1);
        c.V0 = c.LO;
        c.V0 = c.V0 + c.A0;
        c.V0 = c.A1 + c.V0;
        c.V0 = mem.ReadU8(c.V0 + 0x8u);
        if (c.V0 == 0u) {
            goto L8001F568;
        }
        c.A0 = c.S4;
        c.RA = 0x8001F568u;
        KingsField1_game.func_8001ED90(c, mem);
        L8001F568: ;
        c.S0 = c.S0 + 0x18u;
        c.V0 = c.S2 - 0x1u;
        c.S2 = c.V0;
        c.V0 = c.V0 << 16;
        c.V0 = (uint)((int)c.V0 >> 16);
        c.V1 = 0xFFFFFFFFu;
        if (c.V0 != c.V1) {
            c.S4 = c.S4 + 0x18u;
            goto L8001F49C;
        }
        c.S4 = c.S4 + 0x18u;
        L8001F588: ;
        c.A0 = 0x80050000u;
        c.A0 = c.A0 + 0x5FA8u;
        c.S2 = 0u | 0x002Fu;
        c.RA = 0x8001F598u;
        KingsField1_game.SetLightMatrix_game(c, mem);
        c.S1 = 0x800A0000u;
        c.S1 = c.S1 - 0x2FC0u;
        c.S4 = 0u | 0x00FFu;
        c.S3 = 0u | 0x07D0u;
        c.S0 = c.S1 + 0xCu;
        L8001F5AC: ;
        Poll(c, mem);
        c.V0 = mem.ReadU8(c.S1);
        if (c.V0 == c.S4) {
            goto L8001F698;
        }
        c.V0 = mem.ReadU8(c.S0 - 0x9u);
        if (c.V0 == c.S4) {
            goto L8001F698;
        }
        c.V0 = mem.ReadU32(c.S0 + 0x8u);
        Div(c, c.V0, c.S3); // at = 0x80000000 after
        c.V0 = c.LO;
        c.A1 = 0x80090000u;
        c.A1 = mem.ReadU32(c.A1 + 0x5860u);
        c.V1 = c.V0 - c.S6;
        c.V0 = mem.ReadU16(c.A1 + 0x2u);
        c.A2 = c.V1 & 0xFFFFu;
        c.V0 = c.A2 < c.V0 ? 1u : 0u;
        if (c.V0 == 0u) {
            goto L8001F698;
        }
        c.V0 = mem.ReadU32(c.S0);
        Div(c, c.V0, c.S3); // at = 0x80000000 after
        c.V0 = c.LO;
        c.V1 = mem.ReadU16(c.A1);
        c.V0 = c.V0 - c.S5;
        c.A0 = c.V0 & 0xFFFFu;
        c.V0 = c.A0 < c.V1 ? 1u : 0u;
        if (c.V0 == 0u) {
            Mult(c, c.A2, c.V1);
            goto L8001F698;
        }
        Mult(c, c.A2, c.V1);
        c.V0 = c.LO;
        c.V0 = c.V0 + c.A0;
        c.V0 = c.A1 + c.V0;
        c.V0 = mem.ReadU8(c.V0 + 0x8u);
        if (c.V0 == 0u) {
            goto L8001F698;
        }
        c.A0 = c.S1;
        c.RA = 0x8001F698u;
        KingsField1_game.func_8001EEDC(c, mem);
        L8001F698: ;
        c.S0 = c.S0 + 0x3Cu;
        c.V0 = c.S2 - 0x1u;
        c.S2 = c.V0;
        c.V0 = c.V0 << 16;
        c.V0 = (uint)((int)c.V0 >> 16);
        c.V1 = 0xFFFFFFFFu;
        if (c.V0 != c.V1) {
            c.S1 = c.S1 + 0x3Cu;
            goto L8001F5AC;
        }
        c.S1 = c.S1 + 0x3Cu;
        c.A0 = 0x80090000u;
        c.A0 = c.A0 + 0x5700u;
        c.S2 = 0u | 0x0007u;
        c.RA = 0x8001F6C8u;
        KingsField1_game.SetLightMatrix_game(c, mem);
        c.S0 = 0x800A0000u;
        c.S0 = c.S0 - 0x2478u;
        c.S1 = c.S0 + 0x1Cu;
        L8001F6D4: ;
        Poll(c, mem);
        c.V1 = mem.ReadU8(c.S0);
        c.V0 = 0u | 0x0001u;
        if (c.V1 != c.V0) {
            goto L8001F750;
        }
        c.V1 = mem.ReadU16(c.S1 + 0x2u);
        c.A2 = 0x80090000u;
        c.A2 = mem.ReadU32(c.A2 + 0x5860u);
        c.V1 = c.V1 - c.S6;
        c.V0 = mem.ReadU16(c.A2 + 0x2u);
        c.V1 = c.V1 & 0xFFFFu;
        c.V0 = c.V1 < c.V0 ? 1u : 0u;
        if (c.V0 == 0u) {
            goto L8001F750;
        }
        c.V0 = mem.ReadU16(c.S1);
        c.A0 = mem.ReadU16(c.A2);
        c.V0 = c.V0 - c.S5;
        c.A1 = c.V0 & 0xFFFFu;
        c.V0 = c.A1 < c.A0 ? 1u : 0u;
        if (c.V0 == 0u) {
            Mult(c, c.V1, c.A0);
            goto L8001F750;
        }
        Mult(c, c.V1, c.A0);
        c.V0 = c.LO;
        c.V0 = c.V0 + c.A1;
        c.V0 = c.A2 + c.V0;
        c.V0 = mem.ReadU8(c.V0 + 0x8u);
        if (c.V0 == 0u) {
            goto L8001F750;
        }
        c.A0 = c.S0;
        c.RA = 0x8001F750u;
        KingsField1_game.func_8001F0C4(c, mem);
        L8001F750: ;
        c.S1 = c.S1 + 0x44u;
        c.V0 = c.S2 - 0x1u;
        c.S2 = c.V0;
        c.V0 = c.V0 << 16;
        c.V0 = (uint)((int)c.V0 >> 16);
        c.V1 = 0xFFFFFFFFu;
        if (c.V0 != c.V1) {
            c.S0 = c.S0 + 0x44u;
            goto L8001F6D4;
        }
        c.S0 = c.S0 + 0x44u;
        c.RA = mem.ReadU32(c.SP + 0x2Cu);
        c.S6 = mem.ReadU32(c.SP + 0x28u);
        c.S5 = mem.ReadU32(c.SP + 0x24u);
        c.S4 = mem.ReadU32(c.SP + 0x20u);
        c.S3 = mem.ReadU32(c.SP + 0x1Cu);
        c.S2 = mem.ReadU32(c.SP + 0x18u);
        c.S1 = mem.ReadU32(c.SP + 0x14u);
        c.S0 = mem.ReadU32(c.SP + 0x10u);
        c.SP = c.SP + 0x30u;
        return;
    }
}
