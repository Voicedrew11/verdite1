using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8002ACCC</c>: <c>SetPolyFT4</c> on the packet at the menu's packet pointer
/// <c>0x80057E88</c>, coloured grey (0x60). It does not advance the pointer; the link does.
/// <c>KF1_MENU_QUAD</c>.
/// </summary>
public static class MenuQuad
{
    public static readonly Literal R = new(typeof(MenuQuad), "menu quad", "MENU_QUAD", 0x8002ACCC, Run,
        "Starts an FT4 at the menu's packet pointer.", Twin);

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Twin(CpuContext c, PSMemory mem) => MenuDraw.NewQuad(mem);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.A0 = 0x80050000u;
        c.A0 = mem.ReadU32(c.A0 + 0x7E88u);
        c.SP = c.SP - 0x18u;
        mem.WriteU32(c.SP + 0x10u, c.RA);
        c.RA = 0x8002ACE4u;
        KingsField1_game.SetPolyFT4_game(c, mem);
        c.V0 = 0x80050000u;
        c.V0 = mem.ReadU32(c.V0 + 0x7E88u);
        c.V1 = 0u | 0x0060u;
        mem.WriteU8(c.V0 + 0x4u, (byte)c.V1);
        c.V0 = 0x80050000u;
        c.V0 = mem.ReadU32(c.V0 + 0x7E88u);
        mem.WriteU8(c.V0 + 0x5u, (byte)c.V1);
        c.V0 = 0x80050000u;
        c.V0 = mem.ReadU32(c.V0 + 0x7E88u);
        c.RA = mem.ReadU32(c.SP + 0x10u);
        mem.WriteU8(c.V0 + 0x6u, (byte)c.V1);
        c.SP = c.SP + 0x18u;
    }
}
