using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8002ADF8(value, width, zeroPad, out)</c> formats and draws nothing: <c>width</c>
/// u16 fills (0, or 10, a blank, when <c>zeroPad</c> is 0) and a 0xFFFF after them, then the
/// decimal digits right to left, one 0 for a value of 0. <c>KF1_MENU_NUMBER</c>.
/// </summary>
public static class MenuNumber
{
    public static readonly Literal R = new(typeof(MenuNumber), "menu number", "MENU_NUMBER", 0x8002ADF8, Run,
        "Formats a number into the menu's digit string.", Twin);

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Twin(CpuContext c, PSMemory mem)
    {
        int width = Math.Max(0, (int)c.A1);
        var digits = MenuDraw.FormatNumber((int)c.A0, width, c.A2 != 0);
        for (int i = 0; i <= width; i++) mem.WriteU16(c.A3 + (uint)(i * 2), digits[i]);
    }

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x8u;
        c.T0 = 0u;
        c.A2 = c.A2 < 0x00000001u ? 1u : 0u;
        c.A2 = 0u - c.A2;
        c.A2 = c.A2 & 0x000Au;
        if ((int)c.A1 > 0) {
            c.V1 = c.A3;
            do {
                Poll(c, mem);
                mem.WriteU16(c.V1, (ushort)c.A2);
                c.T0 = c.T0 + 0x1u;
                c.V0 = (int)c.T0 < (int)c.A1 ? 1u : 0u;
                c.V1 = c.V1 + 0x2u;
            } while (c.V0 != 0u);
        }
        c.V0 = c.A1 << 1;
        c.V0 = c.V0 + c.A3;
        c.V1 = 0xFFFFFFFFu;
        c.T0 = c.A1 - 0x1u;
        mem.WriteU16(c.V0, (ushort)c.V1);
        if ((int)c.T0 >= 0) {
            c.A2 = 0u | 0x000Au;
            c.V0 = c.T0 << 1;
            c.A1 = c.V0 + c.A3;
            do {
                Poll(c, mem);
                Div(c, c.A0, c.A2);
                c.V1 = c.LO;
                c.V0 = c.HI;
                c.A0 = c.V1;
                mem.WriteU16(c.A1, (ushort)c.V0);
                if (c.A0 == 0u) {
                    c.A1 = c.A3 - 0x2u;
                    c.T0 = 0xFFFFFFFFu;
                }
                c.T0 = c.T0 - 0x1u;
                c.A1 = c.A1 - 0x2u;
            } while ((int)c.T0 >= 0);
        }
        c.SP = c.SP + 0x8u;
    }
}
