using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8002AD1C(slot)</c>: <c>AddPrim</c> of the packet at <c>0x80057E88</c> onto the slot
/// of the ordering table at <c>0x80090EBC</c>, then advances the pointer by 0x28 and writes it
/// to the packet record's cursor (<c>*0x80070EB8 + 8</c>). No overflow check.
/// <c>KF1_MENU_LINK</c>.
/// </summary>
public static class MenuLink
{
    public static readonly Literal R = new(typeof(MenuLink), "menu link", "MENU_LINK", 0x8002AD1C, Run,
        "Links the menu's packet and advances its pointer.", Twin);

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Twin(CpuContext c, PSMemory mem) => MenuDraw.Link(mem, (int)c.A0);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x18u;
        mem.WriteU32(c.SP + 0x10u, c.RA);
        c.A0 = c.A0 << 2;
        c.V0 = 0x80090000u;
        c.V0 = mem.ReadU32(c.V0 + 0xEBCu);
        c.A1 = 0x80050000u;
        c.A1 = mem.ReadU32(c.A1 + 0x7E88u);
        c.A0 = c.A0 + c.V0;
        c.RA = 0x8002AD40u;
        KingsField1_game.AddPrim_game(c, mem);
        c.V0 = 0x80050000u;
        c.V0 = mem.ReadU32(c.V0 + 0x7E88u);
        c.V1 = 0x80070000u;
        c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
        c.V0 = c.V0 + 0x28u;
        c.At = 0x80050000u;
        mem.WriteU32(c.At + 0x7E88u, c.V0);
        c.RA = mem.ReadU32(c.SP + 0x10u);
        mem.WriteU32(c.V1 + 0x8u, c.V0);
        c.SP = c.SP + 0x18u;
    }
}
