using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001F9D4(list)</c>, the HUD's sprite records. The list at <c>A0</c> holds
/// 0x0E-byte records and ends at a byte <c>0xFF</c>; an empty list (first byte 0xFF)
/// draws nothing. A record whose first byte is 1 is passed to <c>func_8001E480</c> at its
/// +2. <c>KF1_HUDSPRITES</c>.
/// </summary>
public static class HudSprites
{
    public static readonly Literal R = new(typeof(HudSprites), "HUD sprites", "HUDSPRITES", 0x8001F9D4, Run,
        "Draws the HUD's sprite records.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x20u;
        mem.WriteU32(c.SP + 0x1Cu, c.RA);
        mem.WriteU32(c.SP + 0x18u, c.S2);
        mem.WriteU32(c.SP + 0x14u, c.S1);
        mem.WriteU32(c.SP + 0x10u, c.S0);
        c.S0 = c.A0;
        c.V1 = mem.ReadU8(c.S0);
        c.V0 = 0u | 0x00FFu;
        bool nonEmpty = c.V1 != c.V0;
        c.S2 = 0u | 0x0001u;
        if (nonEmpty) {
            c.S1 = 0u | 0x00FFu;
            c.V0 = mem.ReadU8(c.S0);
            // one record per pass; 0xFF ends the list
            do {
                Poll(c, mem);
                if (c.V0 == c.S2) {
                    c.A0 = c.S0 + 0x2u;
                    c.RA = 0x8001FA18u;
                    KingsField1_game.func_8001E480(c, mem);
                }
                c.S0 = c.S0 + 0xEu;
                c.V0 = mem.ReadU8(c.S0);
            } while (c.V0 != c.S1);
        }
        c.RA = mem.ReadU32(c.SP + 0x1Cu);
        c.S2 = mem.ReadU32(c.SP + 0x18u);
        c.S1 = mem.ReadU32(c.SP + 0x14u);
        c.S0 = mem.ReadU32(c.SP + 0x10u);
        c.SP = c.SP + 0x20u;
    }
}
