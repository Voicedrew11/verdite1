using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001F9D4</c>, the HUD sprites, transcribed literally. <c>KF1_HUDSPRITES</c>.
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
        if (c.V1 == c.V0) {
            c.S2 = 0u | 0x0001u;
            goto L8001FA2C;
        }
        c.S2 = 0u | 0x0001u;
        c.S1 = 0u | 0x00FFu;
        c.V0 = mem.ReadU8(c.S0);
        L8001FA04: ;
        Poll(c, mem);
        if (c.V0 != c.S2) {
            goto L8001FA18;
        }
        c.A0 = c.S0 + 0x2u;
        c.RA = 0x8001FA18u;
        KingsField1_game.func_8001E480(c, mem);
        L8001FA18: ;
        c.S0 = c.S0 + 0xEu;
        c.V0 = mem.ReadU8(c.S0);
        if (c.V0 != c.S1) {
            goto L8001FA04;
        }
        L8001FA2C: ;
        c.RA = mem.ReadU32(c.SP + 0x1Cu);
        c.S2 = mem.ReadU32(c.SP + 0x18u);
        c.S1 = mem.ReadU32(c.SP + 0x14u);
        c.S0 = mem.ReadU32(c.SP + 0x10u);
        c.SP = c.SP + 0x20u;
        return;
    }
}
