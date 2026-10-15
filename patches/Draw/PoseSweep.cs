using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_80020A98</c>, the pose sweep, transcribed literally. <c>KF1_POSESWEEP</c>.
/// Walks the 12 pose slots at <c>0x800910C0</c> (0x14 bytes each) and calls
/// <c>func_800209E4</c> on each slot still holding <c>1</c> (unclaimed). Takes no
/// arguments.
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
        while (true)
        {
            Poll(c, mem);
            c.V0 = (uint)(short)mem.ReadU16(c.S0);
            bool unclaimed = c.V0 == c.S3;
            c.S1 = c.S1 + c.S2;
            if (unclaimed)
            {
                c.A0 = c.S0;
                c.RA = 0x80020ADCu;
                KingsField1_game.func_800209E4(c, mem);
            }
            c.V0 = c.S1 & 0xFFFFu;
            bool more = c.V0 != 0u;
            c.S0 = c.S0 + 0x14u;
            if (!more) break;
        }
        c.RA = mem.ReadU32(c.SP + 0x20u);
        c.S3 = mem.ReadU32(c.SP + 0x1Cu);
        c.S2 = mem.ReadU32(c.SP + 0x18u);
        c.S1 = mem.ReadU32(c.SP + 0x14u);
        c.S0 = mem.ReadU32(c.SP + 0x10u);
        c.SP = c.SP + 0x28u;
    }
}
