using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_800205D4</c>, the pose blender, transcribed literally. <c>KF1_POSEBLEND</c>.
/// </summary>
public static class PoseBlend
{
    public static readonly Literal R = new(typeof(PoseBlend), "pose blender", "POSEBLEND", 0x800205D4, Run,
        "Blends a model's pose from its clip's keys.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x48u;
        mem.WriteU32(c.SP + 0x44u, c.RA);
        mem.WriteU32(c.SP + 0x40u, c.FP);
        mem.WriteU32(c.SP + 0x3Cu, c.S7);
        mem.WriteU32(c.SP + 0x38u, c.S6);
        mem.WriteU32(c.SP + 0x34u, c.S5);
        mem.WriteU32(c.SP + 0x30u, c.S4);
        mem.WriteU32(c.SP + 0x2Cu, c.S3);
        mem.WriteU32(c.SP + 0x28u, c.S2);
        mem.WriteU32(c.SP + 0x24u, c.S1);
        mem.WriteU32(c.SP + 0x20u, c.S0);
        c.S1 = c.A0;
        c.S4 = mem.ReadU32(c.S1);
        c.FP = c.A1;
        mem.WriteU16(c.SP + 0x10u, (ushort)c.A2);
        c.S0 = c.FP & 0xFFFFu;
        c.V0 = c.S0 << 2;
        c.T2 = mem.ReadU16(c.SP + 0x58u);
        c.At = 0x80090000u;
        c.At = c.At + 0xFCCu;
        c.At = c.At + c.V0;
        c.S2 = mem.ReadU32(c.At);
        mem.WriteU16(c.SP + 0x18u, (ushort)c.T2);
        c.V0 = mem.ReadU32(c.S2 + 0x4u);
        if (c.V0 != 0u) {
            c.S6 = c.A3;
            goto L80020668;
        }
        c.S6 = c.A3;
        if (c.S4 == 0u) {
            goto L80020650;
        }
        c.A0 = c.S4;
        c.RA = 0x80020650u;
        KingsField1_game.func_800209E4(c, mem);
        L80020650: ;
        c.A0 = c.S0;
        c.RA = 0x80020658u;
        KingsField1_game.func_8002059C(c, mem);
        c.A0 = 0u;
        c.RA = 0x80020660u;
        KingsField1_game.func_8001C148(c, mem);
        c.V0 = 0u | 0x0001u;
        goto L80020948;
        L80020668: ;
        if (c.S4 != 0u) {
            goto L800206B8;
        }
        c.RA = 0x80020678u;
        KingsField1_game.func_80020B04(c, mem);
        c.S4 = c.V0;
        if (c.S4 == 0u) {
            c.V0 = 0u;
            goto L80020948;
        }
        c.V0 = 0u;
        L80020684: ;
        Poll(c, mem);
        mem.WriteU16(c.S4 + 0x2u, (ushort)c.FP);
        mem.WriteU32(c.S4 + 0x10u, c.S1);
        L8002068C: ;
        Poll(c, mem);
        c.T2 = mem.ReadU16(c.SP + 0x18u);
        c.A0 = c.T2 << 3;
        c.RA = 0x80020698u;
        KingsField1_game.func_8001AAB0(c, mem);
        if (c.V0 != 0u) {
            mem.WriteU32(c.S4 + 0xCu, c.V0);
            goto L800206B0;
        }
        mem.WriteU32(c.S4 + 0xCu, c.V0);
        c.RA = 0x800206A8u;
        KingsField1_game.func_80020A2C(c, mem);
        goto L8002068C;
        L800206B0: ;
        mem.WriteU32(c.S1, c.S4);
        goto L800206DC;
        L800206B8: ;
        c.V0 = mem.ReadU16(c.S4 + 0x2u);
        if (c.V0 == c.S0) {
            c.A0 = 0u;
            goto L800206E0;
        }
        c.A0 = 0u;
        c.A0 = c.S4;
        c.RA = 0x800206D0u;
        KingsField1_game.func_800209E4(c, mem);
        c.V0 = 0u | 0x00FFu;
        mem.WriteU16(c.S4 + 0x4u, (ushort)c.V0);
        goto L80020684;
        L800206DC: ;
        c.A0 = 0u;
        L800206E0: ;
        c.V1 = mem.ReadU32(c.S2 + 0x10u);
        c.V0 = mem.ReadU16(c.SP + 0x10u);
        c.V1 = c.S2 + c.V1;
        c.V0 = c.V0 << 2;
        c.V0 = c.V0 + c.V1;
        c.V0 = mem.ReadU32(c.V0);
        c.A2 = 0u;
        c.V0 = c.S2 + c.V0;
        c.V1 = mem.ReadU16(c.V0);
        c.A1 = c.V0 + 0x4u;
        c.V0 = c.V1;
        c.V0 = c.V0 & 0xFFFFu;
        if (c.V0 == 0u) {
            c.V1 = c.V1 - 0x1u;
            goto L800207A4;
        }
        c.V1 = c.V1 - 0x1u;
        c.A3 = c.S6 & 0xFFFFu;
        c.T1 = 0u | 0x1000u;
        c.T0 = 0u | 0xFFFFu;
        L80020724: ;
        Poll(c, mem);
        c.V0 = mem.ReadU32(c.A1);
        c.S3 = c.S2 + c.V0;
        c.V0 = mem.ReadU16(c.S3 + 0x2u);
        c.V0 = c.A0 + c.V0;
        c.A0 = c.V0;
        c.V0 = c.A0 & 0xFFFFu;
        c.V0 = c.A3 < c.V0 ? 1u : 0u;
        if (c.V0 == 0u) {
            c.A1 = c.A1 + 0x4u;
            goto L8002078C;
        }
        c.A1 = c.A1 + 0x4u;
        c.V0 = c.S6 - c.A2;
        c.V0 = c.V0 & 0xFFFFu;
        c.V1 = mem.ReadU16(c.S3 + 0x2u);
        c.V0 = c.V0 << 12;
        DivU(c, c.V0, c.V1);
        c.V1 = c.LO;
        c.V0 = mem.ReadU16(c.S3);
        if (c.V0 == 0u) {
            c.S7 = c.V1;
            goto L800207AC;
        }
        c.S7 = c.V1;
        c.S7 = c.T1 - c.V1;
        goto L800207AC;
        L8002078C: ;
        c.A2 = c.A0;
        c.S5 = c.S5 + 0x1u;
        c.V0 = c.V1;
        c.V0 = c.V0 & 0xFFFFu;
        if (c.V0 != 0u) {
            c.V1 = c.V1 + c.T0;
            goto L80020724;
        }
        c.V1 = c.V1 + c.T0;
        L800207A4: ;
        c.S5 = c.S5 - 0x1u;
        c.S7 = 0u | 0x1000u;
        L800207AC: ;
        c.V1 = mem.ReadU16(c.S4 + 0x4u);
        c.V0 = mem.ReadU16(c.SP + 0x10u);
        if (c.V1 != c.V0) {
            c.A0 = c.FP & 0xFFFFu;
            goto L800207D0;
        }
        c.A0 = c.FP & 0xFFFFu;
        c.V1 = mem.ReadU16(c.S4 + 0x6u);
        c.V0 = c.S5 & 0xFFFFu;
        if (c.V1 == c.V0) {
            goto L800208A8;
        }
        L800207D0: ;
        c.V0 = mem.ReadU32(c.S2 + 0xCu);
        c.S6 = c.S2 + c.V0;
        c.RA = 0x800207DCu;
        KingsField1_game.func_8002059C(c, mem);
        c.A0 = 0u;
        c.RA = 0x800207E4u;
        KingsField1_game.func_8001C148(c, mem);
        c.A2 = 0u | 0xFFFFu;
        c.A1 = mem.ReadU16(c.SP + 0x18u);
        c.A0 = 0x80090000u;
        c.A0 = mem.ReadU32(c.A0 + 0x10BCu);
        c.V1 = mem.ReadU32(c.S4 + 0xCu);
        L800207F8: ;
        Poll(c, mem);
        c.V0 = mem.ReadU32(c.A0);
        c.A0 = c.A0 + 0x4u;
        c.A1 = c.A1 + c.A2;
        mem.WriteU32(c.V1, c.V0);
        c.V0 = mem.ReadU32(c.A0);
        c.V1 = c.V1 + 0x4u;
        c.A0 = c.A0 + 0x4u;
        mem.WriteU32(c.V1, c.V0);
        c.V0 = c.A1 & 0xFFFFu;
        if (c.V0 != 0u) {
            c.V1 = c.V1 + 0x4u;
            goto L800207F8;
        }
        c.V1 = c.V1 + 0x4u;
        c.S0 = mem.ReadU16(c.S3 + 0x6u);
        c.S1 = c.S3 + 0x8u;
        c.V0 = c.S0;
        c.V0 = c.V0 & 0xFFFFu;
        if (c.V0 == 0u) {
            c.S0 = c.S0 - 0x1u;
            goto L80020888;
        }
        c.S0 = c.S0 - 0x1u;
        c.FP = 0u | 0xFFFFu;
        L80020840: ;
        Poll(c, mem);
        c.V0 = mem.ReadU16(c.S1);
        c.S1 = c.S1 + 0x2u;
        c.V0 = c.V0 << 2;
        c.V0 = c.V0 + c.S6;
        c.V0 = mem.ReadU32(c.V0);
        c.A3 = 0u | 0x1000u;
        c.V0 = c.S2 + c.V0;
        c.A2 = mem.ReadU32(c.V0 + 0x8u);
        c.A0 = mem.ReadU32(c.V0 + 0x4u);
        c.A1 = c.V0 + 0xCu;
        c.V0 = mem.ReadU32(c.S4 + 0xCu);
        c.A0 = c.A0 << 3;
        c.A0 = c.A0 + c.V0;
        c.RA = 0x80020878u;
        KingsField1_game.gteMIMefunc_game(c, mem);
        c.V0 = c.S0;
        c.V0 = c.V0 & 0xFFFFu;
        if (c.V0 != 0u) {
            c.S0 = c.S0 + c.FP;
            goto L80020840;
        }
        c.S0 = c.S0 + c.FP;
        L80020888: ;
        c.V0 = mem.ReadU16(c.S3 + 0x4u);
        c.V0 = c.V0 << 2;
        c.V0 = c.V0 + c.S6;
        c.V0 = mem.ReadU32(c.V0);
        c.V0 = c.S2 + c.V0;
        mem.WriteU32(c.S4 + 0x8u, c.V0);
        L800208A8: ;
        c.T2 = mem.ReadU16(c.SP + 0x10u);
        mem.WriteU16(c.S4 + 0x6u, (ushort)c.S5);
        c.A1 = 0x80090000u;
        c.A1 = c.A1 + 0x30F8u;
        c.V1 = mem.ReadU32(c.S4 + 0xCu);
        c.A2 = 0u | 0xFFFFu;
        mem.WriteU16(c.S4 + 0x4u, (ushort)c.T2);
        c.A0 = mem.ReadU16(c.SP + 0x18u);
        L800208C8: ;
        Poll(c, mem);
        c.V0 = mem.ReadU32(c.V1);
        c.V1 = c.V1 + 0x4u;
        c.A0 = c.A0 + c.A2;
        mem.WriteU32(c.A1, c.V0);
        c.V0 = mem.ReadU32(c.V1);
        c.A1 = c.A1 + 0x4u;
        c.V1 = c.V1 + 0x4u;
        mem.WriteU32(c.A1, c.V0);
        c.V0 = c.A0 & 0xFFFFu;
        if (c.V0 != 0u) {
            c.A1 = c.A1 + 0x4u;
            goto L800208C8;
        }
        c.A1 = c.A1 + 0x4u;
        c.A3 = c.S7 & 0xFFFFu;
        c.V0 = mem.ReadU32(c.S4 + 0x8u);
        c.S1 = 0x80090000u;
        c.S1 = c.S1 + 0x30F0u;
        c.A1 = c.V0 + 0x4u;
        c.S0 = mem.ReadU32(c.V0 + 0x4u);
        c.A2 = mem.ReadU32(c.V0 + 0x8u);
        c.S0 = c.S0 << 3;
        c.S0 = c.S0 + c.S1;
        c.A0 = c.S0;
        c.S3 = mem.ReadU32(c.S0);
        c.S2 = mem.ReadU32(c.S0 + 0x4u);
        c.A2 = c.A2 + 0x1u;
        c.RA = 0x8002092Cu;
        KingsField1_game.gteMIMefunc_game(c, mem);
        c.A0 = c.S1 + 0x8u;
        mem.WriteU32(c.S0, c.S3);
        mem.WriteU32(c.S0 + 0x4u, c.S2);
        c.RA = 0x8002093Cu;
        KingsField1_game.func_8001C138(c, mem);
        c.V0 = 0u | 0x0002u;
        mem.WriteU16(c.S4, (ushort)c.V0);
        c.V0 = c.S4;
        L80020948: ;
        c.RA = mem.ReadU32(c.SP + 0x44u);
        c.FP = mem.ReadU32(c.SP + 0x40u);
        c.S7 = mem.ReadU32(c.SP + 0x3Cu);
        c.S6 = mem.ReadU32(c.SP + 0x38u);
        c.S5 = mem.ReadU32(c.SP + 0x34u);
        c.S4 = mem.ReadU32(c.SP + 0x30u);
        c.S3 = mem.ReadU32(c.SP + 0x2Cu);
        c.S2 = mem.ReadU32(c.SP + 0x28u);
        c.S1 = mem.ReadU32(c.SP + 0x24u);
        c.S0 = mem.ReadU32(c.SP + 0x20u);
        c.SP = c.SP + 0x48u;
        return;
    }
}
