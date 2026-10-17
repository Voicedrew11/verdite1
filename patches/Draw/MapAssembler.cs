using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001DE18</c>, the map writer. <c>KF1_MAPASSEMBLER</c>.
/// a0 is the map object index (low 16 bits), resolved to the TMD object by
/// <c>func_8001C114</c>. Each packet of its stream is a GT3 (code 0x24) or GT4
/// (code 0x2C) primitive: the vertices come from the cache at S6 = 0x800911B0
/// (<c>func_8001C60C</c>), the packet is written at the cursor of the record at
/// 0x80070EB8 (cursor at +8, buffer end at +4), and it is added to the ordering
/// table at 0x80090EBC by <c>AddPrim_game</c>.
/// </summary>
public static class MapAssembler
{
    public static readonly Literal R = new(typeof(MapAssembler), "map writer", "MAPASSEMBLER", 0x8001DE18, Run,
        "Writes one map object's FT3/FT4 primitives as GT3/GT4 packets.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x50u;
        mem.WriteU32(c.SP + 0x4Cu, c.RA);
        mem.WriteU32(c.SP + 0x48u, c.FP);
        mem.WriteU32(c.SP + 0x44u, c.S7);
        mem.WriteU32(c.SP + 0x40u, c.S6);
        mem.WriteU32(c.SP + 0x3Cu, c.S5);
        mem.WriteU32(c.SP + 0x38u, c.S4);
        mem.WriteU32(c.SP + 0x34u, c.S3);
        mem.WriteU32(c.SP + 0x30u, c.S2);
        mem.WriteU32(c.SP + 0x2Cu, c.S1);
        mem.WriteU32(c.SP + 0x28u, c.S0);
        c.A0 = c.A0 & 0xFFFFu;
        c.RA = 0x8001DE4Cu;
        KingsField1_game.func_8001C114(c, mem);
        c.S0 = c.V0;
        c.S2 = 0x80090000u;
        c.S2 = c.S2 + 0xFC8u;
        c.A0 = mem.ReadU32(c.S0 + 0x4u);
        c.V0 = mem.ReadU32(c.S0 + 0x8u);
        c.V1 = mem.ReadU32(c.S2);
        c.V0 = c.V0 + 0xCu;
        c.V0 = c.V0 + c.V1;
        mem.WriteU32(c.SP + 0x20u, c.V0);
        c.RA = 0x8001DE74u;
        KingsField1_game.func_8001C60C(c, mem);
        c.S7 = mem.ReadU32(c.S0 + 0x14u);
        c.V0 = mem.ReadU32(c.S0 + 0x10u);
        c.V1 = mem.ReadU32(c.S2);
        c.V0 = c.V0 + 0xCu;
        c.S1 = c.V0 + c.V1;
        c.V0 = c.S7;
        bool none = c.V0 == 0u;
        c.S7 = c.S7 - 0x1u;
        if (!none)
        {
            c.S6 = c.S2 + 0x1E8u;
            c.FP = 0x80050000u;
            c.FP = c.FP + 0x7B63u;
            while (true)
            {
                Poll(c, mem);
                c.A3 = mem.ReadU32(c.S1);
                c.S1 = c.S1 + 0x4u;
                c.V0 = 0u | 0x0024u;
                c.V1 = c.A3 >> 24;
                bool gt3 = c.V1 == c.V0;
                mem.WriteU32(c.SP + 0x18u, c.A3);
                // set when the primitive reaches the ordering table below
                bool drawPrim = false;
                if (!gt3)
                {
                    c.V0 = 0u | 0x002Cu;
                    if (c.V1 == c.V0)
                    {
                        // GT4: the four vertices, then NCLIP on the quad (<= 0 skips it)
                        c.V0 = mem.ReadU16(c.S1 + 0x12u);
                        c.V1 = mem.ReadU16(c.S1 + 0x16u);
                        c.S4 = c.V0 + c.S6;
                        c.S3 = c.V1 + c.S6;
                        c.V0 = mem.ReadU16(c.S1 + 0x14u);
                        c.A0 = mem.ReadU32(c.S4);
                        c.A2 = mem.ReadU32(c.S3);
                        c.S2 = c.V0 + c.S6;
                        c.A1 = mem.ReadU32(c.S2);
                        c.RA = 0x8001DEF0u;
                        Facing.Quad(c, mem, R, c.S4, c.S2, c.S3, mem.ReadU16(c.S1 + 0x18u) + c.S6);
                        if ((int)c.V0 > 0)
                        {
                            // the packet's cursor record (0x80070EB8): take 0x34 bytes
                            c.V1 = 0x80070000u;
                            c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
                            c.S0 = mem.ReadU32(c.V1 + 0x8u);
                            c.A0 = mem.ReadU16(c.S1 + 0x18u);
                            c.V0 = c.S0 + 0x34u;
                            mem.WriteU32(c.V1 + 0x8u, c.V0);
                            c.V0 = 0x80070000u;
                            c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
                            c.V1 = mem.ReadU32(c.V0 + 0x8u);
                            c.V0 = mem.ReadU32(c.V0 + 0x4u);
                            c.V0 = c.V0 < c.V1 ? 1u : 0u;
                            // out of buffer space: stop the object
                            bool exit = c.V0 != 0u;
                            c.S5 = c.A0 + c.S6;
                            if (exit) break;
                            c.A0 = c.S0;
                            c.RA = 0x8001DF40u;
                            KingsField1_game.SetPolyGT4_game(c, mem);
                            c.V0 = mem.ReadU16(c.S1 + 0x2u);
                            c.V1 = mem.ReadU8(c.S0 + 0x7u);
                            mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
                            c.V0 = mem.ReadU16(c.S1 + 0x6u);
                            mem.WriteU16(c.S0 + 0x1Au, (ushort)c.V0);
                            c.V0 = mem.ReadU32(c.S4);
                            mem.WriteU32(c.S0 + 0x8u, c.V0);
                            c.V0 = mem.ReadU32(c.S2);
                            mem.WriteU32(c.S0 + 0x14u, c.V0);
                            c.V0 = mem.ReadU32(c.S3);
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
                            c.A1 = c.FP - 0x3u;
                            mem.WriteU16(c.S0 + 0x30u, (ushort)c.V0);
                            mem.WriteU8(c.FP, (byte)c.V1);
                            c.A0 = mem.ReadU16(c.S1 + 0x10u);
                            c.A3 = mem.ReadU32(c.SP + 0x20u);
                            c.A2 = c.SP + 0x10u;
                            c.A0 = c.A3 + c.A0;
                            c.RA = 0x8001DFD0u;
                            KingsField1_game.func_8004DD8C(c, mem);
                            c.A0 = c.SP + 0x10u;
                            c.A1 = (uint)(short)mem.ReadU16(c.S4 + 0x6u);
                            c.A2 = c.S0 + 0x4u;
                            c.RA = 0x8001DFE0u;
                            KingsField1_game.func_8004DBC4(c, mem);
                            c.A0 = c.SP + 0x10u;
                            c.A1 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                            c.A2 = c.S0 + 0x10u;
                            c.RA = 0x8001DFF0u;
                            KingsField1_game.func_8004DBC4(c, mem);
                            c.A0 = c.SP + 0x10u;
                            c.A1 = (uint)(short)mem.ReadU16(c.S3 + 0x6u);
                            c.A2 = c.S0 + 0x1Cu;
                            c.RA = 0x8001E000u;
                            KingsField1_game.func_8004DBC4(c, mem);
                            c.A0 = c.SP + 0x10u;
                            c.A1 = (uint)(short)mem.ReadU16(c.S5 + 0x6u);
                            c.A2 = c.S0 + 0x28u;
                            c.RA = 0x8001E010u;
                            KingsField1_game.func_8004DBC4(c, mem);
                            c.V0 = (uint)(short)mem.ReadU16(c.S4 + 0x4u);
                            c.V1 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                            c.V0 = c.V0 + c.V1;
                            c.V1 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
                            c.A0 = (uint)(short)mem.ReadU16(c.S5 + 0x4u);
                            c.V0 = c.V0 + c.V1;
                            c.V0 = c.V0 + c.A0;
                            c.V0 = (uint)((int)c.V0 >> 4);
                            c.A0 = c.V0 + 0xC8u;
                            c.V0 = (int)c.A0 < 16384 ? 1u : 0u;
                            // the ordering-table slot must be inside the table (< 0x4000)
                            drawPrim = c.V0 != 0u;
                            c.A1 = c.S0;
                            if (drawPrim)
                            {
                                c.A0 = c.A0 & 0x3FFFu;
                            }
                        }
                    }
                }
                else
                {
                    // GT3: the three vertices, then NCLIP (<= 0 skips it)
                    c.V0 = mem.ReadU16(c.S1 + 0xEu);
                    c.V1 = mem.ReadU16(c.S1 + 0x12u);
                    c.S4 = c.V0 + c.S6;
                    c.S3 = c.V1 + c.S6;
                    c.V0 = mem.ReadU16(c.S1 + 0x10u);
                    c.A0 = mem.ReadU32(c.S4);
                    c.A2 = mem.ReadU32(c.S3);
                    c.S2 = c.V0 + c.S6;
                    c.A1 = mem.ReadU32(c.S2);
                    c.RA = 0x8001E078u;
                    Facing.Nclip(c, mem, R, c.S4, c.S2, c.S3);
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
                        // out of buffer space: stop the object
                        if (c.V0 != 0u) break;
                        c.A0 = c.S0;
                        c.RA = 0x8001E0C8u;
                        KingsField1_game.SetPolyGT3_game(c, mem);
                        c.V0 = mem.ReadU16(c.S1 + 0x2u);
                        c.V1 = mem.ReadU8(c.S0 + 0x7u);
                        mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S1 + 0x6u);
                        mem.WriteU16(c.S0 + 0x1Au, (ushort)c.V0);
                        c.V0 = mem.ReadU32(c.S4);
                        mem.WriteU32(c.S0 + 0x8u, c.V0);
                        c.V0 = mem.ReadU32(c.S2);
                        mem.WriteU32(c.S0 + 0x14u, c.V0);
                        c.V0 = mem.ReadU32(c.S3);
                        mem.WriteU32(c.S0 + 0x20u, c.V0);
                        c.V0 = mem.ReadU16(c.S1);
                        mem.WriteU16(c.S0 + 0xCu, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S1 + 0x4u);
                        mem.WriteU16(c.S0 + 0x18u, (ushort)c.V0);
                        c.V0 = mem.ReadU16(c.S1 + 0x8u);
                        c.A1 = c.FP - 0x3u;
                        mem.WriteU16(c.S0 + 0x24u, (ushort)c.V0);
                        mem.WriteU8(c.FP, (byte)c.V1);
                        c.A0 = mem.ReadU16(c.S1 + 0xCu);
                        c.A3 = mem.ReadU32(c.SP + 0x20u);
                        c.A2 = c.SP + 0x10u;
                        c.A0 = c.A3 + c.A0;
                        c.RA = 0x8001E140u;
                        KingsField1_game.func_8004DD8C(c, mem);
                        c.A0 = c.SP + 0x10u;
                        c.A1 = (uint)(short)mem.ReadU16(c.S4 + 0x6u);
                        c.A2 = c.S0 + 0x4u;
                        c.RA = 0x8001E150u;
                        KingsField1_game.func_8004DBC4(c, mem);
                        c.A0 = c.SP + 0x10u;
                        c.A1 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                        c.A2 = c.S0 + 0x10u;
                        c.RA = 0x8001E160u;
                        KingsField1_game.func_8004DBC4(c, mem);
                        c.A0 = c.SP + 0x10u;
                        c.A1 = (uint)(short)mem.ReadU16(c.S3 + 0x6u);
                        c.A2 = c.S0 + 0x1Cu;
                        c.RA = 0x8001E170u;
                        KingsField1_game.func_8004DBC4(c, mem);
                        c.V1 = (uint)(short)mem.ReadU16(c.S4 + 0x4u);
                        c.V0 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                        c.A0 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
                        c.V1 = c.V1 + c.V0;
                        c.V1 = c.V1 + c.A0;
                        c.V0 = 0u | 0x0003u;
                        Div(c, c.V1, c.V0); // at = 0x80000000 after
                        c.V1 = c.LO;
                        c.V1 = (uint)((int)c.V1 >> 2);
                        c.V1 = c.V1 + 0xC8u;
                        c.V0 = (int)c.V1 < 16384 ? 1u : 0u;
                        // the ordering-table slot must be inside the table (< 0x4000)
                        drawPrim = c.V0 != 0u;
                        c.A1 = c.S0;
                        if (drawPrim)
                        {
                            c.A0 = c.V1 & 0x3FFFu;
                        }
                    }
                }
                if (drawPrim)
                {
                    // AddPrim at ordering-table slot A0 (word index << 2 from 0x80090EBC)
                    c.V0 = mem.ReadU32(c.S6 - 0x2F4u);
                    c.A0 = c.A0 << 2;
                    c.A0 = c.A0 + c.V0;
                    if (PacketDepth.On) PacketDepth.Face(mem, c.A1, c.S4, c.S2, c.S3, c.S5);
                    if (RetainedMap.Checking) RetainedMap.CheckFace(mem, c.A1, c.S1, c.S4, c.S2, c.S3, c.S5);
                    c.RA = 0x8001E1E0u;
                    KingsField1_game.AddPrim_game(c, mem);
                }
                // advance the source cursor by the record's word count, then loop
                c.A3 = mem.ReadU32(c.SP + 0x18u);
                c.V0 = c.A3 >> 6;
                c.V0 = c.V0 & 0x03FCu;
                c.S1 = c.S1 + c.V0;
                c.V0 = c.S7;
                bool again = c.V0 != 0u;
                c.S7 = c.S7 - 0x1u;
                if (!again) break;
            }
        }
        c.RA = mem.ReadU32(c.SP + 0x4Cu);
        c.FP = mem.ReadU32(c.SP + 0x48u);
        c.S7 = mem.ReadU32(c.SP + 0x44u);
        c.S6 = mem.ReadU32(c.SP + 0x40u);
        c.S5 = mem.ReadU32(c.SP + 0x3Cu);
        c.S4 = mem.ReadU32(c.SP + 0x38u);
        c.S3 = mem.ReadU32(c.SP + 0x34u);
        c.S2 = mem.ReadU32(c.SP + 0x30u);
        c.S1 = mem.ReadU32(c.SP + 0x2Cu);
        c.S0 = mem.ReadU32(c.SP + 0x28u);
        c.SP = c.SP + 0x50u;
        return;
    }
}
