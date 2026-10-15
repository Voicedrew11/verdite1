using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_80020A98</c>, the pose sweep, transcribed literally. <c>KF1_POSESWEEP</c>.
/// </summary>
public static class PoseSweep
{
    public static readonly Literal R = new(typeof(PoseSweep), "pose sweep", "POSESWEEP", 0x80020A98, Run,
        "Frees the pose slots nothing claimed.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x28u;
        mem.WriteU32(c.SP + 0x20u, c.RA);
        mem.WriteU32(c.SP + 0x1Cu, c.S3);
        mem.WriteU32(c.SP + 0x18u, c.S2);
        mem.WriteU32(c.SP + 0x14u, c.S1);
        mem.WriteU32(c.SP + 0x10u, c.S0);
        c.S0 = 0x80090000u;
        c.S0 = c.S0 + 0x10C0u;
        c.S1 = 0u | 0x000Cu;
        c.S3 = 0u | 0x0001u;
        c.S2 = 0u | 0xFFFFu;
        L80020AC4: ;
        Poll(c, mem);
        c.V0 = (uint)(short)mem.ReadU16(c.S0);
        if (c.V0 != c.S3) {
            c.S1 = c.S1 + c.S2;
            goto L80020ADC;
        }
        c.S1 = c.S1 + c.S2;
        c.A0 = c.S0;
        c.RA = 0x80020ADCu;
        KingsField1_game.func_800209E4(c, mem);
        L80020ADC: ;
        c.V0 = c.S1 & 0xFFFFu;
        if (c.V0 != 0u) {
            c.S0 = c.S0 + 0x14u;
            goto L80020AC4;
        }
        c.S0 = c.S0 + 0x14u;
        c.RA = mem.ReadU32(c.SP + 0x20u);
        c.S3 = mem.ReadU32(c.SP + 0x1Cu);
        c.S2 = mem.ReadU32(c.SP + 0x18u);
        c.S1 = mem.ReadU32(c.SP + 0x14u);
        c.S0 = mem.ReadU32(c.SP + 0x10u);
        c.SP = c.SP + 0x28u;
        return;
    }
}
