using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001C754</c>, the vertex cache (compass), transcribed literally. <c>KF1_VERTEXCACHE_COMPASS</c>.
/// </summary>
public static class VertexCacheCompass
{
    public static readonly Literal R = new(typeof(VertexCacheCompass), "vertex cache (compass)", "VERTEXCACHE_COMPASS", 0x8001C754, Run,
        "Projects the compass's vertices into the cache.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x48u;
        mem.WriteU32(c.SP + 0x44u, c.RA);
        mem.WriteU32(c.SP + 0x40u, c.S4);
        mem.WriteU32(c.SP + 0x3Cu, c.S3);
        mem.WriteU32(c.SP + 0x38u, c.S2);
        mem.WriteU32(c.SP + 0x34u, c.S1);
        mem.WriteU32(c.SP + 0x30u, c.S0);
        c.S2 = 0x80090000u;
        c.S2 = c.S2 + 0x11B0u;
        c.S3 = 0x80090000u;
        c.S3 = mem.ReadU32(c.S3 + 0x10BCu);
        if (c.A0 == 0u) {
            c.S1 = c.A0 - 0x1u;
            goto L8001C7D8;
        }
        c.S1 = c.A0 - 0x1u;
        c.S4 = 0xFFFFFFFFu;
        c.S0 = c.S2 + 0x4u;
        L8001C790: ;
        Poll(c, mem);
        c.A0 = c.S3;
        c.A1 = c.SP + 0x10u;
        c.A2 = c.SP + 0x20u;
        c.RA = 0x8001C7A0u;
        KingsField1_game.func_8004DADC(c, mem);
        c.V0 = mem.ReadU16(c.SP + 0x10u);
        mem.WriteU16(c.S2, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.SP + 0x14u);
        c.S3 = c.S3 + 0x8u;
        mem.WriteU16(c.S0 - 0x2u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.SP + 0x18u);
        c.S1 = c.S1 - 0x1u;
        mem.WriteU16(c.S0 + 0x2u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.SP + 0x18u);
        c.S2 = c.S2 + 0x8u;
        mem.WriteU16(c.S0, (ushort)c.V0);
        if (c.S1 != c.S4) {
            c.S0 = c.S0 + 0x8u;
            goto L8001C790;
        }
        c.S0 = c.S0 + 0x8u;
        L8001C7D8: ;
        c.RA = mem.ReadU32(c.SP + 0x44u);
        c.S4 = mem.ReadU32(c.SP + 0x40u);
        c.S3 = mem.ReadU32(c.SP + 0x3Cu);
        c.S2 = mem.ReadU32(c.SP + 0x38u);
        c.S1 = mem.ReadU32(c.SP + 0x34u);
        c.S0 = mem.ReadU32(c.SP + 0x30u);
        c.SP = c.SP + 0x48u;
        return;
    }
}
