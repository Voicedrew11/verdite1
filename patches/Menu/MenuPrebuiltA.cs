using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_80027E58</c>: links the prebuilt packet of this half at <c>0x80058228 + 0x28 * idx</c>
/// onto slot 2000. <c>KF1_MENU_PREBUILTA</c>.
/// </summary>
public static class MenuPrebuiltA
{
    public static readonly Literal R = new(typeof(MenuPrebuiltA), "menu prebuilt A", "MENU_PREBUILTA", 0x80027E58, Run,
        "Links a prebuilt menu packet at slot 2000.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x18u;
        mem.WriteU32(c.SP + 0x10u, c.RA);
        c.V0 = 0x80070000u;
        c.V0 = mem.ReadU8(c.V0 + 0xE98u);
        c.A0 = 0x80090000u;
        c.A0 = mem.ReadU32(c.A0 + 0xEBCu);
        c.A1 = c.V0 << 2;
        c.A1 = c.A1 + c.V0;
        c.A0 = c.A0 + 0x7D0u;
        c.A1 = c.A1 << 3;
        c.V0 = 0x80060000u;
        c.V0 = c.V0 - 0x7DD8u;
        c.A1 = c.A1 + c.V0;
        c.RA = 0x80027E90u;
        KingsField1_game.AddPrim_game(c, mem);
        c.RA = mem.ReadU32(c.SP + 0x10u);
        c.SP = c.SP + 0x18u;
    }
}
