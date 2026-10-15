using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_800209A8</c>, the pose mark, transcribed literally. <c>KF1_POSEMARK</c>.
/// </summary>
public static class PoseMark
{
    public static readonly Literal R = new(typeof(PoseMark), "pose mark", "POSEMARK", 0x800209A8, Run,
        "Marks the pose slots in use as unclaimed.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.V1 = 0x80090000u;
        c.V1 = c.V1 + 0x10C0u;
        c.A0 = 0u | 0x000Cu;
        c.A2 = 0u | 0x0001u;
        c.A1 = 0u | 0xFFFFu;
        L800209BC: ;
        Poll(c, mem);
        c.V0 = (uint)(short)mem.ReadU16(c.V1);
        if (c.V0 == 0u) {
            c.A0 = c.A0 + c.A1;
            goto L800209D0;
        }
        c.A0 = c.A0 + c.A1;
        mem.WriteU16(c.V1, (ushort)c.A2);
        L800209D0: ;
        c.V0 = c.A0 & 0xFFFFu;
        if (c.V0 != 0u) {
            c.V1 = c.V1 + 0x14u;
            goto L800209BC;
        }
        c.V1 = c.V1 + 0x14u;
        return;
    }
}
