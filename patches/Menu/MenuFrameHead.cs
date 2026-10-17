using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8002ABB4</c>, the menu's frame head. Flips the buffer index <c>u8[0x80070E98]</c>,
/// points the packet record pointer <c>0x80070EB8</c> at <c>0x80070EA0 + 12 * idx</c> and the
/// ordering table pointer <c>0x80090EBC</c> at <c>0x80070EBC + (idx &lt;&lt; 16)</c>, clears that
/// table (<c>ClearOTagR</c>, 0x4000 entries) and puts the record's cursor back at its start.
/// <c>KF1_MENU_FRAMEHEAD</c>.
/// </summary>
public static class MenuFrameHead
{
    public static readonly Literal R = new(typeof(MenuFrameHead), "menu frame head", "MENU_FRAMEHEAD", 0x8002ABB4, Run,
        "Flips the menu's buffers and clears its ordering table.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x18u;
        mem.WriteU32(c.SP + 0x10u, c.RA);
        c.V1 = 0x80070000u;
        c.V1 = c.V1 + 0xE98u;
        c.V0 = mem.ReadU8(c.V1);
        c.V0 = c.V0 < 0x00000001u ? 1u : 0u;
        mem.WriteU8(c.V1, (byte)c.V0);
        c.A0 = mem.ReadU8(c.V1);
        c.A1 = c.V1 + 0x8u;
        c.V0 = c.A0 << 1;
        c.V0 = c.V0 + c.A0;
        c.V0 = c.V0 << 2;
        c.A0 = mem.ReadU8(c.V1);
        c.V0 = c.V0 + c.A1;
        c.At = 0x80070000u;
        mem.WriteU32(c.At + 0xEB8u, c.V0);
        c.V1 = c.V1 + 0x24u;
        c.A0 = c.A0 << 16;
        c.A0 = c.A0 + c.V1;
        c.At = 0x80090000u;
        mem.WriteU32(c.At + 0xEBCu, c.A0);
        c.A1 = 0u | 0x4000u;
        c.RA = 0x8002AC14u;
        KingsField1_game.ClearOTagR_game(c, mem);
        c.V1 = 0x80070000u;
        c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
        c.V0 = mem.ReadU32(c.V1);
        c.RA = mem.ReadU32(c.SP + 0x10u);
        mem.WriteU32(c.V1 + 0x8u, c.V0);
        c.SP = c.SP + 0x18u;
    }
}
