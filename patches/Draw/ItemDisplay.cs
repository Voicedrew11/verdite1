using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001FAFC</c>, the item display, transcribed literally. <c>KF1_ITEMDISPLAY</c>.
/// </summary>
public static class ItemDisplay
{
    public static readonly Literal R = new(typeof(ItemDisplay), "item display", "ITEMDISPLAY", 0x8001FAFC, Run,
        "Draws the selected item, turning.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x38u;
        mem.WriteU32(c.SP + 0x30u, c.RA);
        mem.WriteU32(c.SP + 0x2Cu, c.S1);
        mem.WriteU32(c.SP + 0x28u, c.S0);
        c.T0 = 0x80090000u;
        c.T0 = c.T0 + 0x5088u;
        c.V1 = mem.ReadU8(c.T0);
        c.V0 = 0u | 0x0002u;
        if (c.V1 == c.V0) {
            c.V0 = (int)c.V1 < 3 ? 1u : 0u;
            goto L8001FCA4;
        }
        c.V0 = (int)c.V1 < 3 ? 1u : 0u;
        if (c.V0 == 0u) {
            goto L8001FB3C;
        }
        if (c.V1 == 0u) {
            c.V0 = 0u | 0x00FFu;
            goto L8001FB50;
        }
        c.V0 = 0u | 0x00FFu;
        goto L8001FDB4;
        L8001FB3C: ;
        c.V0 = 0u | 0x0003u;
        if (c.V1 == c.V0) {
            goto L8001FCD0;
        }
        goto L8001FDB4;
        L8001FB50: ;
        c.A3 = 0x80090000u;
        c.A3 = mem.ReadU8(c.A3 + 0x5086u);
        c.At = 0x80090000u;
        c.At = c.At + 0x506Eu;
        c.At = c.At + c.A3;
        c.V1 = mem.ReadU8(c.At);
        c.A0 = c.V1 & 0x00FFu;
        if (c.A0 == c.V0) {
            c.V0 = 0u | 0x0002u;
            goto L8001FDB4;
        }
        c.V0 = 0u | 0x0002u;
        mem.WriteU8(c.T0, (byte)c.V0);
        c.At = 0x80090000u;
        mem.WriteU16(c.At + 0x508Au, (ushort)0u);
        c.V0 = 0u | 0x000Fu;
        c.At = 0x80090000u;
        mem.WriteU8(c.At + 0x5089u, (byte)c.V0);
        c.V0 = 0u | 0x0013u;
        if (c.A0 != c.V0) {
            c.V0 = 0u | 0x0001u;
            goto L8001FC4C;
        }
        c.V0 = 0u | 0x0001u;
        c.S1 = 0x80050000u;
        c.S1 = c.S1 + 0x5D20u;
        mem.WriteU8(c.S1, (byte)0u);
        c.S0 = 0u | 0x0001u;
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D2Eu, (byte)c.S0);
        c.V0 = c.V1 & 0x00F0u;
        c.V0 = c.V0 << 3;
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D30u, (byte)c.V0);
        c.V0 = c.V1 & 0x000Fu;
        c.V0 = c.V0 << 4;
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D31u, (byte)c.V0);
        c.A1 = 0u | 0x0004u;
        c.A2 = 0u;
        c.V0 = c.A3 << 1;
        c.V0 = c.T0 + c.V0;
        c.A0 = mem.ReadU16(c.V0 - 0x12u);
        c.A3 = c.SP + 0x10u;
        c.RA = 0x8001FBF4u;
        KingsField1_game.func_8002ADF8(c, mem);
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D3Cu, (byte)c.S0);
        c.A1 = mem.ReadU16(c.SP + 0x16u);
        c.A0 = c.S1 + 0x1Eu;
        c.RA = 0x8001FC08u;
        KingsField1_game.func_8001FAE4(c, mem);
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D4Au, (byte)c.S0);
        c.A1 = mem.ReadU16(c.SP + 0x14u);
        c.A0 = c.S1 + 0x2Cu;
        c.RA = 0x8001FC1Cu;
        KingsField1_game.func_8001FAE4(c, mem);
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D58u, (byte)c.S0);
        c.A1 = mem.ReadU16(c.SP + 0x12u);
        c.A0 = c.S1 + 0x3Au;
        c.RA = 0x8001FC30u;
        KingsField1_game.func_8001FAE4(c, mem);
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D66u, (byte)c.S0);
        c.A1 = mem.ReadU16(c.SP + 0x10u);
        c.A0 = c.S1 + 0x48u;
        c.RA = 0x8001FC44u;
        KingsField1_game.func_8001FAE4(c, mem);
        goto L8001FDB4;
        L8001FC4C: ;
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D20u, (byte)c.V0);
        c.V0 = c.V1 & 0x00F0u;
        c.V0 = c.V0 << 3;
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D22u, (byte)c.V0);
        c.V0 = c.V1 & 0x000Fu;
        c.V0 = c.V0 << 4;
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D23u, (byte)c.V0);
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D66u, (byte)0u);
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D58u, (byte)0u);
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D4Au, (byte)0u);
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D3Cu, (byte)0u);
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D2Eu, (byte)0u);
        goto L8001FDB4;
        L8001FCA4: ;
        c.V0 = 0x80090000u;
        c.V0 = mem.ReadU8(c.V0 + 0x5089u);
        c.V0 = c.V0 - 0x1u;
        c.At = 0x80090000u;
        mem.WriteU8(c.At + 0x5089u, (byte)c.V0);
        c.V0 = c.V0 & 0x00FFu;
        if (c.V0 != 0u) {
            c.V0 = 0u | 0x0003u;
            goto L8001FDB4;
        }
        c.V0 = 0u | 0x0003u;
        mem.WriteU8(c.T0, (byte)c.V0);
        goto L8001FDB4;
        L8001FCD0: ;
        c.V0 = 0x80090000u;
        c.V0 = mem.ReadU16(c.V0 + 0x508Au);
        c.V0 = c.V0 + 0x80u;
        c.At = 0x80090000u;
        mem.WriteU16(c.At + 0x508Au, (ushort)c.V0);
        c.V0 = c.V0 << 16;
        c.V0 = (uint)((int)c.V0 >> 16);
        c.V0 = (int)c.V0 < 512 ? 1u : 0u;
        if (c.V0 != 0u) {
            c.V0 = 0u | 0x0200u;
            goto L8001FDB4;
        }
        c.V0 = 0u | 0x0200u;
        c.At = 0x80090000u;
        mem.WriteU16(c.At + 0x508Au, (ushort)c.V0);
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D66u, (byte)0u);
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D58u, (byte)0u);
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D4Au, (byte)0u);
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D3Cu, (byte)0u);
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D2Eu, (byte)0u);
        c.At = 0x80050000u;
        mem.WriteU8(c.At + 0x5D20u, (byte)0u);
        c.V1 = c.T0 - 0x2u;
        c.A2 = 0u | 0x00FFu;
        c.V0 = 0x80090000u;
        c.V0 = mem.ReadU8(c.V0 + 0x5086u);
        c.A1 = 0u | 0x0013u;
        c.At = 0x80090000u;
        c.At = c.At + 0x506Eu;
        c.At = c.At + c.V0;
        c.A0 = mem.ReadU8(c.At);
        L8001FD58: ;
        Poll(c, mem);
        c.V0 = mem.ReadU8(c.V1);
        c.At = 0x80090000u;
        c.At = c.At + 0x506Eu;
        c.At = c.At + c.V0;
        mem.WriteU8(c.At, (byte)c.A2);
        c.V0 = mem.ReadU8(c.V1);
        c.V0 = c.V0 + 0x1u;
        c.V0 = c.V0 & 0x0007u;
        mem.WriteU8(c.V1, (byte)c.V0);
        c.V0 = mem.ReadU8(c.V1);
        c.At = 0x80090000u;
        c.At = c.At + 0x506Eu;
        c.At = c.At + c.V0;
        c.V0 = mem.ReadU8(c.At);
        if (c.A0 != c.V0) {
            goto L8001FDB0;
        }
        if (c.A0 != c.A1) {
            goto L8001FD58;
        }
        L8001FDB0: ;
        mem.WriteU8(c.V1 + 0x2u, (byte)0u);
        L8001FDB4: ;
        c.RA = mem.ReadU32(c.SP + 0x30u);
        c.S1 = mem.ReadU32(c.SP + 0x2Cu);
        c.S0 = mem.ReadU32(c.SP + 0x28u);
        c.SP = c.SP + 0x38u;
        return;
    }
}
