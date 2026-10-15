using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001D730</c>, the polygon writer (objects), transcribed literally. <c>KF1_POLYASSEMBLER_CACHED</c>.
/// </summary>
public static class PolyAssemblerCached
{
    public static readonly Literal R = new(typeof(PolyAssemblerCached), "polygon writer (objects)", "POLYASSEMBLER_CACHED", 0x8001D730, Run,
        "Writes an object's primitives from the vertex cache, unlit.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x60u;
        mem.WriteU32(c.SP + 0x5Cu, c.RA);
        mem.WriteU32(c.SP + 0x58u, c.FP);
        mem.WriteU32(c.SP + 0x54u, c.S7);
        mem.WriteU32(c.SP + 0x50u, c.S6);
        mem.WriteU32(c.SP + 0x4Cu, c.S5);
        mem.WriteU32(c.SP + 0x48u, c.S4);
        mem.WriteU32(c.SP + 0x44u, c.S3);
        mem.WriteU32(c.SP + 0x40u, c.S2);
        mem.WriteU32(c.SP + 0x3Cu, c.S1);
        mem.WriteU32(c.SP + 0x38u, c.S0);
        c.S0 = c.A1;
        c.A0 = c.A0 & 0xFFFFu;
        c.RA = 0x8001D768u;
        KingsField1_game.func_8001C114(c, mem);
        c.T0 = mem.ReadU32(c.V0 + 0x14u);
        c.A1 = 0x80090000u;
        c.A1 = c.A1 + 0xFC8u;
        mem.WriteU32(c.SP + 0x28u, c.T0);
        c.V1 = mem.ReadU32(c.V0 + 0x10u);
        c.A0 = mem.ReadU32(c.A1);
        c.V1 = c.V1 + 0xCu;
        c.S1 = c.V1 + c.A0;
        c.V1 = mem.ReadU32(c.SP + 0x28u);
        c.V0 = mem.ReadU32(c.V0 + 0x8u);
        c.T0 = c.T0 - 0x1u;
        mem.WriteU32(c.SP + 0x28u, c.T0);
        c.V0 = c.V0 + 0xCu;
        if (c.V1 == 0u) {
            c.S7 = c.V0 + c.A0;
            goto L8001DDE8;
        }
        c.S7 = c.V0 + c.A0;
        c.S6 = c.A1 + 0x1E8u;
        c.FP = 0x80050000u;
        c.FP = c.FP + 0x7B5Fu;
        c.V0 = c.S0 << 16;
        c.V0 = (uint)((int)c.V0 >> 16);
        mem.WriteU32(c.SP + 0x30u, c.V0);
        L8001D7BC: ;
        Poll(c, mem);
        c.T0 = mem.ReadU32(c.S1);
        c.S1 = c.S1 + 0x4u;
        c.V0 = 0u | 0x0034u;
        c.V1 = c.T0 >> 24;
        if (c.V1 == c.V0) {
            mem.WriteU32(c.SP + 0x20u, c.T0);
            goto L8001D80C;
        }
        mem.WriteU32(c.SP + 0x20u, c.T0);
        c.V0 = (int)c.V1 < 53 ? 1u : 0u;
        if (c.V0 == 0u) {
            c.V0 = 0u | 0x0024u;
            goto L8001D7F8;
        }
        c.V0 = 0u | 0x0024u;
        if (c.V1 == c.V0) {
            c.V0 = 0u | 0x002Cu;
            goto L8001DACC;
        }
        c.V0 = 0u | 0x002Cu;
        if (c.V1 == c.V0) {
            goto L8001DC50;
        }
        goto L8001DDBC;
        L8001D7F8: ;
        c.V0 = 0u | 0x003Cu;
        if (c.V1 == c.V0) {
            goto L8001D978;
        }
        goto L8001DDBC;
        L8001D80C: ;
        c.V0 = mem.ReadU16(c.S1 + 0xEu);
        c.V1 = mem.ReadU16(c.S1 + 0x16u);
        c.S2 = c.V0 + c.S6;
        c.S4 = c.V1 + c.S6;
        c.V0 = mem.ReadU16(c.S1 + 0x12u);
        c.A0 = mem.ReadU32(c.S2);
        c.A2 = mem.ReadU32(c.S4);
        c.S3 = c.V0 + c.S6;
        c.A1 = mem.ReadU32(c.S3);
        c.RA = 0x8001D838u;
        KingsField1_game.func_8004DE4C(c, mem);
        if ((int)c.V0 <= 0) {
            goto L8001DDBC;
        }
        c.V1 = 0x80070000u;
        c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
        c.S0 = mem.ReadU32(c.V1 + 0x8u);
        c.V0 = c.S0 + 0x28u;
        mem.WriteU32(c.V1 + 0x8u, c.V0);
        c.V0 = 0x80070000u;
        c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
        c.V1 = mem.ReadU32(c.V0 + 0x8u);
        c.V0 = mem.ReadU32(c.V0 + 0x4u);
        c.V0 = c.V0 < c.V1 ? 1u : 0u;
        if (c.V0 != 0u) {
            goto L8001DDE8;
        }
        c.A0 = c.S0;
        c.RA = 0x8001D888u;
        KingsField1_game.SetPolyGT3_game(c, mem);
        c.V0 = mem.ReadU16(c.S6 + 0x3EA8u);
        c.V1 = mem.ReadU8(c.S0 + 0x7u);
        mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S6 + 0x3EAAu);
        mem.WriteU16(c.S0 + 0x1Au, (ushort)c.V0);
        c.V0 = mem.ReadU32(c.S2);
        mem.WriteU32(c.S0 + 0x8u, c.V0);
        c.V0 = mem.ReadU32(c.S3);
        mem.WriteU32(c.S0 + 0x14u, c.V0);
        c.V0 = mem.ReadU32(c.S4);
        mem.WriteU32(c.S0 + 0x20u, c.V0);
        c.V0 = mem.ReadU16(c.S1);
        mem.WriteU16(c.S0 + 0xCu, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x4u);
        mem.WriteU16(c.S0 + 0x18u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x8u);
        c.A3 = c.FP - 0x3u;
        mem.WriteU16(c.S0 + 0x24u, (ushort)c.V0);
        mem.WriteU8(c.FP, (byte)c.V1);
        c.A0 = mem.ReadU16(c.S1 + 0xCu);
        c.A1 = mem.ReadU16(c.S1 + 0x10u);
        c.A2 = mem.ReadU16(c.S1 + 0x14u);
        c.V1 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
        c.V0 = c.S0 + 0x4u;
        mem.WriteU32(c.SP + 0x14u, c.V0);
        c.V0 = c.S0 + 0x10u;
        mem.WriteU32(c.SP + 0x18u, c.V0);
        c.V0 = c.S0 + 0x1Cu;
        mem.WriteU32(c.SP + 0x1Cu, c.V0);
        mem.WriteU32(c.SP + 0x10u, c.V1);
        c.A0 = c.S7 + c.A0;
        c.A1 = c.S7 + c.A1;
        c.A2 = c.S7 + c.A2;
        c.RA = 0x8001D928u;
        KingsField1_game.func_8004DD40(c, mem);
        c.T0 = 0u | 0x0003u;
        c.V0 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
        c.V1 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
        c.A0 = (uint)(short)mem.ReadU16(c.S4 + 0x4u);
        c.V0 = c.V0 + c.V1;
        c.V0 = c.V0 + c.A0;
        Div(c, c.V0, c.T0); // at = 0x80000000 after
        c.V0 = c.LO;
        c.T0 = mem.ReadU32(c.SP + 0x30u);
        c.V0 = (uint)((int)c.V0 >> 2);
        goto L8001DD98;
        L8001D978: ;
        c.V0 = mem.ReadU16(c.S1 + 0x12u);
        c.V1 = mem.ReadU16(c.S1 + 0x1Au);
        c.S2 = c.V0 + c.S6;
        c.S4 = c.V1 + c.S6;
        c.V0 = mem.ReadU16(c.S1 + 0x16u);
        c.A0 = mem.ReadU32(c.S2);
        c.A2 = mem.ReadU32(c.S4);
        c.S3 = c.V0 + c.S6;
        c.A1 = mem.ReadU32(c.S3);
        c.RA = 0x8001D9A4u;
        KingsField1_game.func_8004DE4C(c, mem);
        if ((int)c.V0 <= 0) {
            goto L8001DDBC;
        }
        c.V1 = 0x80070000u;
        c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
        c.S0 = mem.ReadU32(c.V1 + 0x8u);
        c.A0 = mem.ReadU16(c.S1 + 0x1Eu);
        c.V0 = c.S0 + 0x34u;
        mem.WriteU32(c.V1 + 0x8u, c.V0);
        c.V0 = 0x80070000u;
        c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
        c.V1 = mem.ReadU32(c.V0 + 0x8u);
        c.V0 = mem.ReadU32(c.V0 + 0x4u);
        c.V0 = c.V0 < c.V1 ? 1u : 0u;
        if (c.V0 != 0u) {
            c.S5 = c.A0 + c.S6;
            goto L8001DDE8;
        }
        c.S5 = c.A0 + c.S6;
        c.A0 = c.S0;
        c.RA = 0x8001D9F4u;
        KingsField1_game.SetPolyGT4_game(c, mem);
        c.V0 = mem.ReadU16(c.S6 + 0x3EA8u);
        c.V1 = mem.ReadU8(c.S0 + 0x7u);
        mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S6 + 0x3EAAu);
        mem.WriteU16(c.S0 + 0x1Au, (ushort)c.V0);
        c.V0 = mem.ReadU32(c.S2);
        mem.WriteU32(c.S0 + 0x8u, c.V0);
        c.V0 = mem.ReadU32(c.S3);
        mem.WriteU32(c.S0 + 0x14u, c.V0);
        c.V0 = mem.ReadU32(c.S4);
        mem.WriteU32(c.S0 + 0x20u, c.V0);
        c.V0 = mem.ReadU32(c.S5);
        mem.WriteU32(c.S0 + 0x2Cu, c.V0);
        c.V0 = mem.ReadU16(c.S1);
        mem.WriteU16(c.S0 + 0xCu, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x4u);
        mem.WriteU16(c.S0 + 0x18u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x8u);
        mem.WriteU16(c.S0 + 0x24u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0xCu);
        c.A3 = c.FP - 0x3u;
        mem.WriteU16(c.S0 + 0x30u, (ushort)c.V0);
        mem.WriteU8(c.FP, (byte)c.V1);
        c.A0 = mem.ReadU16(c.S1 + 0x10u);
        c.A1 = mem.ReadU16(c.S1 + 0x14u);
        c.A2 = mem.ReadU16(c.S1 + 0x18u);
        c.V1 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
        c.V0 = c.S0 + 0x4u;
        mem.WriteU32(c.SP + 0x14u, c.V0);
        c.V0 = c.S0 + 0x10u;
        mem.WriteU32(c.SP + 0x18u, c.V0);
        c.V0 = c.S0 + 0x1Cu;
        mem.WriteU32(c.SP + 0x1Cu, c.V0);
        mem.WriteU32(c.SP + 0x10u, c.V1);
        c.A0 = c.S7 + c.A0;
        c.A1 = c.S7 + c.A1;
        c.A2 = c.S7 + c.A2;
        c.RA = 0x8001DAACu;
        KingsField1_game.func_8004DD40(c, mem);
        c.A1 = c.FP - 0x3u;
        c.A3 = c.S0 + 0x28u;
        c.A0 = mem.ReadU16(c.S1 + 0x1Cu);
        c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
        c.A0 = c.S7 + c.A0;
        c.RA = 0x8001DAC4u;
        KingsField1_game.func_8004DD14(c, mem);
        goto L8001DD74;
        L8001DACC: ;
        c.V0 = mem.ReadU16(c.S1 + 0xEu);
        c.V1 = mem.ReadU16(c.S1 + 0x12u);
        c.S2 = c.V0 + c.S6;
        c.S4 = c.V1 + c.S6;
        c.V0 = mem.ReadU16(c.S1 + 0x10u);
        c.A0 = mem.ReadU32(c.S2);
        c.A2 = mem.ReadU32(c.S4);
        c.S3 = c.V0 + c.S6;
        c.A1 = mem.ReadU32(c.S3);
        c.RA = 0x8001DAF8u;
        KingsField1_game.func_8004DE4C(c, mem);
        if ((int)c.V0 <= 0) {
            goto L8001DDBC;
        }
        c.V1 = 0x80070000u;
        c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
        c.S0 = mem.ReadU32(c.V1 + 0x8u);
        c.V0 = c.S0 + 0x20u;
        mem.WriteU32(c.V1 + 0x8u, c.V0);
        c.V0 = 0x80070000u;
        c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
        c.V1 = mem.ReadU32(c.V0 + 0x8u);
        c.V0 = mem.ReadU32(c.V0 + 0x4u);
        c.V0 = c.V0 < c.V1 ? 1u : 0u;
        if (c.V0 != 0u) {
            goto L8001DDE8;
        }
        c.A0 = c.S0;
        c.RA = 0x8001DB48u;
        KingsField1_game.SetPolyFT3_game(c, mem);
        c.V0 = mem.ReadU16(c.S6 + 0x3EA8u);
        c.V1 = mem.ReadU8(c.S0 + 0x7u);
        mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S6 + 0x3EAAu);
        mem.WriteU16(c.S0 + 0x16u, (ushort)c.V0);
        c.V0 = mem.ReadU32(c.S2);
        mem.WriteU32(c.S0 + 0x8u, c.V0);
        c.V0 = mem.ReadU32(c.S3);
        mem.WriteU32(c.S0 + 0x10u, c.V0);
        c.V0 = mem.ReadU32(c.S4);
        mem.WriteU32(c.S0 + 0x18u, c.V0);
        c.V0 = mem.ReadU16(c.S1);
        mem.WriteU16(c.S0 + 0xCu, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x4u);
        mem.WriteU16(c.S0 + 0x14u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x8u);
        c.T0 = 0u | 0x0003u;
        mem.WriteU16(c.S0 + 0x1Cu, (ushort)c.V0);
        mem.WriteU8(c.FP, (byte)c.V1);
        c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
        c.V0 = (uint)(short)mem.ReadU16(c.S3 + 0x6u);
        c.V1 = (uint)(short)mem.ReadU16(c.S4 + 0x6u);
        c.A2 = c.A2 + c.V0;
        c.A2 = c.A2 + c.V1;
        Div(c, c.A2, c.T0); // at = 0x80000000 after
        c.A2 = c.LO;
        c.A1 = c.FP - 0x3u;
        c.A0 = mem.ReadU16(c.S1 + 0xCu);
        c.A3 = c.S0 + 0x4u;
        c.A0 = c.S7 + c.A0;
        c.RA = 0x8001DC00u;
        KingsField1_game.func_8004DD14(c, mem);
        c.T0 = 0u | 0x0003u;
        c.V0 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
        c.V1 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
        c.A0 = (uint)(short)mem.ReadU16(c.S4 + 0x4u);
        c.V0 = c.V0 + c.V1;
        c.V0 = c.V0 + c.A0;
        Div(c, c.V0, c.T0); // at = 0x80000000 after
        c.V0 = c.LO;
        c.T0 = mem.ReadU32(c.SP + 0x30u);
        c.V0 = (uint)((int)c.V0 >> 2);
        goto L8001DD98;
        L8001DC50: ;
        c.V0 = mem.ReadU16(c.S1 + 0x12u);
        c.V1 = mem.ReadU16(c.S1 + 0x16u);
        c.S2 = c.V0 + c.S6;
        c.S4 = c.V1 + c.S6;
        c.V0 = mem.ReadU16(c.S1 + 0x14u);
        c.A0 = mem.ReadU32(c.S2);
        c.A2 = mem.ReadU32(c.S4);
        c.S3 = c.V0 + c.S6;
        c.A1 = mem.ReadU32(c.S3);
        c.RA = 0x8001DC7Cu;
        KingsField1_game.func_8004DE4C(c, mem);
        if ((int)c.V0 <= 0) {
            goto L8001DDBC;
        }
        c.V1 = 0x80070000u;
        c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
        c.S0 = mem.ReadU32(c.V1 + 0x8u);
        c.A0 = mem.ReadU16(c.S1 + 0x18u);
        c.V0 = c.S0 + 0x28u;
        mem.WriteU32(c.V1 + 0x8u, c.V0);
        c.V0 = 0x80070000u;
        c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
        c.V1 = mem.ReadU32(c.V0 + 0x8u);
        c.V0 = mem.ReadU32(c.V0 + 0x4u);
        c.V0 = c.V0 < c.V1 ? 1u : 0u;
        if (c.V0 != 0u) {
            c.S5 = c.A0 + c.S6;
            goto L8001DDE8;
        }
        c.S5 = c.A0 + c.S6;
        c.A0 = c.S0;
        c.RA = 0x8001DCCCu;
        KingsField1_game.SetPolyFT4_game(c, mem);
        c.V0 = mem.ReadU16(c.S6 + 0x3EA8u);
        c.V1 = mem.ReadU8(c.S0 + 0x7u);
        mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S6 + 0x3EAAu);
        mem.WriteU16(c.S0 + 0x16u, (ushort)c.V0);
        c.V0 = mem.ReadU32(c.S2);
        mem.WriteU32(c.S0 + 0x8u, c.V0);
        c.V0 = mem.ReadU32(c.S3);
        mem.WriteU32(c.S0 + 0x10u, c.V0);
        c.V0 = mem.ReadU32(c.S4);
        mem.WriteU32(c.S0 + 0x18u, c.V0);
        c.V0 = mem.ReadU32(c.S5);
        mem.WriteU32(c.S0 + 0x20u, c.V0);
        c.V0 = mem.ReadU16(c.S1);
        mem.WriteU16(c.S0 + 0xCu, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x4u);
        mem.WriteU16(c.S0 + 0x14u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x8u);
        c.A1 = c.FP - 0x3u;
        mem.WriteU16(c.S0 + 0x1Cu, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0xCu);
        c.A3 = c.S0 + 0x4u;
        mem.WriteU16(c.S0 + 0x24u, (ushort)c.V0);
        mem.WriteU8(c.FP, (byte)c.V1);
        c.A0 = mem.ReadU16(c.S1 + 0x10u);
        c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
        c.V0 = (uint)(short)mem.ReadU16(c.S3 + 0x6u);
        c.V1 = (uint)(short)mem.ReadU16(c.S5 + 0x6u);
        c.A2 = c.A2 + c.V0;
        c.V0 = (uint)(short)mem.ReadU16(c.S4 + 0x6u);
        c.A0 = c.S7 + c.A0;
        c.A2 = c.A2 + c.V0;
        c.A2 = c.A2 + c.V1;
        c.A2 = (uint)((int)c.A2 >> 2);
        c.RA = 0x8001DD74u;
        KingsField1_game.func_8004DD14(c, mem);
        L8001DD74: ;
        c.V0 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
        c.V1 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
        c.A0 = (uint)(short)mem.ReadU16(c.S5 + 0x4u);
        c.V0 = c.V0 + c.V1;
        c.V1 = (uint)(short)mem.ReadU16(c.S4 + 0x4u);
        c.T0 = mem.ReadU32(c.SP + 0x30u);
        c.V0 = c.V0 + c.V1;
        c.V0 = c.V0 + c.A0;
        c.V0 = (uint)((int)c.V0 >> 4);
        L8001DD98: ;
        c.A0 = c.V0 + c.T0;
        c.V0 = (int)c.A0 < 5 ? 1u : 0u;
        if (c.V0 != 0u) {
            c.A1 = c.S0;
            goto L8001DDBC;
        }
        c.A1 = c.S0;
        c.A0 = c.A0 & 0x3FFFu;
        c.V0 = mem.ReadU32(c.S6 - 0x2F4u);
        c.A0 = c.A0 << 2;
        c.A0 = c.A0 + c.V0;
        c.RA = 0x8001DDBCu;
        KingsField1_game.AddPrim_game(c, mem);
        L8001DDBC: ;
        c.T0 = mem.ReadU32(c.SP + 0x20u);
        c.V0 = c.T0 >> 6;
        c.V0 = c.V0 & 0x03FCu;
        c.S1 = c.S1 + c.V0;
        c.V0 = mem.ReadU32(c.SP + 0x28u);
        c.T0 = c.V0;
        c.T0 = c.T0 - 0x1u;
        if (c.V0 != 0u) {
            mem.WriteU32(c.SP + 0x28u, c.T0);
            goto L8001D7BC;
        }
        mem.WriteU32(c.SP + 0x28u, c.T0);
        L8001DDE8: ;
        c.RA = mem.ReadU32(c.SP + 0x5Cu);
        c.FP = mem.ReadU32(c.SP + 0x58u);
        c.S7 = mem.ReadU32(c.SP + 0x54u);
        c.S6 = mem.ReadU32(c.SP + 0x50u);
        c.S5 = mem.ReadU32(c.SP + 0x4Cu);
        c.S4 = mem.ReadU32(c.SP + 0x48u);
        c.S3 = mem.ReadU32(c.SP + 0x44u);
        c.S2 = mem.ReadU32(c.SP + 0x40u);
        c.S1 = mem.ReadU32(c.SP + 0x3Cu);
        c.S0 = mem.ReadU32(c.SP + 0x38u);
        c.SP = c.SP + 0x60u;
        return;
    }
}
