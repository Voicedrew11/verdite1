using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_80029AB0(template, pos)</c>: one semi-transparent FT4 of the 12-byte template
/// (<c>u16 tpage</c>, <c>u16 clut</c>, <c>u8 u</c> at +4, <c>u8 v</c> at +6, <c>u16 w</c> at +8,
/// <c>u16 h</c> at +0xA) at the position's x - 4, y - 3, on slot 2000. <c>KF1_MENU_PANEL</c>.
/// </summary>
public static class MenuPanel
{
    public static readonly Literal R = new(typeof(MenuPanel), "menu panel", "MENU_PANEL", 0x80029AB0, Run,
        "Draws a semi-transparent template quad at a position.", Twin);

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Twin(CpuContext c, PSMemory mem) => MenuDraw.Panel(mem, c.A0, mem.ReadU16(c.A1), mem.ReadU16(c.A1 + 2));

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x20u;
        mem.WriteU32(c.SP + 0x18u, c.RA);
        mem.WriteU32(c.SP + 0x14u, c.S1);
        mem.WriteU32(c.SP + 0x10u, c.S0);
        c.S0 = c.A0;
        c.S1 = c.A1;
        c.RA = 0x80029ACCu;
        KingsField1_game.func_8002ACCC(c, mem);
        c.A0 = 0x80050000u;
        c.A0 = mem.ReadU32(c.A0 + 0x7E88u);
        c.V0 = mem.ReadU16(c.S0);
        mem.WriteU16(c.A0 + 0x16u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S0 + 0x2u);
        mem.WriteU16(c.A0 + 0xEu, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1);
        c.V0 = c.V0 - 0x4u;
        mem.WriteU16(c.A0 + 0x8u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x2u);
        c.V0 = c.V0 - 0x3u;
        mem.WriteU16(c.A0 + 0xAu, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S0 + 0x8u);
        c.V1 = mem.ReadU16(c.S1);
        c.V0 = c.V0 - 0x4u;
        c.V1 = c.V1 + c.V0;
        mem.WriteU16(c.A0 + 0x10u, (ushort)c.V1);
        c.V0 = mem.ReadU16(c.S1 + 0x2u);
        c.V0 = c.V0 - 0x3u;
        mem.WriteU16(c.A0 + 0x12u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1);
        c.V0 = c.V0 - 0x4u;
        mem.WriteU16(c.A0 + 0x18u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S0 + 0xAu);
        c.V1 = mem.ReadU16(c.S1 + 0x2u);
        c.V0 = c.V0 - 0x3u;
        c.V1 = c.V1 + c.V0;
        mem.WriteU16(c.A0 + 0x1Au, (ushort)c.V1);
        c.V0 = mem.ReadU16(c.S0 + 0x8u);
        c.V1 = mem.ReadU16(c.S1);
        c.V0 = c.V0 - 0x4u;
        c.V1 = c.V1 + c.V0;
        mem.WriteU16(c.A0 + 0x20u, (ushort)c.V1);
        c.V0 = mem.ReadU16(c.S0 + 0xAu);
        c.V1 = mem.ReadU16(c.S1 + 0x2u);
        c.V0 = c.V0 - 0x3u;
        c.V1 = c.V1 + c.V0;
        mem.WriteU16(c.A0 + 0x22u, (ushort)c.V1);
        c.V0 = mem.ReadU8(c.S0 + 0x4u);
        mem.WriteU8(c.A0 + 0xCu, (byte)c.V0);
        c.V1 = 0x80050000u;
        c.V1 = mem.ReadU32(c.V1 + 0x7E88u);
        c.V0 = mem.ReadU8(c.S0 + 0x6u);
        mem.WriteU8(c.V1 + 0xDu, (byte)c.V0);
        c.V0 = mem.ReadU8(c.S0 + 0x4u);
        c.V1 = mem.ReadU8(c.S0 + 0x8u);
        c.A0 = 0x80050000u;
        c.A0 = mem.ReadU32(c.A0 + 0x7E88u);
        c.V0 = c.V0 + c.V1;
        mem.WriteU8(c.A0 + 0x14u, (byte)c.V0);
        c.V1 = 0x80050000u;
        c.V1 = mem.ReadU32(c.V1 + 0x7E88u);
        c.V0 = mem.ReadU8(c.S0 + 0x6u);
        mem.WriteU8(c.V1 + 0x15u, (byte)c.V0);
        c.V1 = 0x80050000u;
        c.V1 = mem.ReadU32(c.V1 + 0x7E88u);
        c.V0 = mem.ReadU8(c.S0 + 0x4u);
        mem.WriteU8(c.V1 + 0x1Cu, (byte)c.V0);
        c.V0 = mem.ReadU8(c.S0 + 0x6u);
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
        c.V0 = mem.ReadU8(c.S0 + 0x6u);
        c.V1 = mem.ReadU8(c.S0 + 0xAu);
        c.A0 = 0x80050000u;
        c.A0 = mem.ReadU32(c.A0 + 0x7E88u);
        c.V0 = c.V0 + c.V1;
        mem.WriteU8(c.A0 + 0x25u, (byte)c.V0);
        c.A0 = 0x80050000u;
        c.A0 = mem.ReadU32(c.A0 + 0x7E88u);
        c.A1 = 0u | 0x0001u;
        c.RA = 0x80029C34u;
        KingsField1_game.SetSemiTrans_game(c, mem);
        c.A0 = 0u | 0x07D0u;
        c.RA = 0x80029C3Cu;
        KingsField1_game.func_8002AD1C(c, mem);
        c.RA = mem.ReadU32(c.SP + 0x18u);
        c.S1 = mem.ReadU32(c.SP + 0x14u);
        c.S0 = mem.ReadU32(c.SP + 0x10u);
        c.SP = c.SP + 0x20u;
    }
}
