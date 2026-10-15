using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001C60C(n)</c>, the projected-vertex cache: <c>RTPS</c>
/// (<c>func_8004DA4C</c>) on <paramref name="n"/> vertices from the list at
/// <c>u32[0x800910BC]</c>, 8 bytes each, into the cache at 0x800911B0: SXY, then
/// at +4 the vertex's SZ and at +6 its IR0 doubled. <c>KF1_VERTEXCACHE</c>.
/// </summary>
public static class VertexCache
{
    public static readonly Literal R = new(typeof(VertexCache), "vertex cache", "VERTEXCACHE", 0x8001C60C, Run,
        "Projects the model's vertices into the cache the polygon writers read.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x40u;
        mem.WriteU32(c.SP + 0x38u, c.RA);
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
        if (!none)
        {
            c.S1 = c.S2 + 0x4u;
            while (true)
            {
                Poll(c, mem);
                // RTPS(list, cache, &p, &flag)
                c.A0 = c.S3;
                c.A1 = c.S2;
                c.A2 = c.SP + 0x10u;
                c.A3 = c.SP + 0x14u;
                c.RA = 0x8001C654u;
                KingsField1_game.func_8004DA4C(c, mem);
                c.A0 = c.SP + 0x18u;
                c.A1 = c.SP + 0x1Cu;
                c.S2 = c.S2 + 0x8u;
                c.V0 = mem.ReadU16(c.SP + 0x10u);
                c.S3 = c.S3 + 0x8u;
                c.V0 = c.V0 << 1;
                mem.WriteU16(c.S1 + 0x2u, (ushort)c.V0);
                c.RA = 0x8001C674u;
                KingsField1_game.ReadSZ2_game(c, mem);
                c.V0 = mem.ReadU16(c.SP + 0x18u);
                c.S0 = c.S0 - 0x1u;
                mem.WriteU16(c.S1, (ushort)c.V0);
                c.V0 = 0xFFFFFFFFu;
                bool more = c.S0 != c.V0;
                c.S1 = c.S1 + 0x8u;
                if (!more) break;
            }
        }
        c.RA = mem.ReadU32(c.SP + 0x38u);
        c.S3 = mem.ReadU32(c.SP + 0x34u);
        c.S2 = mem.ReadU32(c.SP + 0x30u);
        c.S1 = mem.ReadU32(c.SP + 0x2Cu);
        c.S0 = mem.ReadU32(c.SP + 0x28u);
        c.SP = c.SP + 0x40u;
    }
}
