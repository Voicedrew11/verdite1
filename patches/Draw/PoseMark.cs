using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_800209A8</c>, the pose mark, transcribed literally. <c>KF1_POSEMARK</c>.
/// Walks the 12 pose slots at <c>0x800910C0</c> (0x14 bytes each). A slot whose
/// first halfword is nonzero is set to <c>1</c>, unclaimed, for <c>PoseSweep</c>
/// to free. Takes no arguments.
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
        while (true)
        {
            Poll(c, mem);
            c.V0 = (uint)(short)mem.ReadU16(c.V1);
            bool unused = c.V0 == 0u;
            c.A0 = c.A0 + c.A1;
            if (!unused)
            {
                mem.WriteU16(c.V1, (ushort)c.A2);
            }
            c.V0 = c.A0 & 0xFFFFu;
            bool more = c.V0 != 0u;
            c.V1 = c.V1 + 0x14u;
            if (!more) break;
        }
    }
}
