using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_80029DE0(template, rec)</c>, the menu's text: <c>rec</c> is <c>s16 x</c>, <c>s16 y</c>,
/// then u16 glyph codes to a 0xFFFF. Each code's low 12 bits pick a 14x12 cell of the font
/// page (u = (g % 16) * 14, v = (g / 16) * 12, the template's u8 w/h added for the far
/// corners), 14 px a step, on slot 1000; bit 0x1000 adds a dakuten quad (u 196) and bit
/// 0x2000 a handakuten (u 210), both from v 24. <see cref="Translation"/>'s pre-hook writes
/// the English codes into the string before this runs. <c>KF1_MENU_TEXT</c>.
/// </summary>
public static class MenuText
{
    public static readonly Literal R = new(typeof(MenuText), "menu text", "MENU_TEXT", 0x80029DE0, Run,
        "Draws a string of glyph codes in the menu's font.", Twin);

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Twin(CpuContext c, PSMemory mem)
    {
        var (x, y, codes) = MenuDraw.ReadRecord(mem, c.A1);
        MenuDraw.Text(mem, c.A0, x, y, codes);
    }

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x30u;
        mem.WriteU32(c.SP + 0x28u, c.RA);
        mem.WriteU32(c.SP + 0x24u, c.S5);
        mem.WriteU32(c.SP + 0x20u, c.S4);
        mem.WriteU32(c.SP + 0x1Cu, c.S3);
        mem.WriteU32(c.SP + 0x18u, c.S2);
        mem.WriteU32(c.SP + 0x14u, c.S1);
        mem.WriteU32(c.SP + 0x10u, c.S0);
        c.S1 = c.A1;
        c.V1 = (uint)(short)mem.ReadU16(c.S1 + 0x4u);
        c.V0 = 0xFFFFFFFFu;
        c.S0 = c.A0;
        if (c.V1 != c.V0) {
            c.S5 = 0u | 0x00C4u;
            c.S4 = 0u | 0x0018u;
            c.S3 = c.S1;
            c.S2 = 0u;
            do {
                Poll(c, mem);
                c.RA = 0x80029E2Cu;
                KingsField1_game.func_8002ACCC(c, mem);
                c.A1 = 0x80050000u;
                c.A1 = mem.ReadU32(c.A1 + 0x7E88u);
                c.V0 = mem.ReadU16(c.S0);
                mem.WriteU16(c.A1 + 0x16u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S0 + 0x2u);
                mem.WriteU16(c.A1 + 0xEu, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1);
                c.V0 = c.V0 + c.S2;
                mem.WriteU16(c.A1 + 0x8u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0x2u);
                mem.WriteU16(c.A1 + 0xAu, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1);
                c.V1 = mem.ReadU16(c.S0 + 0x8u);
                c.V0 = c.V0 + c.S2;
                c.V0 = c.V0 + c.V1;
                mem.WriteU16(c.A1 + 0x10u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0x2u);
                mem.WriteU16(c.A1 + 0x12u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1);
                c.V0 = c.V0 + c.S2;
                mem.WriteU16(c.A1 + 0x18u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0x2u);
                c.V1 = mem.ReadU16(c.S0 + 0xAu);
                c.V0 = c.V0 + c.V1;
                mem.WriteU16(c.A1 + 0x1Au, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1);
                c.V1 = mem.ReadU16(c.S0 + 0x8u);
                c.V0 = c.V0 + c.S2;
                c.V0 = c.V0 + c.V1;
                mem.WriteU16(c.A1 + 0x20u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0x2u);
                c.V1 = mem.ReadU16(c.S0 + 0xAu);
                c.V0 = c.V0 + c.V1;
                mem.WriteU16(c.A1 + 0x22u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S3 + 0x4u);
                c.A0 = c.V0 & 0x0FFFu;
                c.V1 = c.A0;
                if ((int)c.A0 < 0) {
                    c.V1 = c.A0 + 0xFu;
                }
                c.V1 = c.V1 & 0x1FF0u;
                c.V1 = c.A0 - c.V1;
                c.V0 = c.V1 << 3;
                c.V0 = c.V0 - c.V1;
                c.V0 = c.V0 << 1;
                mem.WriteU8(c.A1 + 0xCu, (byte)c.V0);
                c.A1 = 0x80050000u;
                c.A1 = mem.ReadU32(c.A1 + 0x7E88u);
                c.V1 = c.A0;
                if ((int)c.A0 < 0) {
                    c.V1 = c.A0 + 0xFu;
                }
                c.V1 = (uint)((int)c.V1 >> 4);
                c.V0 = c.V1 << 1;
                c.V0 = c.V0 + c.V1;
                c.V0 = c.V0 << 2;
                mem.WriteU8(c.A1 + 0xDu, (byte)c.V0);
                c.A1 = 0x80050000u;
                c.A1 = mem.ReadU32(c.A1 + 0x7E88u);
                c.V0 = c.A0;
                if ((int)c.A0 < 0) {
                    c.V0 = c.A0 + 0xFu;
                }
                c.V0 = c.V0 & 0x1FF0u;
                c.V0 = c.A0 - c.V0;
                c.V1 = c.V0 << 3;
                c.V1 = c.V1 - c.V0;
                c.V0 = mem.ReadU8(c.S0 + 0x8u);
                c.V1 = c.V1 << 1;
                c.V1 = c.V1 + c.V0;
                mem.WriteU8(c.A1 + 0x14u, (byte)c.V1);
                c.A1 = 0x80050000u;
                c.A1 = mem.ReadU32(c.A1 + 0x7E88u);
                c.V1 = c.A0;
                if ((int)c.A0 < 0) {
                    c.V1 = c.A0 + 0xFu;
                }
                c.V1 = (uint)((int)c.V1 >> 4);
                c.V0 = c.V1 << 1;
                c.V0 = c.V0 + c.V1;
                c.V0 = c.V0 << 2;
                mem.WriteU8(c.A1 + 0x15u, (byte)c.V0);
                c.A1 = 0x80050000u;
                c.A1 = mem.ReadU32(c.A1 + 0x7E88u);
                c.V1 = c.A0;
                if ((int)c.A0 < 0) {
                    c.V1 = c.A0 + 0xFu;
                }
                c.V1 = c.V1 & 0x1FF0u;
                c.V1 = c.A0 - c.V1;
                c.V0 = c.V1 << 3;
                c.V0 = c.V0 - c.V1;
                c.V0 = c.V0 << 1;
                mem.WriteU8(c.A1 + 0x1Cu, (byte)c.V0);
                c.A1 = 0x80050000u;
                c.A1 = mem.ReadU32(c.A1 + 0x7E88u);
                c.V0 = c.A0;
                if ((int)c.A0 < 0) {
                    c.V0 = c.A0 + 0xFu;
                }
                c.V0 = (uint)((int)c.V0 >> 4);
                c.V1 = c.V0 << 1;
                c.V1 = c.V1 + c.V0;
                c.V0 = mem.ReadU8(c.S0 + 0xAu);
                c.V1 = c.V1 << 2;
                c.V1 = c.V1 + c.V0;
                mem.WriteU8(c.A1 + 0x1Du, (byte)c.V1);
                c.A1 = 0x80050000u;
                c.A1 = mem.ReadU32(c.A1 + 0x7E88u);
                c.V0 = c.A0;
                if ((int)c.A0 < 0) {
                    c.V0 = c.A0 + 0xFu;
                }
                c.V0 = c.V0 & 0x1FF0u;
                c.V0 = c.A0 - c.V0;
                c.V1 = c.V0 << 3;
                c.V1 = c.V1 - c.V0;
                c.V0 = mem.ReadU8(c.S0 + 0x8u);
                c.V1 = c.V1 << 1;
                c.V1 = c.V1 + c.V0;
                mem.WriteU8(c.A1 + 0x24u, (byte)c.V1);
                c.A1 = 0x80050000u;
                c.A1 = mem.ReadU32(c.A1 + 0x7E88u);
                c.V0 = c.A0;
                c.A0 = 0u | 0x03E8u;
                if ((int)c.V0 < 0) {
                    c.V0 = c.V0 + 0xFu;
                }
                c.V1 = (uint)((int)c.V0 >> 4);
                c.V0 = c.V1 << 1;
                c.V0 = c.V0 + c.V1;
                c.V1 = mem.ReadU8(c.S0 + 0xAu);
                c.V0 = c.V0 << 2;
                c.V0 = c.V0 + c.V1;
                mem.WriteU8(c.A1 + 0x25u, (byte)c.V0);
                c.RA = 0x8002A050u;
                KingsField1_game.func_8002AD1C(c, mem);
                c.V0 = mem.ReadU16(c.S3 + 0x4u);
                c.V0 = c.V0 & 0x1000u;
                if (c.V0 != 0u) {
                    c.RA = 0x8002A06Cu;
                    KingsField1_game.func_8002ACCC(c, mem);
                    c.A0 = 0x80050000u;
                    c.A0 = mem.ReadU32(c.A0 + 0x7E88u);
                    c.V0 = mem.ReadU16(c.S0);
                    mem.WriteU16(c.A0 + 0x16u, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S0 + 0x2u);
                    mem.WriteU16(c.A0 + 0xEu, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1);
                    c.V0 = c.V0 + c.S2;
                    mem.WriteU16(c.A0 + 0x8u, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1 + 0x2u);
                    mem.WriteU16(c.A0 + 0xAu, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1);
                    c.V1 = mem.ReadU16(c.S0 + 0x8u);
                    c.V0 = c.V0 + c.S2;
                    c.V0 = c.V0 + c.V1;
                    mem.WriteU16(c.A0 + 0x10u, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1 + 0x2u);
                    mem.WriteU16(c.A0 + 0x12u, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1);
                    c.V0 = c.V0 + c.S2;
                    mem.WriteU16(c.A0 + 0x18u, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1 + 0x2u);
                    c.V1 = mem.ReadU16(c.S0 + 0xAu);
                    c.V0 = c.V0 + c.V1;
                    mem.WriteU16(c.A0 + 0x1Au, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1);
                    c.V1 = mem.ReadU16(c.S0 + 0x8u);
                    c.V0 = c.V0 + c.S2;
                    c.V0 = c.V0 + c.V1;
                    mem.WriteU16(c.A0 + 0x20u, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1 + 0x2u);
                    c.V1 = mem.ReadU16(c.S0 + 0xAu);
                    mem.WriteU8(c.A0 + 0xCu, (byte)c.S5);
                    c.A1 = 0x80050000u;
                    c.A1 = mem.ReadU32(c.A1 + 0x7E88u);
                    c.V0 = c.V0 + c.V1;
                    mem.WriteU16(c.A0 + 0x22u, (ushort)c.V0);
                    mem.WriteU8(c.A1 + 0xDu, (byte)c.S4);
                    c.V0 = mem.ReadU8(c.S0 + 0x8u);
                    c.V1 = 0x80050000u;
                    c.V1 = mem.ReadU32(c.V1 + 0x7E88u);
                    c.V0 = c.V0 - 0x3Cu;
                    mem.WriteU8(c.V1 + 0x14u, (byte)c.V0);
                    c.V0 = 0x80050000u;
                    c.V0 = mem.ReadU32(c.V0 + 0x7E88u);
                    mem.WriteU8(c.V0 + 0x15u, (byte)c.S4);
                    c.V0 = 0x80050000u;
                    c.V0 = mem.ReadU32(c.V0 + 0x7E88u);
                    c.A0 = 0u | 0x03E8u;
                    mem.WriteU8(c.V0 + 0x1Cu, (byte)c.S5);
                    c.V0 = mem.ReadU8(c.S0 + 0xAu);
                    c.V1 = 0x80050000u;
                    c.V1 = mem.ReadU32(c.V1 + 0x7E88u);
                    c.V0 = c.V0 + 0x18u;
                    mem.WriteU8(c.V1 + 0x1Du, (byte)c.V0);
                    c.V0 = mem.ReadU8(c.S0 + 0x8u);
                    c.V1 = 0x80050000u;
                    c.V1 = mem.ReadU32(c.V1 + 0x7E88u);
                    c.V0 = c.V0 - 0x3Cu;
                    mem.WriteU8(c.V1 + 0x24u, (byte)c.V0);
                    c.V0 = mem.ReadU8(c.S0 + 0xAu);
                    c.V1 = 0x80050000u;
                    c.V1 = mem.ReadU32(c.V1 + 0x7E88u);
                    c.V0 = c.V0 + 0x18u;
                    mem.WriteU8(c.V1 + 0x25u, (byte)c.V0);
                    c.RA = 0x8002A194u;
                    KingsField1_game.func_8002AD1C(c, mem);
                }
                c.V0 = mem.ReadU16(c.S3 + 0x4u);
                c.V0 = c.V0 & 0x2000u;
                c.S3 = c.S3 + 0x2u;
                if (c.V0 != 0u) {
                    c.RA = 0x8002A1B0u;
                    KingsField1_game.func_8002ACCC(c, mem);
                    c.A0 = 0x80050000u;
                    c.A0 = mem.ReadU32(c.A0 + 0x7E88u);
                    c.V0 = mem.ReadU16(c.S0);
                    mem.WriteU16(c.A0 + 0x16u, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S0 + 0x2u);
                    mem.WriteU16(c.A0 + 0xEu, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1);
                    c.V0 = c.V0 + c.S2;
                    mem.WriteU16(c.A0 + 0x8u, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1 + 0x2u);
                    mem.WriteU16(c.A0 + 0xAu, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1);
                    c.V1 = mem.ReadU16(c.S0 + 0x8u);
                    c.V0 = c.V0 + c.S2;
                    c.V0 = c.V0 + c.V1;
                    mem.WriteU16(c.A0 + 0x10u, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1 + 0x2u);
                    mem.WriteU16(c.A0 + 0x12u, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1);
                    c.V0 = c.V0 + c.S2;
                    mem.WriteU16(c.A0 + 0x18u, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1 + 0x2u);
                    c.V1 = mem.ReadU16(c.S0 + 0xAu);
                    c.V0 = c.V0 + c.V1;
                    mem.WriteU16(c.A0 + 0x1Au, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1);
                    c.V1 = mem.ReadU16(c.S0 + 0x8u);
                    c.V0 = c.V0 + c.S2;
                    c.V0 = c.V0 + c.V1;
                    mem.WriteU16(c.A0 + 0x20u, (ushort)c.V0);
                    c.V0 = mem.ReadU16(c.S1 + 0x2u);
                    c.A1 = mem.ReadU16(c.S0 + 0xAu);
                    c.A2 = 0u | 0x00D2u;
                    mem.WriteU8(c.A0 + 0xCu, (byte)c.A2);
                    c.V1 = 0x80050000u;
                    c.V1 = mem.ReadU32(c.V1 + 0x7E88u);
                    c.V0 = c.V0 + c.A1;
                    mem.WriteU16(c.A0 + 0x22u, (ushort)c.V0);
                    mem.WriteU8(c.V1 + 0xDu, (byte)c.S4);
                    c.V0 = mem.ReadU8(c.S0 + 0x8u);
                    c.V1 = 0x80050000u;
                    c.V1 = mem.ReadU32(c.V1 + 0x7E88u);
                    c.V0 = c.V0 - 0x2Eu;
                    mem.WriteU8(c.V1 + 0x14u, (byte)c.V0);
                    c.V0 = 0x80050000u;
                    c.V0 = mem.ReadU32(c.V0 + 0x7E88u);
                    mem.WriteU8(c.V0 + 0x15u, (byte)c.S4);
                    c.V0 = 0x80050000u;
                    c.V0 = mem.ReadU32(c.V0 + 0x7E88u);
                    c.A0 = 0u | 0x03E8u;
                    mem.WriteU8(c.V0 + 0x1Cu, (byte)c.A2);
                    c.V0 = mem.ReadU8(c.S0 + 0xAu);
                    c.V1 = 0x80050000u;
                    c.V1 = mem.ReadU32(c.V1 + 0x7E88u);
                    c.V0 = c.V0 + 0x18u;
                    mem.WriteU8(c.V1 + 0x1Du, (byte)c.V0);
                    c.V0 = mem.ReadU8(c.S0 + 0x8u);
                    c.V1 = 0x80050000u;
                    c.V1 = mem.ReadU32(c.V1 + 0x7E88u);
                    c.V0 = c.V0 - 0x2Eu;
                    mem.WriteU8(c.V1 + 0x24u, (byte)c.V0);
                    c.V0 = mem.ReadU8(c.S0 + 0xAu);
                    c.V1 = 0x80050000u;
                    c.V1 = mem.ReadU32(c.V1 + 0x7E88u);
                    c.V0 = c.V0 + 0x18u;
                    mem.WriteU8(c.V1 + 0x25u, (byte)c.V0);
                    c.RA = 0x8002A2DCu;
                    KingsField1_game.func_8002AD1C(c, mem);
                }
                c.V1 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
                c.V0 = 0xFFFFFFFFu;
                c.S2 = c.S2 + 0xEu;
            } while (c.V1 != c.V0);
        }
        c.RA = mem.ReadU32(c.SP + 0x28u);
        c.S5 = mem.ReadU32(c.SP + 0x24u);
        c.S4 = mem.ReadU32(c.SP + 0x20u);
        c.S3 = mem.ReadU32(c.SP + 0x1Cu);
        c.S2 = mem.ReadU32(c.SP + 0x18u);
        c.S1 = mem.ReadU32(c.SP + 0x14u);
        c.S0 = mem.ReadU32(c.SP + 0x10u);
        c.SP = c.SP + 0x30u;
    }
}
