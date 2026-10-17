using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001C754(n)</c>, the vertex cache (compass): <c>func_8004DADC</c> on
/// <paramref name="n"/> groups from the list at <c>u32[0x800910BC]</c> (8 bytes each),
/// writing eight bytes per group into the cache at 0x800911B0 from the call's results at
/// SP+0x10, SP+0x14 and SP+0x18 (the last one twice, at +4 and +6, as the code has it).
/// <c>KF1_VERTEXCACHE_COMPASS</c>.
///
/// The transform is MVMVA with no divide, so no screen coordinate leaves the GTE for
/// the vertex map to follow; with sub-pixel on, each vertex's dropped fraction is
/// offered here, Verdite3's <c>PublishScreenVertex</c> ("The compass's fraction" in
/// docs/PICTURE.md).
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
        bool none = c.A0 == 0u;
        c.S1 = c.A0 - 0x1u;
        if (!none) {
            c.S4 = 0xFFFFFFFFu;
            c.S0 = c.S2 + 0x4u;
            // one group per pass; S1 counts down from n-1 to -1
            bool more;
            do {
                Poll(c, mem);
                c.A0 = c.S3;
                c.A1 = c.SP + 0x10u;
                c.A2 = c.SP + 0x20u;
                c.RA = 0x8001C7A0u;
                KingsField1_game.func_8004DADC(c, mem);
                // X and Y as one word, the same bytes, so the vertex map can follow it
                uint xy = mem.ReadU16(c.SP + 0x10u) | (uint)mem.ReadU16(c.SP + 0x14u) << 16;
                if (GteDepth.Subpixel && GteVertexMap.Active) PublishFraction(mem, c.S3, xy);
                mem.WriteU32(c.S2, xy);
                c.V0 = xy >> 16;
                c.S3 = c.S3 + 0x8u;
                c.V0 = mem.ReadU16(c.SP + 0x18u);
                c.S1 = c.S1 - 0x1u;
                mem.WriteU16(c.S0 + 0x2u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.SP + 0x18u);
                c.S2 = c.S2 + 0x8u;
                mem.WriteU16(c.S0, (ushort)c.V0);
                // both paths step S0 to the next group; the loop runs while S1 is not -1
                more = c.S1 != c.S4;
                c.S0 = c.S0 + 0x8u;
            } while (more);
        }
        c.RA = mem.ReadU32(c.SP + 0x44u);
        c.S4 = mem.ReadU32(c.SP + 0x40u);
        c.S3 = mem.ReadU32(c.SP + 0x3Cu);
        c.S2 = mem.ReadU32(c.SP + 0x38u);
        c.S1 = mem.ReadU32(c.SP + 0x34u);
        c.S0 = mem.ReadU32(c.SP + 0x30u);
        c.SP = c.SP + 0x48u;
    }

    /// <summary>Offer the vertex's fraction for the store that follows: the part of
    /// <c>(TR &lt;&lt; 12) + R·V</c> below the GTE's shift of 12, with a depth of 0 (placed,
    /// not projected). A product the GTE saturated is offered with no fraction.</summary>
    static void PublishFraction(PSMemory mem, uint src, uint xy)
    {
        uint v01 = mem.ReadU32(src), v2 = mem.ReadU32(src + 0x4u);
        short vx = (short)v01, vy = (short)(v01 >> 16), vz = (short)v2;
        uint r0 = Gte.ReadControl(0), r1 = Gte.ReadControl(1), r2 = Gte.ReadControl(2);
        long sx = ((long)(int)Gte.ReadControl(5) << 12) + (short)r0 * vx + (short)(r0 >> 16) * vy + (short)r1 * vz;
        long sy = ((long)(int)Gte.ReadControl(6) << 12) + (short)(r1 >> 16) * vx + (short)r2 * vy + (short)(r2 >> 16) * vz;
        float fx = (short)(sx >> 12) == (short)xy ? (sx & 0xFFF) / 4096f : 0f;
        float fy = (short)(sy >> 12) == (short)(xy >> 16) ? (sy & 0xFFF) / 4096f : 0f;
        GteVertexMap.Publish(xy, 0f, fx, fy, false);
    }
}
