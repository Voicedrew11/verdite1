using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001D730</c>, the polygon writer (objects), transcribed literally. <c>KF1_POLYASSEMBLER_CACHED</c>.
/// Selects the object <c>func_8001C114</c> returns for the low 16 bits of a0, then walks its primitive
/// records one at a time. The top byte of a record is its kind: 0x34 GT3, 0x3C GT4, 0x24 FT3, 0x2C FT4.
/// Each of these is built from the vertex cache at 0x800911B0, its depth is the average of the vertex
/// depths plus the signed low half of a1, and it is linked into the ordering table at 0x80090EBC
/// (<c>AddPrim_game</c>). Other kinds are skipped. Vertices are <c>func_8004DE4C</c> (back-face test)
/// first; a back-facing primitive is dropped.
/// </summary>
public static class PolyAssemblerCached
{
    public static readonly Literal R = new(typeof(PolyAssemblerCached), "polygon writer (objects)", "POLYASSEMBLER_CACHED", 0x8001D730, Run,
        "Writes an object's primitives from the vertex cache, unlit.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x60u;
        mem.WriteU32(c.SP + 0x5Cu, c.RA);
        mem.WriteU32(c.SP + 0x58u, c.FP);
        mem.WriteU32(c.SP + 0x54u, c.S7);
        mem.WriteU32(c.SP + 0x50u, c.S6);
        mem.WriteU32(c.SP + 0x4Cu, c.S5);
        mem.WriteU32(c.SP + 0x48u, c.S4);
        mem.WriteU32(c.SP + 0x44u, c.S3);
        mem.WriteU32(c.SP + 0x40u, c.S2);
        mem.WriteU32(c.SP + 0x3Cu, c.S1);
        mem.WriteU32(c.SP + 0x38u, c.S0);
        c.S0 = c.A1;
        c.A0 = c.A0 & 0xFFFFu;
        c.RA = 0x8001D768u;
        KingsField1_game.func_8001C114(c, mem);
        c.T0 = mem.ReadU32(c.V0 + 0x14u);
        c.A1 = 0x80090000u;
        c.A1 = c.A1 + 0xFC8u;
        mem.WriteU32(c.SP + 0x28u, c.T0);
        c.V1 = mem.ReadU32(c.V0 + 0x10u);
        c.A0 = mem.ReadU32(c.A1);
        c.V1 = c.V1 + 0xCu;
        c.S1 = c.V1 + c.A0;
        c.V1 = mem.ReadU32(c.SP + 0x28u);
        c.V0 = mem.ReadU32(c.V0 + 0x8u);
        c.T0 = c.T0 - 0x1u;
        mem.WriteU32(c.SP + 0x28u, c.T0);
        c.V0 = c.V0 + 0xCu;
        bool empty = c.V1 == 0u;
        c.S7 = c.V0 + c.A0;
        if (!empty)
        {
            c.S6 = c.A1 + 0x1E8u;
            c.FP = 0x80050000u;
            c.FP = c.FP + 0x7B5Fu;
            c.V0 = c.S0 << 16;
            c.V0 = (uint)((int)c.V0 >> 16);
            mem.WriteU32(c.SP + 0x30u, c.V0);
            while (true)
            {
                Poll(c, mem);
                c.T0 = mem.ReadU32(c.S1);
                c.S1 = c.S1 + 0x4u;
                c.V0 = 0u | 0x0034u;
                c.V1 = c.T0 >> 24;
                // The kind tests read V1 before any body below changes it.
                bool gt3 = c.V1 == c.V0;
                bool gt4 = c.V1 == 0x003Cu;
                bool ft3 = c.V1 == 0x0024u;
                bool ft4 = c.V1 == 0x002Cu;
                mem.WriteU32(c.SP + 0x20u, c.T0);
                bool emit = false;
                if (!gt3)
                {
                    c.V0 = (int)c.V1 < 53 ? 1u : 0u;
                    bool lt53 = c.V0 != 0u;
                    c.V0 = 0u | 0x0024u;
                    if (lt53) { c.V0 = 0u | 0x002Cu; }
                    if (!lt53) { c.V0 = 0u | 0x003Cu; }
                }

                if (gt3)
                {
                    c.V0 = mem.ReadU16(c.S1 + 0xEu);
                    c.V1 = mem.ReadU16(c.S1 + 0x16u);
                    c.S2 = c.V0 + c.S6;
                    c.S4 = c.V1 + c.S6;
                    c.V0 = mem.ReadU16(c.S1 + 0x12u);
                    c.A0 = mem.ReadU32(c.S2);
                    c.A2 = mem.ReadU32(c.S4);
                    c.S3 = c.V0 + c.S6;
                    c.A1 = mem.ReadU32(c.S3);
                    c.RA = 0x8001D838u;
                    KingsField1_game.func_8004DE4C(c, mem);
                    if ((int)c.V0 > 0)
                    {
                        c.V1 = 0x80070000u;
                        c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
                        c.S0 = mem.ReadU32(c.V1 + 0x8u);
                        c.V0 = c.S0 + 0x28u;
                        mem.WriteU32(c.V1 + 0x8u, c.V0);
                        c.V0 = 0x80070000u;
                        c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
                        c.V1 = mem.ReadU32(c.V0 + 0x8u);
                        c.V0 = mem.ReadU32(c.V0 + 0x4u);
                        c.V0 = c.V0 < c.V1 ? 1u : 0u;
                        if (c.V0 != 0u) break;
                        c.A0 = c.S0;
                        c.RA = 0x8001D888u;
                        KingsField1_game.SetPolyGT3_game(c, mem);
                        c.V0 = mem.ReadU16(c.S6 + 0x3EA8u);
                        c.V1 = mem.ReadU8(c.S0 + 0x7u);
                        mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S6 + 0x3EAAu);
                        mem.WriteU16(c.S0 + 0x1Au, (ushort)c.V0);
                        c.V0 = mem.ReadU32(c.S2);
                        mem.WriteU32(c.S0 + 0x8u, c.V0);
                        c.V0 = mem.ReadU32(c.S3);
                        mem.WriteU32(c.S0 + 0x14u, c.V0);
                        c.V0 = mem.ReadU32(c.S4);
                        mem.WriteU32(c.S0 + 0x20u, c.V0);
                        c.V0 = mem.ReadU16(c.S1);
                        mem.WriteU16(c.S0 + 0xCu, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S1 + 0x4u);
                        mem.WriteU16(c.S0 + 0x18u, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S1 + 0x8u);
                        c.A3 = c.FP - 0x3u;
                        mem.WriteU16(c.S0 + 0x24u, (ushort)c.V0);
                        mem.WriteU8(c.FP, (byte)c.V1);
                        c.A0 = mem.ReadU16(c.S1 + 0xCu);
                        c.A1 = mem.ReadU16(c.S1 + 0x10u);
                        c.A2 = mem.ReadU16(c.S1 + 0x14u);
                        c.V1 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                        c.V0 = c.S0 + 0x4u;
                        mem.WriteU32(c.SP + 0x14u, c.V0);
                        c.V0 = c.S0 + 0x10u;
                        mem.WriteU32(c.SP + 0x18u, c.V0);
                        c.V0 = c.S0 + 0x1Cu;
                        mem.WriteU32(c.SP + 0x1Cu, c.V0);
                        mem.WriteU32(c.SP + 0x10u, c.V1);
                        c.A0 = c.S7 + c.A0;
                        c.A1 = c.S7 + c.A1;
                        c.A2 = c.S7 + c.A2;
                        c.RA = 0x8001D928u;
                        KingsField1_game.func_8004DD40(c, mem);
                        c.T0 = 0u | 0x0003u;
                        c.V0 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                        c.V1 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
                        c.A0 = (uint)(short)mem.ReadU16(c.S4 + 0x4u);
                        c.V0 = c.V0 + c.V1;
                        c.V0 = c.V0 + c.A0;
                        Div(c, c.V0, c.T0); // at = 0x80000000 after
                        c.V0 = c.LO;
                        c.T0 = mem.ReadU32(c.SP + 0x30u);
                        c.V0 = (uint)((int)c.V0 >> 2);
                        emit = true;
                    }
                }

                if (gt4)
                {
                    c.V0 = mem.ReadU16(c.S1 + 0x12u);
                    c.V1 = mem.ReadU16(c.S1 + 0x1Au);
                    c.S2 = c.V0 + c.S6;
                    c.S4 = c.V1 + c.S6;
                    c.V0 = mem.ReadU16(c.S1 + 0x16u);
                    c.A0 = mem.ReadU32(c.S2);
                    c.A2 = mem.ReadU32(c.S4);
                    c.S3 = c.V0 + c.S6;
                    c.A1 = mem.ReadU32(c.S3);
                    c.RA = 0x8001D9A4u;
                    KingsField1_game.func_8004DE4C(c, mem);
                    if ((int)c.V0 > 0)
                    {
                        c.V1 = 0x80070000u;
                        c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
                        c.S0 = mem.ReadU32(c.V1 + 0x8u);
                        c.A0 = mem.ReadU16(c.S1 + 0x1Eu);
                        c.V0 = c.S0 + 0x34u;
                        mem.WriteU32(c.V1 + 0x8u, c.V0);
                        c.V0 = 0x80070000u;
                        c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
                        c.V1 = mem.ReadU32(c.V0 + 0x8u);
                        c.V0 = mem.ReadU32(c.V0 + 0x4u);
                        c.V0 = c.V0 < c.V1 ? 1u : 0u;
                        bool done = c.V0 != 0u;
                        c.S5 = c.A0 + c.S6;
                        if (done) break;
                        c.A0 = c.S0;
                        c.RA = 0x8001D9F4u;
                        KingsField1_game.SetPolyGT4_game(c, mem);
                        c.V0 = mem.ReadU16(c.S6 + 0x3EA8u);
                        c.V1 = mem.ReadU8(c.S0 + 0x7u);
                        mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S6 + 0x3EAAu);
                        mem.WriteU16(c.S0 + 0x1Au, (ushort)c.V0);
                        c.V0 = mem.ReadU32(c.S2);
                        mem.WriteU32(c.S0 + 0x8u, c.V0);
                        c.V0 = mem.ReadU32(c.S3);
                        mem.WriteU32(c.S0 + 0x14u, c.V0);
                        c.V0 = mem.ReadU32(c.S4);
                        mem.WriteU32(c.S0 + 0x20u, c.V0);
                        c.V0 = mem.ReadU32(c.S5);
                        mem.WriteU32(c.S0 + 0x2Cu, c.V0);
                        c.V0 = mem.ReadU16(c.S1);
                        mem.WriteU16(c.S0 + 0xCu, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S1 + 0x4u);
                        mem.WriteU16(c.S0 + 0x18u, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S1 + 0x8u);
                        mem.WriteU16(c.S0 + 0x24u, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S1 + 0xCu);
                        c.A3 = c.FP - 0x3u;
                        mem.WriteU16(c.S0 + 0x30u, (ushort)c.V0);
                        mem.WriteU8(c.FP, (byte)c.V1);
                        c.A0 = mem.ReadU16(c.S1 + 0x10u);
                        c.A1 = mem.ReadU16(c.S1 + 0x14u);
                        c.A2 = mem.ReadU16(c.S1 + 0x18u);
                        c.V1 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                        c.V0 = c.S0 + 0x4u;
                        mem.WriteU32(c.SP + 0x14u, c.V0);
                        c.V0 = c.S0 + 0x10u;
                        mem.WriteU32(c.SP + 0x18u, c.V0);
                        c.V0 = c.S0 + 0x1Cu;
                        mem.WriteU32(c.SP + 0x1Cu, c.V0);
                        mem.WriteU32(c.SP + 0x10u, c.V1);
                        c.A0 = c.S7 + c.A0;
                        c.A1 = c.S7 + c.A1;
                        c.A2 = c.S7 + c.A2;
                        c.RA = 0x8001DAACu;
                        KingsField1_game.func_8004DD40(c, mem);
                        c.A1 = c.FP - 0x3u;
                        c.A3 = c.S0 + 0x28u;
                        c.A0 = mem.ReadU16(c.S1 + 0x1Cu);
                        c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                        c.A0 = c.S7 + c.A0;
                        c.RA = 0x8001DAC4u;
                        KingsField1_game.func_8004DD14(c, mem);
                        emit = true;
                    }
                }

                if (ft3)
                {
                    c.V0 = mem.ReadU16(c.S1 + 0xEu);
                    c.V1 = mem.ReadU16(c.S1 + 0x12u);
                    c.S2 = c.V0 + c.S6;
                    c.S4 = c.V1 + c.S6;
                    c.V0 = mem.ReadU16(c.S1 + 0x10u);
                    c.A0 = mem.ReadU32(c.S2);
                    c.A2 = mem.ReadU32(c.S4);
                    c.S3 = c.V0 + c.S6;
                    c.A1 = mem.ReadU32(c.S3);
                    c.RA = 0x8001DAF8u;
                    KingsField1_game.func_8004DE4C(c, mem);
                    if ((int)c.V0 > 0)
                    {
                        c.V1 = 0x80070000u;
                        c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
                        c.S0 = mem.ReadU32(c.V1 + 0x8u);
                        c.V0 = c.S0 + 0x20u;
                        mem.WriteU32(c.V1 + 0x8u, c.V0);
                        c.V0 = 0x80070000u;
                        c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
                        c.V1 = mem.ReadU32(c.V0 + 0x8u);
                        c.V0 = mem.ReadU32(c.V0 + 0x4u);
                        c.V0 = c.V0 < c.V1 ? 1u : 0u;
                        if (c.V0 != 0u) break;
                        c.A0 = c.S0;
                        c.RA = 0x8001DB48u;
                        KingsField1_game.SetPolyFT3_game(c, mem);
                        c.V0 = mem.ReadU16(c.S6 + 0x3EA8u);
                        c.V1 = mem.ReadU8(c.S0 + 0x7u);
                        mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S6 + 0x3EAAu);
                        mem.WriteU16(c.S0 + 0x16u, (ushort)c.V0);
                        c.V0 = mem.ReadU32(c.S2);
                        mem.WriteU32(c.S0 + 0x8u, c.V0);
                        c.V0 = mem.ReadU32(c.S3);
                        mem.WriteU32(c.S0 + 0x10u, c.V0);
                        c.V0 = mem.ReadU32(c.S4);
                        mem.WriteU32(c.S0 + 0x18u, c.V0);
                        c.V0 = mem.ReadU16(c.S1);
                        mem.WriteU16(c.S0 + 0xCu, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S1 + 0x4u);
                        mem.WriteU16(c.S0 + 0x14u, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S1 + 0x8u);
                        c.T0 = 0u | 0x0003u;
                        mem.WriteU16(c.S0 + 0x1Cu, (ushort)c.V0);
                        mem.WriteU8(c.FP, (byte)c.V1);
                        c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                        c.V0 = (uint)(short)mem.ReadU16(c.S3 + 0x6u);
                        c.V1 = (uint)(short)mem.ReadU16(c.S4 + 0x6u);
                        c.A2 = c.A2 + c.V0;
                        c.A2 = c.A2 + c.V1;
                        Div(c, c.A2, c.T0); // at = 0x80000000 after
                        c.A2 = c.LO;
                        c.A1 = c.FP - 0x3u;
                        c.A0 = mem.ReadU16(c.S1 + 0xCu);
                        c.A3 = c.S0 + 0x4u;
                        c.A0 = c.S7 + c.A0;
                        c.RA = 0x8001DC00u;
                        KingsField1_game.func_8004DD14(c, mem);
                        c.T0 = 0u | 0x0003u;
                        c.V0 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                        c.V1 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
                        c.A0 = (uint)(short)mem.ReadU16(c.S4 + 0x4u);
                        c.V0 = c.V0 + c.V1;
                        c.V0 = c.V0 + c.A0;
                        Div(c, c.V0, c.T0); // at = 0x80000000 after
                        c.V0 = c.LO;
                        c.T0 = mem.ReadU32(c.SP + 0x30u);
                        c.V0 = (uint)((int)c.V0 >> 2);
                        emit = true;
                    }
                }

                if (ft4)
                {
                    c.V0 = mem.ReadU16(c.S1 + 0x12u);
                    c.V1 = mem.ReadU16(c.S1 + 0x16u);
                    c.S2 = c.V0 + c.S6;
                    c.S4 = c.V1 + c.S6;
                    c.V0 = mem.ReadU16(c.S1 + 0x14u);
                    c.A0 = mem.ReadU32(c.S2);
                    c.A2 = mem.ReadU32(c.S4);
                    c.S3 = c.V0 + c.S6;
                    c.A1 = mem.ReadU32(c.S3);
                    c.RA = 0x8001DC7Cu;
                    KingsField1_game.func_8004DE4C(c, mem);
                    if ((int)c.V0 > 0)
                    {
                        c.V1 = 0x80070000u;
                        c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
                        c.S0 = mem.ReadU32(c.V1 + 0x8u);
                        c.A0 = mem.ReadU16(c.S1 + 0x18u);
                        c.V0 = c.S0 + 0x28u;
                        mem.WriteU32(c.V1 + 0x8u, c.V0);
                        c.V0 = 0x80070000u;
                        c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
                        c.V1 = mem.ReadU32(c.V0 + 0x8u);
                        c.V0 = mem.ReadU32(c.V0 + 0x4u);
                        c.V0 = c.V0 < c.V1 ? 1u : 0u;
                        bool done = c.V0 != 0u;
                        c.S5 = c.A0 + c.S6;
                        if (done) break;
                        c.A0 = c.S0;
                        c.RA = 0x8001DCCCu;
                        KingsField1_game.SetPolyFT4_game(c, mem);
                        c.V0 = mem.ReadU16(c.S6 + 0x3EA8u);
                        c.V1 = mem.ReadU8(c.S0 + 0x7u);
                        mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S6 + 0x3EAAu);
                        mem.WriteU16(c.S0 + 0x16u, (ushort)c.V0);
                        c.V0 = mem.ReadU32(c.S2);
                        mem.WriteU32(c.S0 + 0x8u, c.V0);
                        c.V0 = mem.ReadU32(c.S3);
                        mem.WriteU32(c.S0 + 0x10u, c.V0);
                        c.V0 = mem.ReadU32(c.S4);
                        mem.WriteU32(c.S0 + 0x18u, c.V0);
                        c.V0 = mem.ReadU32(c.S5);
                        mem.WriteU32(c.S0 + 0x20u, c.V0);
                        c.V0 = mem.ReadU16(c.S1);
                        mem.WriteU16(c.S0 + 0xCu, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S1 + 0x4u);
                        mem.WriteU16(c.S0 + 0x14u, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S1 + 0x8u);
                        c.A1 = c.FP - 0x3u;
                        mem.WriteU16(c.S0 + 0x1Cu, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S1 + 0xCu);
                        c.A3 = c.S0 + 0x4u;
                        mem.WriteU16(c.S0 + 0x24u, (ushort)c.V0);
                        mem.WriteU8(c.FP, (byte)c.V1);
                        c.A0 = mem.ReadU16(c.S1 + 0x10u);
                        c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                        c.V0 = (uint)(short)mem.ReadU16(c.S3 + 0x6u);
                        c.V1 = (uint)(short)mem.ReadU16(c.S5 + 0x6u);
                        c.A2 = c.A2 + c.V0;
                        c.V0 = (uint)(short)mem.ReadU16(c.S4 + 0x6u);
                        c.A0 = c.S7 + c.A0;
                        c.A2 = c.A2 + c.V0;
                        c.A2 = c.A2 + c.V1;
                        c.A2 = (uint)((int)c.A2 >> 2);
                        c.RA = 0x8001DD74u;
                        KingsField1_game.func_8004DD14(c, mem);
                        emit = true;
                    }
                }

                // Tail shared by the GT4 and FT4 kinds.
                if (emit && (gt4 || ft4))
                {
                    c.V0 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                    c.V1 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
                    c.A0 = (uint)(short)mem.ReadU16(c.S5 + 0x4u);
                    c.V0 = c.V0 + c.V1;
                    c.V1 = (uint)(short)mem.ReadU16(c.S4 + 0x4u);
                    c.T0 = mem.ReadU32(c.SP + 0x30u);
                    c.V0 = c.V0 + c.V1;
                    c.V0 = c.V0 + c.A0;
                    c.V0 = (uint)((int)c.V0 >> 4);
                }

                // Ordering-table slot from depth plus a1's offset; skipped when the depth is below 5.
                if (emit)
                {
                    c.A0 = c.V0 + c.T0;
                    c.V0 = (int)c.A0 < 5 ? 1u : 0u;
                    bool skip = c.V0 != 0u;
                    c.A1 = c.S0;
                    if (!skip)
                    {
                        c.A0 = c.A0 & 0x3FFFu;
                        c.V0 = mem.ReadU32(c.S6 - 0x2F4u);
                        c.A0 = c.A0 << 2;
                        c.A0 = c.A0 + c.V0;
                        if (PacketDepth.On) PolyAssembler.Depth(mem, c.A1, c.S2, c.S3, c.S4, c.S5);
                        c.RA = 0x8001DDBCu;
                        KingsField1_game.AddPrim_game(c, mem);
                    }
                }

                c.T0 = mem.ReadU32(c.SP + 0x20u);
                c.V0 = c.T0 >> 6;
                c.V0 = c.V0 & 0x03FCu;
                c.S1 = c.S1 + c.V0;
                c.V0 = mem.ReadU32(c.SP + 0x28u);
                c.T0 = c.V0;
                c.T0 = c.T0 - 0x1u;
                bool more = c.V0 != 0u;
                mem.WriteU32(c.SP + 0x28u, c.T0);
                if (!more) break;
            }
        }
        c.RA = mem.ReadU32(c.SP + 0x5Cu);
        c.FP = mem.ReadU32(c.SP + 0x58u);
        c.S7 = mem.ReadU32(c.SP + 0x54u);
        c.S6 = mem.ReadU32(c.SP + 0x50u);
        c.S5 = mem.ReadU32(c.SP + 0x4Cu);
        c.S4 = mem.ReadU32(c.SP + 0x48u);
        c.S3 = mem.ReadU32(c.SP + 0x44u);
        c.S2 = mem.ReadU32(c.SP + 0x40u);
        c.S1 = mem.ReadU32(c.SP + 0x3Cu);
        c.S0 = mem.ReadU32(c.SP + 0x38u);
        c.SP = c.SP + 0x60u;
        return;
    }
}
