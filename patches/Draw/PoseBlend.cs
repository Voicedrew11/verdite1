using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_800205D4</c>, the pose blender. <c>a0</c> points at a pose slot,
/// <c>a1</c> is the frame index (masked to 16 bits), <c>a2</c> a 16-bit key and
/// <c>a3</c> a 16-bit limit. The clip comes from the table at
/// <c>0x80090FCC</c>, indexed by <c>a1</c>. Returns the pose record in V0, 1,
/// or 0. <c>KF1_POSEBLEND</c>.
/// </summary>
public static class PoseBlend
{
    public static readonly Literal R = new(typeof(PoseBlend), "pose blender", "POSEBLEND", 0x800205D4, Run,
        "Blends a model's pose from its clip's keys.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x48u;
        mem.WriteU32(c.SP + 0x44u, c.RA);
        mem.WriteU32(c.SP + 0x40u, c.FP);
        mem.WriteU32(c.SP + 0x3Cu, c.S7);
        mem.WriteU32(c.SP + 0x38u, c.S6);
        mem.WriteU32(c.SP + 0x34u, c.S5);
        mem.WriteU32(c.SP + 0x30u, c.S4);
        mem.WriteU32(c.SP + 0x2Cu, c.S3);
        mem.WriteU32(c.SP + 0x28u, c.S2);
        mem.WriteU32(c.SP + 0x24u, c.S1);
        mem.WriteU32(c.SP + 0x20u, c.S0);
        c.S1 = c.A0;
        c.S4 = mem.ReadU32(c.S1);
        c.FP = c.A1;
        mem.WriteU16(c.SP + 0x10u, (ushort)c.A2);
        c.S0 = c.FP & 0xFFFFu;
        c.V0 = c.S0 << 2;
        c.T2 = mem.ReadU16(c.SP + 0x58u);
        c.At = 0x80090000u;
        c.At = c.At + 0xFCCu;
        c.At = c.At + c.V0;
        c.S2 = mem.ReadU32(c.At);
        mem.WriteU16(c.SP + 0x18u, (ushort)c.T2);
        c.V0 = mem.ReadU32(c.S2 + 0x4u);

        // The clip's flag at +4 selects the path: zero takes the first, the
        // other goes on to the record's own keys.
        bool flagZero = c.V0 == 0u;
        c.S6 = c.A3;
        if (flagZero)
        {
            if (c.S4 != 0u)
            {
                c.A0 = c.S4;
                c.RA = 0x80020650u;
                KingsField1_game.func_800209E4(c, mem);
            }
            c.A0 = c.S0;
            c.RA = 0x80020658u;
            KingsField1_game.func_8002059C(c, mem);
            c.A0 = 0u;
            c.RA = 0x80020660u;
            KingsField1_game.func_8001C148(c, mem);
            c.V0 = 0u | 0x0001u;
        }
        else
        {
            // A slot already holds a record (S4 != 0) or one is taken from
            // func_80020B04 first. Both meet at the poll loop below.
            if (c.S4 != 0u) goto L800206B8;
            c.RA = 0x80020678u;
            KingsField1_game.func_80020B04(c, mem);
            c.S4 = c.V0;
            if (c.S4 == 0u)
            {
                c.V0 = 0u;
                goto L80020948;
            }
            c.V0 = 0u;

            // Loop head: fill the record, then take a sub-block until
            // func_8001AAB0 returns one.
            L80020684: ;
            Poll(c, mem);
            mem.WriteU16(c.S4 + 0x2u, (ushort)c.FP);
            mem.WriteU32(c.S4 + 0x10u, c.S1);
            while (true)
            {
                Poll(c, mem);
                c.T2 = mem.ReadU16(c.SP + 0x18u);
                c.A0 = c.T2 << 3;
                c.RA = 0x80020698u;
                KingsField1_game.func_8001AAB0(c, mem);
                if (c.V0 != 0u)
                {
                    mem.WriteU32(c.S4 + 0xCu, c.V0);
                    break;
                }
                mem.WriteU32(c.S4 + 0xCu, c.V0);
                c.RA = 0x800206A8u;
                KingsField1_game.func_80020A2C(c, mem);
            }
            mem.WriteU32(c.S1, c.S4);
            goto L800206DC;

            // Record already present: keep it when its key matches, else
            // rebuild it through func_800209E4.
            L800206B8: ;
            c.V0 = mem.ReadU16(c.S4 + 0x2u);
            bool keyChanged = c.V0 != c.S0;
            c.A0 = 0u;
            if (keyChanged)
            {
                c.A0 = c.S4;
                c.RA = 0x800206D0u;
                KingsField1_game.func_800209E4(c, mem);
                c.V0 = 0u | 0x00FFu;
                mem.WriteU16(c.S4 + 0x4u, (ushort)c.V0);
                goto L80020684;
            }

            L800206DC: ;
            c.A0 = 0u;

            // Common tail: locate the clip's segment for this frame.
            c.V1 = mem.ReadU32(c.S2 + 0x10u);
            c.V0 = mem.ReadU16(c.SP + 0x10u);
            c.V1 = c.S2 + c.V1;
            c.V0 = c.V0 << 2;
            c.V0 = c.V0 + c.V1;
            c.V0 = mem.ReadU32(c.V0);
            c.A2 = 0u;
            c.V0 = c.S2 + c.V0;
            c.V1 = mem.ReadU16(c.V0);
            c.A1 = c.V0 + 0x4u;
            c.V0 = c.V1;
            c.V0 = c.V0 & 0xFFFFu;
            bool hasSegments = c.V0 != 0u;
            c.V1 = c.V1 - 0x1u;
            bool found = false;
            if (hasSegments)
            {
                c.A3 = c.S6 & 0xFFFFu;
                c.T1 = 0u | 0x1000u;
                c.T0 = 0u | 0xFFFFu;
                while (true)
                {
                    Poll(c, mem);
                    c.V0 = mem.ReadU32(c.A1);
                    c.S3 = c.S2 + c.V0;
                    c.V0 = mem.ReadU16(c.S3 + 0x2u);
                    c.V0 = c.A0 + c.V0;
                    c.A0 = c.V0;
                    c.V0 = c.A0 & 0xFFFFu;
                    c.V0 = c.A3 < c.V0 ? 1u : 0u;
                    bool inSegment = c.V0 != 0u;
                    c.A1 = c.A1 + 0x4u;
                    if (inSegment)
                    {
                        c.V0 = c.S6 - c.A2;
                        c.V0 = c.V0 & 0xFFFFu;
                        c.V1 = mem.ReadU16(c.S3 + 0x2u);
                        c.V0 = c.V0 << 12;
                        DivU(c, c.V0, c.V1);
                        c.V1 = c.LO;
                        c.V0 = mem.ReadU16(c.S3);
                        bool plain = c.V0 == 0u;
                        c.S7 = c.V1;
                        if (!plain)
                        {
                            c.S7 = c.T1 - c.V1;
                        }
                        found = true;
                        break;
                    }
                    c.A2 = c.A0;
                    c.S5 = c.S5 + 0x1u;
                    c.V0 = c.V1;
                    c.V0 = c.V0 & 0xFFFFu;
                    bool moreSegments = c.V0 != 0u;
                    c.V1 = c.V1 + c.T0;
                    if (!moreSegments) break;
                }
            }
            if (!found)
            {
                c.S5 = c.S5 - 0x1u;
                c.S7 = 0u | 0x1000u;
            }

            c.V1 = mem.ReadU16(c.S4 + 0x4u);
            c.V0 = mem.ReadU16(c.SP + 0x10u);
            bool differs = c.V1 != c.V0;
            c.A0 = c.FP & 0xFFFFu;
            if (!differs)
            {
                c.V1 = mem.ReadU16(c.S4 + 0x6u);
                c.V0 = c.S5 & 0xFFFFu;
                differs = c.V1 != c.V0;
            }
            if (differs)
            {
                // Rebuild the record's matrices from the clip's keys.
                c.V0 = mem.ReadU32(c.S2 + 0xCu);
                c.S6 = c.S2 + c.V0;
                c.RA = 0x800207DCu;
                KingsField1_game.func_8002059C(c, mem);
                c.A0 = 0u;
                c.RA = 0x800207E4u;
                KingsField1_game.func_8001C148(c, mem);
                c.A2 = 0u | 0xFFFFu;
                c.A1 = mem.ReadU16(c.SP + 0x18u);
                c.A0 = 0x80090000u;
                c.A0 = mem.ReadU32(c.A0 + 0x10BCu);
                c.V1 = mem.ReadU32(c.S4 + 0xCu);
                bool copyMore;
                do
                {
                    Poll(c, mem);
                    c.V0 = mem.ReadU32(c.A0);
                    c.A0 = c.A0 + 0x4u;
                    c.A1 = c.A1 + c.A2;
                    mem.WriteU32(c.V1, c.V0);
                    c.V0 = mem.ReadU32(c.A0);
                    c.V1 = c.V1 + 0x4u;
                    c.A0 = c.A0 + 0x4u;
                    mem.WriteU32(c.V1, c.V0);
                    c.V0 = c.A1 & 0xFFFFu;
                    copyMore = c.V0 != 0u;
                    c.V1 = c.V1 + 0x4u;
                } while (copyMore);

                c.S0 = mem.ReadU16(c.S3 + 0x6u);
                c.S1 = c.S3 + 0x8u;
                c.V0 = c.S0;
                c.V0 = c.V0 & 0xFFFFu;
                bool hasWeights = c.V0 != 0u;
                c.S0 = c.S0 - 0x1u;
                if (hasWeights)
                {
                    c.FP = 0u | 0xFFFFu;
                    bool weightMore;
                    do
                    {
                        Poll(c, mem);
                        c.V0 = mem.ReadU16(c.S1);
                        c.S1 = c.S1 + 0x2u;
                        c.V0 = c.V0 << 2;
                        c.V0 = c.V0 + c.S6;
                        c.V0 = mem.ReadU32(c.V0);
                        c.A3 = 0u | 0x1000u;
                        c.V0 = c.S2 + c.V0;
                        c.A2 = mem.ReadU32(c.V0 + 0x8u);
                        c.A0 = mem.ReadU32(c.V0 + 0x4u);
                        c.A1 = c.V0 + 0xCu;
                        c.V0 = mem.ReadU32(c.S4 + 0xCu);
                        c.A0 = c.A0 << 3;
                        c.A0 = c.A0 + c.V0;
                        c.RA = 0x80020878u;
                        KingsField1_game.gteMIMefunc_game(c, mem);
                        c.V0 = c.S0;
                        c.V0 = c.V0 & 0xFFFFu;
                        weightMore = c.V0 != 0u;
                        c.S0 = c.S0 + c.FP;
                    } while (weightMore);
                }
                c.V0 = mem.ReadU16(c.S3 + 0x4u);
                c.V0 = c.V0 << 2;
                c.V0 = c.V0 + c.S6;
                c.V0 = mem.ReadU32(c.V0);
                c.V0 = c.S2 + c.V0;
                mem.WriteU32(c.S4 + 0x8u, c.V0);
            }

            c.T2 = mem.ReadU16(c.SP + 0x10u);
            mem.WriteU16(c.S4 + 0x6u, (ushort)c.S5);
            c.A1 = 0x80090000u;
            c.A1 = c.A1 + 0x30F8u;
            c.V1 = mem.ReadU32(c.S4 + 0xCu);
            c.A2 = 0u | 0xFFFFu;
            mem.WriteU16(c.S4 + 0x4u, (ushort)c.T2);
            c.A0 = mem.ReadU16(c.SP + 0x18u);
            bool scratchMore;
            do
            {
                Poll(c, mem);
                c.V0 = mem.ReadU32(c.V1);
                c.V1 = c.V1 + 0x4u;
                c.A0 = c.A0 + c.A2;
                mem.WriteU32(c.A1, c.V0);
                c.V0 = mem.ReadU32(c.V1);
                c.A1 = c.A1 + 0x4u;
                c.V1 = c.V1 + 0x4u;
                mem.WriteU32(c.A1, c.V0);
                c.V0 = c.A0 & 0xFFFFu;
                scratchMore = c.V0 != 0u;
                c.A1 = c.A1 + 0x4u;
            } while (scratchMore);
            c.A3 = c.S7 & 0xFFFFu;
            c.V0 = mem.ReadU32(c.S4 + 0x8u);
            c.S1 = 0x80090000u;
            c.S1 = c.S1 + 0x30F0u;
            c.A1 = c.V0 + 0x4u;
            c.S0 = mem.ReadU32(c.V0 + 0x4u);
            c.A2 = mem.ReadU32(c.V0 + 0x8u);
            c.S0 = c.S0 << 3;
            c.S0 = c.S0 + c.S1;
            c.A0 = c.S0;
            c.S3 = mem.ReadU32(c.S0);
            c.S2 = mem.ReadU32(c.S0 + 0x4u);
            c.A2 = c.A2 + 0x1u;
            c.RA = 0x8002092Cu;
            KingsField1_game.gteMIMefunc_game(c, mem);
            c.A0 = c.S1 + 0x8u;
            mem.WriteU32(c.S0, c.S3);
            mem.WriteU32(c.S0 + 0x4u, c.S2);
            c.RA = 0x8002093Cu;
            KingsField1_game.func_8001C138(c, mem);
            c.V0 = 0u | 0x0002u;
            mem.WriteU16(c.S4, (ushort)c.V0);
            c.V0 = c.S4;
        }

        L80020948: ;
        c.RA = mem.ReadU32(c.SP + 0x44u);
        c.FP = mem.ReadU32(c.SP + 0x40u);
        c.S7 = mem.ReadU32(c.SP + 0x3Cu);
        c.S6 = mem.ReadU32(c.SP + 0x38u);
        c.S5 = mem.ReadU32(c.SP + 0x34u);
        c.S4 = mem.ReadU32(c.SP + 0x30u);
        c.S3 = mem.ReadU32(c.SP + 0x2Cu);
        c.S2 = mem.ReadU32(c.SP + 0x28u);
        c.S1 = mem.ReadU32(c.SP + 0x24u);
        c.S0 = mem.ReadU32(c.SP + 0x20u);
        c.SP = c.SP + 0x48u;
        return;
    }
}
