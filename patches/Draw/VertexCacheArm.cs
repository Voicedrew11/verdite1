using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001C6A8(n, shift)</c>, the vertex cache (arm): as <c>func_8001C60C</c>, but
/// the SZ of each vertex is shifted right (arithmetic) by the low byte of <paramref name="shift"/>
/// (<c>a1</c>) before it is stored. Reads the list at <c>u32[0x800910BC]</c> and writes the cache
/// at 0x800911B0, eight bytes per vertex. <c>KF1_VERTEXCACHE_ARM</c>.
/// </summary>
public static class VertexCacheArm
{
    public static readonly Literal R = new(typeof(VertexCacheArm), "vertex cache (arm)", "VERTEXCACHE_ARM", 0x8001C6A8, Run,
        "Projects the arm's vertices into the cache, with a depth bias.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x40u;
        mem.WriteU32(c.SP + 0x3Cu, c.RA);
        mem.WriteU32(c.SP + 0x38u, c.S4);
        mem.WriteU32(c.SP + 0x34u, c.S3);
        mem.WriteU32(c.SP + 0x30u, c.S2);
        mem.WriteU32(c.SP + 0x2Cu, c.S1);
        mem.WriteU32(c.SP + 0x28u, c.S0);
        c.S2 = 0x80090000u;
        c.S2 = c.S2 + 0x11B0u;
        c.S3 = 0x80090000u;
        c.S3 = mem.ReadU32(c.S3 + 0x10BCu);
        bool none = c.A0 == 0u;
        c.S0 = c.A0 - 0x1u;
        if (!none) {
            c.S4 = c.A1 & 0x00FFu;
            c.S1 = c.S2 + 0x4u;
            // one vertex per pass; the count runs down in S0 until it reaches -1
            bool more;
            do {
                Poll(c, mem);
                // RTPS(list, cache, &p, &flag)
                c.A0 = c.S3;
                c.A1 = c.S2;
                c.A2 = c.SP + 0x10u;
                c.A3 = c.SP + 0x14u;
                c.RA = 0x8001C6F8u;
                KingsField1_game.func_8004DA4C(c, mem);
                c.A0 = c.SP + 0x18u;
                c.A1 = c.SP + 0x1Cu;
                c.S2 = c.S2 + 0x8u;
                c.V0 = mem.ReadU16(c.SP + 0x10u);
                c.S3 = c.S3 + 0x8u;
                c.V0 = c.V0 << 1;
                mem.WriteU16(c.S1 + 0x2u, (ushort)c.V0);
                // SZ from ReadSZ2 at SP+0x18, shifted right by the low byte of a1
                c.RA = 0x8001C718u;
                KingsField1_game.ReadSZ2_game(c, mem);
                c.V0 = mem.ReadU32(c.SP + 0x18u);
                c.S0 = c.S0 - 0x1u;
                c.V0 = (uint)((int)c.V0 >> (int)(c.S4 & 31u));
                mem.WriteU16(c.S1, (ushort)c.V0);
                c.V0 = 0xFFFFFFFFu;
                more = c.S0 != c.V0;
                c.S1 = c.S1 + 0x8u;
                // both paths step S1 to the next vertex; the loop runs while the count is not -1
            } while (more);
        }
        c.RA = mem.ReadU32(c.SP + 0x3Cu);
        c.S4 = mem.ReadU32(c.SP + 0x38u);
        c.S3 = mem.ReadU32(c.SP + 0x34u);
        c.S2 = mem.ReadU32(c.SP + 0x30u);
        c.S1 = mem.ReadU32(c.SP + 0x2Cu);
        c.S0 = mem.ReadU32(c.SP + 0x28u);
        c.SP = c.SP + 0x40u;
    }
}
