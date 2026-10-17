using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8002A310(template, rec)</c>, the menu's digits: <c>rec</c> is <c>s16 x</c>,
/// <c>s16 y</c>, then u16 digits to a 0xFFFF. A digit d reads the template's u with
/// v = d * 11, 7 px a step, on slot 1000. <c>KF1_MENU_DIGITS</c>.
/// </summary>
public static class MenuDigits
{
    public static readonly Literal R = new(typeof(MenuDigits), "menu digits", "MENU_DIGITS", 0x8002A310, Run,
        "Draws a string of digits.", Twin);

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Twin(CpuContext c, PSMemory mem)
    {
        var (x, y, digits) = MenuDraw.ReadRecord(mem, c.A1);
        MenuDraw.Digits(mem, c.A0, x, y, digits);
    }

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x28u;
        mem.WriteU32(c.SP + 0x20u, c.RA);
        mem.WriteU32(c.SP + 0x1Cu, c.S3);
        mem.WriteU32(c.SP + 0x18u, c.S2);
        mem.WriteU32(c.SP + 0x14u, c.S1);
        mem.WriteU32(c.SP + 0x10u, c.S0);
        c.S1 = c.A1;
        c.V1 = (uint)(short)mem.ReadU16(c.S1 + 0x4u);
        c.V0 = 0xFFFFFFFFu;
        c.S0 = c.A0;
        if (c.V1 != c.V0) {
            c.S2 = c.S1;
            c.S3 = 0u;
            do {
                Poll(c, mem);
                c.RA = 0x8002A34Cu;
                KingsField1_game.func_8002ACCC(c, mem);
                c.A0 = 0x80050000u;
                c.A0 = mem.ReadU32(c.A0 + 0x7E88u);
                c.V0 = mem.ReadU16(c.S0);
                mem.WriteU16(c.A0 + 0x16u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S0 + 0x2u);
                mem.WriteU16(c.A0 + 0xEu, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1);
                c.V0 = c.V0 + c.S3;
                mem.WriteU16(c.A0 + 0x8u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0x2u);
                mem.WriteU16(c.A0 + 0xAu, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1);
                c.V1 = mem.ReadU16(c.S0 + 0x8u);
                c.V0 = c.V0 + c.S3;
                c.V0 = c.V0 + c.V1;
                mem.WriteU16(c.A0 + 0x10u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0x2u);
                mem.WriteU16(c.A0 + 0x12u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1);
                c.V0 = c.V0 + c.S3;
                mem.WriteU16(c.A0 + 0x18u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0x2u);
                c.V1 = mem.ReadU16(c.S0 + 0xAu);
                c.V0 = c.V0 + c.V1;
                mem.WriteU16(c.A0 + 0x1Au, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1);
                c.V1 = mem.ReadU16(c.S0 + 0x8u);
                c.V0 = c.V0 + c.S3;
                c.V0 = c.V0 + c.V1;
                mem.WriteU16(c.A0 + 0x20u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0x2u);
                c.V1 = mem.ReadU16(c.S0 + 0xAu);
                c.V0 = c.V0 + c.V1;
                mem.WriteU16(c.A0 + 0x22u, (ushort)c.V0);
                c.V0 = mem.ReadU8(c.S0 + 0x4u);
                mem.WriteU8(c.A0 + 0xCu, (byte)c.V0);
                c.V1 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                c.A0 = 0x80050000u;
                c.A0 = mem.ReadU32(c.A0 + 0x7E88u);
                c.V0 = c.V1 << 1;
                c.V0 = c.V0 + c.V1;
                c.V0 = c.V0 << 2;
                c.V0 = c.V0 - c.V1;
                mem.WriteU8(c.A0 + 0xDu, (byte)c.V0);
                c.V0 = mem.ReadU8(c.S0 + 0x4u);
                c.V1 = mem.ReadU8(c.S0 + 0x8u);
                c.A0 = 0x80050000u;
                c.A0 = mem.ReadU32(c.A0 + 0x7E88u);
                c.V0 = c.V0 + c.V1;
                mem.WriteU8(c.A0 + 0x14u, (byte)c.V0);
                c.V1 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                c.A0 = 0x80050000u;
                c.A0 = mem.ReadU32(c.A0 + 0x7E88u);
                c.V0 = c.V1 << 1;
                c.V0 = c.V0 + c.V1;
                c.V0 = c.V0 << 2;
                c.V0 = c.V0 - c.V1;
                mem.WriteU8(c.A0 + 0x15u, (byte)c.V0);
                c.V1 = 0x80050000u;
                c.V1 = mem.ReadU32(c.V1 + 0x7E88u);
                c.V0 = mem.ReadU8(c.S0 + 0x4u);
                mem.WriteU8(c.V1 + 0x1Cu, (byte)c.V0);
                c.V1 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                c.S3 = c.S3 + 0x7u;
                c.V0 = c.V1 << 1;
                c.V0 = c.V0 + c.V1;
                c.V0 = c.V0 << 2;
                c.V0 = c.V0 - c.V1;
                c.V1 = mem.ReadU8(c.S0 + 0xAu);
                c.A0 = 0x80050000u;
                c.A0 = mem.ReadU32(c.A0 + 0x7E88u);
                c.V0 = c.V0 + c.V1;
                mem.WriteU8(c.A0 + 0x1Du, (byte)c.V0);
                c.V0 = mem.ReadU8(c.S0 + 0x4u);
                c.V1 = mem.ReadU8(c.S0 + 0x8u);
                c.A0 = 0x80050000u;
                c.A0 = mem.ReadU32(c.A0 + 0x7E88u);
                c.V0 = c.V0 + c.V1;
                mem.WriteU8(c.A0 + 0x24u, (byte)c.V0);
                c.V1 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                c.A0 = 0u | 0x03E8u;
                c.S2 = c.S2 + 0x2u;
                c.V0 = c.V1 << 1;
                c.V0 = c.V0 + c.V1;
                c.V0 = c.V0 << 2;
                c.V0 = c.V0 - c.V1;
                c.V1 = mem.ReadU8(c.S0 + 0xAu);
                c.A1 = 0x80050000u;
                c.A1 = mem.ReadU32(c.A1 + 0x7E88u);
                c.V0 = c.V0 + c.V1;
                mem.WriteU8(c.A1 + 0x25u, (byte)c.V0);
                c.RA = 0x8002A4E4u;
                KingsField1_game.func_8002AD1C(c, mem);
                c.V1 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                c.V0 = 0xFFFFFFFFu;
            } while (c.V1 != c.V0);
        }
        c.RA = mem.ReadU32(c.SP + 0x20u);
        c.S3 = mem.ReadU32(c.SP + 0x1Cu);
        c.S2 = mem.ReadU32(c.SP + 0x18u);
        c.S1 = mem.ReadU32(c.SP + 0x14u);
        c.S0 = mem.ReadU32(c.SP + 0x10u);
        c.SP = c.SP + 0x28u;
    }
}
