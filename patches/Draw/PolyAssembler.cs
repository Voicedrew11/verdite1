using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001C7F8</c>, the polygon writer, transcribed literally. <c>KF1_POLYASSEMBLER</c>.
/// Walks one TMD model's primitives and writes each as a lit packet, linked into the
/// ordering table (0x80090EBC) through <c>AddPrim_game</c>. Arguments: a0 = the model
/// (low 16 bits, resolved by <c>func_8001C114</c>); a1 = the depth offset (low 16 bits,
/// sign-extended into FP). Packets come from the record at 0x80070EB8.
/// </summary>
public static class PolyAssembler
{
    public static readonly Literal R = new(typeof(PolyAssembler), "polygon writer", "POLYASSEMBLER", 0x8001C7F8, Run,
        "Writes a model's TMD primitives as lit packets: every submitter, the arm and the compass.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    /// <summary>The model walk's packets are recorded for the Z-buffer; the arm's,
    /// the compass's and the menu's are dropped so a stale record cannot match.</summary>
    internal static void Depth(PSMemory mem, uint pkt, uint p0, uint p1, uint p2, uint p3)
    {
        if (ModelWalk.Carrying) PacketDepth.Face(mem, pkt, p0, p1, p2, p3);
        else PacketDepth.None(pkt);
    }

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x58u;
        mem.WriteU32(c.SP + 0x54u, c.RA);
        mem.WriteU32(c.SP + 0x50u, c.FP);
        mem.WriteU32(c.SP + 0x4Cu, c.S7);
        mem.WriteU32(c.SP + 0x48u, c.S6);
        mem.WriteU32(c.SP + 0x44u, c.S5);
        mem.WriteU32(c.SP + 0x40u, c.S4);
        mem.WriteU32(c.SP + 0x3Cu, c.S3);
        mem.WriteU32(c.SP + 0x38u, c.S2);
        mem.WriteU32(c.SP + 0x34u, c.S1);
        mem.WriteU32(c.SP + 0x30u, c.S0);
        // S0 = a1 (the depth offset); the model comes back in V0 from func_8001C114
        c.S0 = c.A1;
        c.A0 = c.A0 & 0xFFFFu;
        c.RA = 0x8001C830u;
        KingsField1_game.func_8001C114(c, mem);
        // T0 = the model's primitive count (+0x14); SP+0x28 counts down through it
        c.T0 = mem.ReadU32(c.V0 + 0x14u);
        c.A1 = 0x80090000u;
        c.A1 = c.A1 + 0xFC8u;
        mem.WriteU32(c.SP + 0x28u, c.T0);
        c.V1 = mem.ReadU32(c.V0 + 0x10u);
        c.A0 = mem.ReadU32(c.A1);
        c.V1 = c.V1 + 0xCu;
        c.S1 = c.V1 + c.A0; // S1: the primitive cursor
        c.V1 = mem.ReadU32(c.SP + 0x28u);
        c.V0 = mem.ReadU32(c.V0 + 0x8u);
        c.T0 = c.T0 - 0x1u;
        mem.WriteU32(c.SP + 0x28u, c.T0);
        c.V0 = c.V0 + 0xCu;
        bool noPrimitives = c.V1 == 0u;
        c.S7 = c.V0 + c.A0;
        if (noPrimitives) {
            goto L8001D700;
        }
        c.S6 = c.A1 + 0x1E8u; // S6 = 0x800911B0: corner indices are offset by it
        c.V0 = c.S0 << 16;
        c.FP = (uint)((int)c.V0 >> 16);
        while (true)
        {
            Poll(c, mem);
            c.T0 = mem.ReadU32(c.S1);
            c.S1 = c.S1 + 0x4u;
            // the top byte of the primitive's first word is its code
            c.V0 = c.T0 >> 24;
            c.V1 = c.V0 - 0x20u;
            c.V0 = 0u | 0x001Cu;
            c.V0 = c.V0 < c.V1 ? 1u : 0u;
            bool outOfRange = c.V0 != 0u; // codes 0x20..0x3C only; others go to the tail
            mem.WriteU32(c.SP + 0x20u, c.T0);
            if (outOfRange) {
                goto L8001D6D4;
            }
            // the table at 0x8001222C maps (code - 0x20) to a handler's address
            c.V0 = c.V1 << 2;
            c.At = 0x80010000u;
            c.At = c.At + 0x222Cu;
            c.At = c.At + c.V0;
            c.V0 = mem.ReadU32(c.At);
            switch (c.V0)
            {
                case 0x8001D204u: goto L8001D204;
                case 0x8001D6D4u: goto L8001D6D4;
                case 0x8001D45Cu: goto L8001D45C;
                case 0x8001C8B8u: goto L8001C8B8;
                case 0x8001CA44u: goto L8001CA44;
                case 0x8001D5A8u: goto L8001D5A8;
                case 0x8001D0F8u: goto L8001D0F8;
                case 0x8001CAC8u: goto L8001CAC8;
                case 0x8001CFCCu: goto L8001CFCC;
                case 0x8001CCF8u: goto L8001CCF8;
                case 0x8001CBF0u: goto L8001CBF0;
                case 0x8001D344u: goto L8001D344;
                case 0x8001CE68u: goto L8001CE68;
                // a handler outside this list runs through the dispatcher, then the routine returns
                default: Dispatcher.Call(c, mem, c.V0); return;
            }

            // FT3: textured flat triangle, NCLIP-culled
            // FT3: textured flat triangle (SetPolyFT3), NCLIP-culled; 0x20-byte packet
            L8001C8B8: ;
            c.V0 = mem.ReadU16(c.S1 + 0xEu);
            c.V1 = mem.ReadU16(c.S1 + 0x12u);
            c.S2 = c.V0 + c.S6;
            c.S4 = c.V1 + c.S6;
            c.V0 = mem.ReadU16(c.S1 + 0x10u);
            c.A0 = mem.ReadU32(c.S2);
            c.A2 = mem.ReadU32(c.S4);
            c.S3 = c.V0 + c.S6;
            c.A1 = mem.ReadU32(c.S3);
            c.RA = 0x8001C8E4u;
            KingsField1_game.func_8004DE4C(c, mem);
            if ((int)c.V0 > 0) {
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
                if (c.V0 != 0u) {
                    goto L8001D700;
                }
                c.A0 = c.S0;
                c.RA = 0x8001C934u;
                KingsField1_game.SetPolyFT3_game(c, mem);
                c.V0 = mem.ReadU16(c.S1 + 0x2u);
                c.V1 = mem.ReadU8(c.S0 + 0x7u);
                mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0x6u);
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
                c.At = 0x80050000u;
                mem.WriteU8(c.At + 0x7B5Bu, (byte)c.V1);
                c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                c.V0 = (uint)(short)mem.ReadU16(c.S3 + 0x6u);
                c.V1 = (uint)(short)mem.ReadU16(c.S4 + 0x6u);
                c.A2 = c.A2 + c.V0;
                c.A2 = c.A2 + c.V1;
                Div(c, c.A2, c.T0); // at = 0x80000000 after
                c.A2 = c.LO;
                c.A3 = c.S0 + 0x4u;
                c.A0 = mem.ReadU16(c.S1 + 0xCu);
                c.T0 = 0x80050000u;
                c.T0 = c.T0 + 0x7B5Bu;
                c.A1 = c.T0 - 0x3u;
                c.A0 = c.S7 + c.A0;
                c.RA = 0x8001C9F8u;
                KingsField1_game.func_8004DD14(c, mem);
                c.T0 = 0u | 0x0003u;
                c.V0 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                c.V1 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
                c.A0 = (uint)(short)mem.ReadU16(c.S4 + 0x4u);
                c.V0 = c.V0 + c.V1;
                c.V0 = c.V0 + c.A0;
                Div(c, c.V0, c.T0); // at = 0x80000000 after
                c.V0 = c.LO;
                c.V0 = (uint)((int)c.V0 >> 2);
                goto L8001D6B0;
            }
            goto L8001D6D4;

            // F4: flat quad (SetPolyF4), NCLIP-culled; 0x18-byte packet, then the shared quad tail
            L8001CA44: ;
            c.V0 = mem.ReadU16(c.S1 + 0x6u);
            c.V1 = mem.ReadU16(c.S1 + 0xAu);
            c.S2 = c.V0 + c.S6;
            c.S4 = c.V1 + c.S6;
            c.V0 = mem.ReadU16(c.S1 + 0x8u);
            c.A0 = mem.ReadU32(c.S2);
            c.A2 = mem.ReadU32(c.S4);
            c.S3 = c.V0 + c.S6;
            c.A1 = mem.ReadU32(c.S3);
            c.RA = 0x8001CA70u;
            KingsField1_game.func_8004DE4C(c, mem);
            if ((int)c.V0 > 0) {
                c.V1 = 0x80070000u;
                c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
                c.S0 = mem.ReadU32(c.V1 + 0x8u);
                c.A0 = mem.ReadU16(c.S1 + 0xCu);
                c.V0 = c.S0 + 0x18u;
                mem.WriteU32(c.V1 + 0x8u, c.V0);
                c.V0 = 0x80070000u;
                c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
                c.V1 = mem.ReadU32(c.V0 + 0x8u);
                c.V0 = mem.ReadU32(c.V0 + 0x4u);
                c.V0 = c.V0 < c.V1 ? 1u : 0u;
                bool full = c.V0 != 0u;
                c.S5 = c.A0 + c.S6;
                if (full) {
                    goto L8001D700;
                }
                c.A0 = c.S0;
                c.RA = 0x8001CAC0u;
                KingsField1_game.SetPolyF4_game(c, mem);
                goto L8001D630;
            }
            goto L8001D6D4;

            // G3: Gouraud triangle (SetPolyG3), NCLIP-culled; 0x1C-byte packet
            // G3: Gouraud triangle (SetPolyG3), NCLIP-culled; 0x1C-byte packet
            L8001CAC8: ;
            c.V0 = mem.ReadU16(c.S1 + 0x6u);
            c.V1 = mem.ReadU16(c.S1 + 0xEu);
            c.S2 = c.V0 + c.S6;
            c.S4 = c.V1 + c.S6;
            c.V0 = mem.ReadU16(c.S1 + 0xAu);
            c.A0 = mem.ReadU32(c.S2);
            c.A2 = mem.ReadU32(c.S4);
            c.S3 = c.V0 + c.S6;
            c.A1 = mem.ReadU32(c.S3);
            c.RA = 0x8001CAF4u;
            KingsField1_game.func_8004DE4C(c, mem);
            if ((int)c.V0 > 0) {
                c.V1 = 0x80070000u;
                c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
                c.S0 = mem.ReadU32(c.V1 + 0x8u);
                c.V0 = c.S0 + 0x1Cu;
                mem.WriteU32(c.V1 + 0x8u, c.V0);
                c.V0 = 0x80070000u;
                c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
                c.V1 = mem.ReadU32(c.V0 + 0x8u);
                c.V0 = mem.ReadU32(c.V0 + 0x4u);
                c.V0 = c.V0 < c.V1 ? 1u : 0u;
                if (c.V0 != 0u) {
                    goto L8001D700;
                }
                c.A0 = c.S0;
                c.RA = 0x8001CB44u;
                KingsField1_game.SetPolyG3_game(c, mem);
                c.V0 = mem.ReadU32(c.S2);
                mem.WriteU32(c.S0 + 0x8u, c.V0);
                c.V0 = mem.ReadU32(c.S3);
                mem.WriteU32(c.S0 + 0x10u, c.V0);
                c.V0 = mem.ReadU32(c.S4);
                c.A3 = c.S1;
                mem.WriteU32(c.S0 + 0x18u, c.V0);
                c.A0 = mem.ReadU16(c.S1 + 0x4u);
                c.A1 = mem.ReadU16(c.S1 + 0x8u);
                c.A2 = mem.ReadU16(c.S1 + 0xCu);
                c.V1 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                c.V0 = c.S0 + 0x4u;
                mem.WriteU32(c.SP + 0x14u, c.V0);
                c.V0 = c.S0 + 0xCu;
                mem.WriteU32(c.SP + 0x18u, c.V0);
                c.V0 = c.S0 + 0x14u;
                mem.WriteU32(c.SP + 0x1Cu, c.V0);
                mem.WriteU32(c.SP + 0x10u, c.V1);
                c.A0 = c.S7 + c.A0;
                c.A1 = c.S7 + c.A1;
                c.A2 = c.S7 + c.A2;
                c.RA = 0x8001CBA4u;
                KingsField1_game.func_8004DD40(c, mem);
                c.T0 = 0u | 0x0003u;
                c.V0 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                c.V1 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
                c.A0 = (uint)(short)mem.ReadU16(c.S4 + 0x4u);
                c.V0 = c.V0 + c.V1;
                c.V0 = c.V0 + c.A0;
                Div(c, c.V0, c.T0); // at = 0x80000000 after
                c.V0 = c.LO;
                c.V0 = (uint)((int)c.V0 >> 2);
                goto L8001D6B0;
            }
            goto L8001D6D4;

            // G4: Gouraud quad (SetPolyG4), NCLIP-culled; 0x24-byte packet
            // G4: Gouraud quad (SetPolyG4), NCLIP-culled; 0x24-byte packet, ends in the shared tail
            L8001CBF0: ;
            c.V0 = mem.ReadU16(c.S1 + 0x6u);
            c.V1 = mem.ReadU16(c.S1 + 0xEu);
            c.S2 = c.V0 + c.S6;
            c.S4 = c.V1 + c.S6;
            c.V0 = mem.ReadU16(c.S1 + 0xAu);
            c.A0 = mem.ReadU32(c.S2);
            c.A2 = mem.ReadU32(c.S4);
            c.S3 = c.V0 + c.S6;
            c.A1 = mem.ReadU32(c.S3);
            c.RA = 0x8001CC1Cu;
            KingsField1_game.func_8004DE4C(c, mem);
            if ((int)c.V0 > 0) {
                c.V1 = 0x80070000u;
                c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
                c.S0 = mem.ReadU32(c.V1 + 0x8u);
                c.A0 = mem.ReadU16(c.S1 + 0x12u);
                c.V0 = c.S0 + 0x24u;
                mem.WriteU32(c.V1 + 0x8u, c.V0);
                c.V0 = 0x80070000u;
                c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
                c.V1 = mem.ReadU32(c.V0 + 0x8u);
                c.V0 = mem.ReadU32(c.V0 + 0x4u);
                c.V0 = c.V0 < c.V1 ? 1u : 0u;
                bool full = c.V0 != 0u;
                c.S5 = c.A0 + c.S6;
                if (full) {
                    goto L8001D700;
                }
                c.A0 = c.S0;
                c.RA = 0x8001CC6Cu;
                KingsField1_game.SetPolyG4_game(c, mem);
                c.V0 = mem.ReadU32(c.S2);
                mem.WriteU32(c.S0 + 0x8u, c.V0);
                c.V0 = mem.ReadU32(c.S3);
                mem.WriteU32(c.S0 + 0x10u, c.V0);
                c.V0 = mem.ReadU32(c.S4);
                mem.WriteU32(c.S0 + 0x18u, c.V0);
                c.V0 = mem.ReadU32(c.S5);
                c.A3 = c.S1;
                mem.WriteU32(c.S0 + 0x20u, c.V0);
                c.A0 = mem.ReadU16(c.S1 + 0x4u);
                c.A1 = mem.ReadU16(c.S1 + 0x8u);
                c.A2 = mem.ReadU16(c.S1 + 0xCu);
                c.V1 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                c.V0 = c.S0 + 0x4u;
                mem.WriteU32(c.SP + 0x14u, c.V0);
                c.V0 = c.S0 + 0xCu;
                mem.WriteU32(c.SP + 0x18u, c.V0);
                c.V0 = c.S0 + 0x14u;
                mem.WriteU32(c.SP + 0x1Cu, c.V0);
                mem.WriteU32(c.SP + 0x10u, c.V1);
                c.A0 = c.S7 + c.A0;
                c.A1 = c.S7 + c.A1;
                c.A2 = c.S7 + c.A2;
                c.RA = 0x8001CCD8u;
                KingsField1_game.func_8004DD40(c, mem);
                c.A1 = c.S1;
                c.A3 = c.S0 + 0x1Cu;
                c.A0 = mem.ReadU16(c.S1 + 0x10u);
                c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                c.A0 = c.S7 + c.A0;
                c.RA = 0x8001CCF0u;
                KingsField1_game.func_8004DD14(c, mem);
                goto L8001D68C;
            }
            goto L8001D6D4;

            // GT3: textured Gouraud triangle (SetPolyGT3), NCLIP-culled; 0x28-byte packet
            L8001CCF8: ;
            c.V0 = mem.ReadU16(c.S1 + 0xEu);
            c.V1 = mem.ReadU16(c.S1 + 0x16u);
            c.S2 = c.V0 + c.S6;
            c.S4 = c.V1 + c.S6;
            c.V0 = mem.ReadU16(c.S1 + 0x12u);
            c.A0 = mem.ReadU32(c.S2);
            c.A2 = mem.ReadU32(c.S4);
            c.S3 = c.V0 + c.S6;
            c.A1 = mem.ReadU32(c.S3);
            c.RA = 0x8001CD24u;
            KingsField1_game.func_8004DE4C(c, mem);
            if ((int)c.V0 > 0) {
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
                if (c.V0 != 0u) {
                    goto L8001D700;
                }
                c.A0 = c.S0;
                c.RA = 0x8001CD74u;
                KingsField1_game.SetPolyGT3_game(c, mem);
                c.V0 = mem.ReadU16(c.S1 + 0x2u);
                c.V1 = mem.ReadU8(c.S0 + 0x7u);
                mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0x6u);
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
                c.T0 = 0x80050000u;
                c.T0 = c.T0 + 0x7B5Bu;
                mem.WriteU16(c.S0 + 0x18u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0x8u);
                c.A3 = c.T0 - 0x3u;
                mem.WriteU16(c.S0 + 0x24u, (ushort)c.V0);
                c.At = 0x80050000u;
                mem.WriteU8(c.At + 0x7B5Bu, (byte)c.V1);
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
                c.RA = 0x8001CE1Cu;
                KingsField1_game.func_8004DD40(c, mem);
                c.T0 = 0u | 0x0003u;
                c.V0 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                c.V1 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
                c.A0 = (uint)(short)mem.ReadU16(c.S4 + 0x4u);
                c.V0 = c.V0 + c.V1;
                c.V0 = c.V0 + c.A0;
                Div(c, c.V0, c.T0); // at = 0x80000000 after
                c.V0 = c.LO;
                c.V0 = (uint)((int)c.V0 >> 2);
                goto L8001D6B0;
            }
            goto L8001D6D4;

            // GT4: textured Gouraud quad (SetPolyGT4), NCLIP-culled; 0x34-byte packet
            // GT4: textured Gouraud quad (SetPolyGT4), NCLIP-culled; 0x34-byte packet, ends in the shared tail
            L8001CE68: ;
            c.V0 = mem.ReadU16(c.S1 + 0x12u);
            c.V1 = mem.ReadU16(c.S1 + 0x1Au);
            c.S2 = c.V0 + c.S6;
            c.S4 = c.V1 + c.S6;
            c.V0 = mem.ReadU16(c.S1 + 0x16u);
            c.A0 = mem.ReadU32(c.S2);
            c.A2 = mem.ReadU32(c.S4);
            c.S3 = c.V0 + c.S6;
            c.A1 = mem.ReadU32(c.S3);
            c.RA = 0x8001CE94u;
            KingsField1_game.func_8004DE4C(c, mem);
            if ((int)c.V0 > 0) {
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
                bool full = c.V0 != 0u;
                c.S5 = c.A0 + c.S6;
                if (full) {
                    goto L8001D700;
                }
                c.A0 = c.S0;
                c.RA = 0x8001CEE4u;
                KingsField1_game.SetPolyGT4_game(c, mem);
                c.V0 = mem.ReadU16(c.S1 + 0x2u);
                c.V1 = mem.ReadU8(c.S0 + 0x7u);
                mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0x6u);
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
                c.T0 = 0x80050000u;
                c.T0 = c.T0 + 0x7B5Bu;
                mem.WriteU16(c.S0 + 0x24u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0xCu);
                c.A3 = c.T0 - 0x3u;
                mem.WriteU16(c.S0 + 0x30u, (ushort)c.V0);
                c.At = 0x80050000u;
                mem.WriteU8(c.At + 0x7B5Bu, (byte)c.V1);
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
                c.RA = 0x8001CFA4u;
                KingsField1_game.func_8004DD40(c, mem);
                c.T0 = 0x80050000u;
                c.T0 = c.T0 + 0x7B5Bu;
                c.A1 = c.T0 - 0x3u;
                c.A3 = c.S0 + 0x28u;
                c.A0 = mem.ReadU16(c.S1 + 0x1Cu);
                c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                c.A0 = c.S7 + c.A0;
                c.RA = 0x8001CFC4u;
                KingsField1_game.func_8004DD14(c, mem);
                goto L8001D68C;
            }
            goto L8001D6D4;

            // G3 semi-transparent: Gouraud triangle (SetPolyG3 then SetSemiTrans), NCLIP-culled
            L8001CFCC: ;
            c.V0 = mem.ReadU16(c.S1 + 0x6u);
            c.V1 = mem.ReadU16(c.S1 + 0xEu);
            c.S2 = c.V0 + c.S6;
            c.S4 = c.V1 + c.S6;
            c.V0 = mem.ReadU16(c.S1 + 0xAu);
            c.A0 = mem.ReadU32(c.S2);
            c.A2 = mem.ReadU32(c.S4);
            c.S3 = c.V0 + c.S6;
            c.A1 = mem.ReadU32(c.S3);
            c.RA = 0x8001CFF8u;
            KingsField1_game.func_8004DE4C(c, mem);
            if ((int)c.V0 > 0) {
                c.V1 = 0x80070000u;
                c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
                c.S0 = mem.ReadU32(c.V1 + 0x8u);
                c.V0 = c.S0 + 0x1Cu;
                mem.WriteU32(c.V1 + 0x8u, c.V0);
                c.V0 = 0x80070000u;
                c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
                c.V1 = mem.ReadU32(c.V0 + 0x8u);
                c.V0 = mem.ReadU32(c.V0 + 0x4u);
                c.V0 = c.V0 < c.V1 ? 1u : 0u;
                if (c.V0 != 0u) {
                    goto L8001D700;
                }
                c.A0 = c.S0;
                c.RA = 0x8001D048u;
                KingsField1_game.SetPolyG3_game(c, mem);
                c.A0 = c.S0;
                c.A1 = 0u | 0x0001u;
                c.RA = 0x8001D054u;
                KingsField1_game.SetSemiTrans_game(c, mem);
                c.V0 = mem.ReadU32(c.S2);
                mem.WriteU32(c.S0 + 0x8u, c.V0);
                c.V0 = mem.ReadU32(c.S3);
                mem.WriteU32(c.S0 + 0x10u, c.V0);
                c.V0 = mem.ReadU32(c.S4);
                c.A3 = c.S1;
                mem.WriteU32(c.S0 + 0x18u, c.V0);
                c.A0 = mem.ReadU16(c.S1 + 0x4u);
                c.A1 = mem.ReadU16(c.S1 + 0x8u);
                c.A2 = mem.ReadU16(c.S1 + 0xCu);
                c.V0 = c.S0 + 0x4u;
                mem.WriteU32(c.SP + 0x10u, c.V0);
                c.V0 = c.S0 + 0xCu;
                mem.WriteU32(c.SP + 0x14u, c.V0);
                c.V0 = c.S0 + 0x14u;
                mem.WriteU32(c.SP + 0x18u, c.V0);
                c.A0 = c.S7 + c.A0;
                c.A1 = c.S7 + c.A1;
                c.A2 = c.S7 + c.A2;
                c.RA = 0x8001D0ACu;
                KingsField1_game.func_8004DDB0(c, mem);
                c.T0 = 0u | 0x0003u;
                c.V0 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                c.V1 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
                c.A0 = (uint)(short)mem.ReadU16(c.S4 + 0x4u);
                c.V0 = c.V0 + c.V1;
                c.V0 = c.V0 + c.A0;
                Div(c, c.V0, c.T0); // at = 0x80000000 after
                c.V0 = c.LO;
                c.V0 = (uint)((int)c.V0 >> 2);
                goto L8001D6B0;
            }
            goto L8001D6D4;

            // FT4: textured flat quad (SetPolyFT4), NCLIP-culled; 0x28-byte packet, ends in the shared tail
            L8001D0F8: ;
            c.V0 = mem.ReadU16(c.S1 + 0x12u);
            c.V1 = mem.ReadU16(c.S1 + 0x16u);
            c.S2 = c.V0 + c.S6;
            c.S4 = c.V1 + c.S6;
            c.V0 = mem.ReadU16(c.S1 + 0x14u);
            c.A0 = mem.ReadU32(c.S2);
            c.A2 = mem.ReadU32(c.S4);
            c.S3 = c.V0 + c.S6;
            c.A1 = mem.ReadU32(c.S3);
            c.RA = 0x8001D124u;
            KingsField1_game.func_8004DE4C(c, mem);
            if ((int)c.V0 > 0) {
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
                bool full = c.V0 != 0u;
                c.S5 = c.A0 + c.S6;
                if (full) {
                    goto L8001D700;
                }
                c.A0 = c.S0;
                c.RA = 0x8001D174u;
                KingsField1_game.SetPolyFT4_game(c, mem);
                c.V0 = mem.ReadU16(c.S1 + 0x2u);
                c.V1 = mem.ReadU8(c.S0 + 0x7u);
                mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0x6u);
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
                c.T0 = 0x80050000u;
                c.T0 = c.T0 + 0x7B5Bu;
                mem.WriteU16(c.S0 + 0x14u, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0x8u);
                c.A1 = c.T0 - 0x3u;
                mem.WriteU16(c.S0 + 0x1Cu, (ushort)c.V0);
                c.V0 = mem.ReadU16(c.S1 + 0xCu);
                mem.WriteU16(c.S0 + 0x24u, (ushort)c.V0);
                c.At = 0x80050000u;
                mem.WriteU8(c.At + 0x7B5Bu, (byte)c.V1);
                c.A0 = mem.ReadU16(c.S1 + 0x10u);
                c.A3 = c.S0 + 0x4u;
                goto L8001D664;
            }
            goto L8001D6D4;

            // F3: flat triangle (SetPolyF3), NCLIP-culled; 0x14-byte packet
            L8001D204: ;
            c.V0 = mem.ReadU16(c.S1 + 0x6u);
            c.V1 = mem.ReadU16(c.S1 + 0xAu);
            c.S2 = c.V0 + c.S6;
            c.S4 = c.V1 + c.S6;
            c.V0 = mem.ReadU16(c.S1 + 0x8u);
            c.A0 = mem.ReadU32(c.S2);
            c.A2 = mem.ReadU32(c.S4);
            c.S3 = c.V0 + c.S6;
            c.A1 = mem.ReadU32(c.S3);
            c.RA = 0x8001D230u;
            KingsField1_game.func_8004DE4C(c, mem);
            if ((int)c.V0 > 0) {
                c.V1 = 0x80070000u;
                c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
                c.S0 = mem.ReadU32(c.V1 + 0x8u);
                c.V0 = c.S0 + 0x14u;
                mem.WriteU32(c.V1 + 0x8u, c.V0);
                c.V0 = 0x80070000u;
                c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
                c.V1 = mem.ReadU32(c.V0 + 0x8u);
                c.V0 = mem.ReadU32(c.V0 + 0x4u);
                c.V0 = c.V0 < c.V1 ? 1u : 0u;
                if (c.V0 != 0u) {
                    goto L8001D700;
                }
                c.A0 = c.S0;
                c.RA = 0x8001D280u;
                KingsField1_game.SetPolyF3_game(c, mem);
                c.V0 = mem.ReadU32(c.S2);
                mem.WriteU32(c.S0 + 0x8u, c.V0);
                c.V0 = mem.ReadU32(c.S3);
                mem.WriteU32(c.S0 + 0xCu, c.V0);
                c.V0 = mem.ReadU32(c.S4);
                c.T0 = 0u | 0x0003u;
                mem.WriteU32(c.S0 + 0x10u, c.V0);
                c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                c.V0 = (uint)(short)mem.ReadU16(c.S3 + 0x6u);
                c.V1 = (uint)(short)mem.ReadU16(c.S4 + 0x6u);
                c.A2 = c.A2 + c.V0;
                c.A2 = c.A2 + c.V1;
                Div(c, c.A2, c.T0); // at = 0x80000000 after
                c.A2 = c.LO;
                c.A1 = c.S1;
                c.A0 = mem.ReadU16(c.S1 + 0x4u);
                c.A3 = c.S0 + 0x4u;
                c.A0 = c.S7 + c.A0;
                c.RA = 0x8001D2F8u;
                KingsField1_game.func_8004DD14(c, mem);
                c.T0 = 0u | 0x0003u;
                c.V0 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                c.V1 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
                c.A0 = (uint)(short)mem.ReadU16(c.S4 + 0x4u);
                c.V0 = c.V0 + c.V1;
                c.V0 = c.V0 + c.A0;
                Div(c, c.V0, c.T0); // at = 0x80000000 after
                c.V0 = c.LO;
                c.V0 = (uint)((int)c.V0 >> 2);
                goto L8001D6B0;
            }
            goto L8001D6D4;

            // G4 semi-transparent (SetPolyG4 then SetSemiTrans), NCLIP-culled; 0x24-byte packet
            L8001D344: ;
            c.V0 = mem.ReadU16(c.S1 + 0x6u);
            c.V1 = mem.ReadU16(c.S1 + 0xEu);
            c.S2 = c.V0 + c.S6;
            c.S4 = c.V1 + c.S6;
            c.V0 = mem.ReadU16(c.S1 + 0xAu);
            c.A0 = mem.ReadU32(c.S2);
            c.A2 = mem.ReadU32(c.S4);
            c.S3 = c.V0 + c.S6;
            c.A1 = mem.ReadU32(c.S3);
            c.RA = 0x8001D370u;
            KingsField1_game.func_8004DE4C(c, mem);
            if ((int)c.V0 > 0) {
                c.V1 = 0x80070000u;
                c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
                c.S0 = mem.ReadU32(c.V1 + 0x8u);
                c.A0 = mem.ReadU16(c.S1 + 0x12u);
                c.V0 = c.S0 + 0x24u;
                mem.WriteU32(c.V1 + 0x8u, c.V0);
                c.V0 = 0x80070000u;
                c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
                c.V1 = mem.ReadU32(c.V0 + 0x8u);
                c.V0 = mem.ReadU32(c.V0 + 0x4u);
                c.V0 = c.V0 < c.V1 ? 1u : 0u;
                bool full = c.V0 != 0u;
                c.S5 = c.A0 + c.S6;
                if (full) {
                    goto L8001D700;
                }
                c.A0 = c.S0;
                c.RA = 0x8001D3C0u;
                KingsField1_game.SetPolyG4_game(c, mem);
                c.A0 = c.S0;
                c.A1 = 0u | 0x0001u;
                c.RA = 0x8001D3CCu;
                KingsField1_game.SetSemiTrans_game(c, mem);
                c.V0 = mem.ReadU32(c.S2);
                mem.WriteU32(c.S0 + 0x8u, c.V0);
                c.V0 = mem.ReadU32(c.S3);
                mem.WriteU32(c.S0 + 0x10u, c.V0);
                c.V0 = mem.ReadU32(c.S4);
                c.A1 = c.S1;
                mem.WriteU32(c.S0 + 0x18u, c.V0);
                c.V0 = mem.ReadU32(c.S5);
                c.A3 = c.S0 + 0x4u;
                mem.WriteU32(c.S0 + 0x20u, c.V0);
                c.A0 = mem.ReadU16(c.S1 + 0x4u);
                c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                c.A0 = c.S7 + c.A0;
                c.RA = 0x8001D40Cu;
                KingsField1_game.func_8004DD14(c, mem);
                c.A1 = c.S1;
                c.A3 = c.S0 + 0xCu;
                c.A0 = mem.ReadU16(c.S1 + 0x8u);
                c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                c.A0 = c.S7 + c.A0;
                c.RA = 0x8001D424u;
                KingsField1_game.func_8004DD14(c, mem);
                c.A1 = c.S1;
                c.A3 = c.S0 + 0x14u;
                c.A0 = mem.ReadU16(c.S1 + 0xCu);
                c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                c.A0 = c.S7 + c.A0;
                c.RA = 0x8001D43Cu;
                KingsField1_game.func_8004DD14(c, mem);
                c.A1 = c.S1;
                c.A3 = c.S0 + 0x1Cu;
                c.A0 = mem.ReadU16(c.S1 + 0x10u);
                c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                c.A0 = c.S7 + c.A0;
                c.RA = 0x8001D454u;
                KingsField1_game.func_8004DD14(c, mem);
                goto L8001D68C;
            }
            goto L8001D6D4;

            // F3 semi-transparent (SetPolyF3 then SetSemiTrans), NCLIP-culled; 0x14-byte packet
            L8001D45C: ;
            c.V0 = mem.ReadU16(c.S1 + 0x6u);
            c.V1 = mem.ReadU16(c.S1 + 0xAu);
            c.S2 = c.V0 + c.S6;
            c.S4 = c.V1 + c.S6;
            c.V0 = mem.ReadU16(c.S1 + 0x8u);
            c.A0 = mem.ReadU32(c.S2);
            c.A2 = mem.ReadU32(c.S4);
            c.S3 = c.V0 + c.S6;
            c.A1 = mem.ReadU32(c.S3);
            c.RA = 0x8001D488u;
            KingsField1_game.func_8004DE4C(c, mem);
            if ((int)c.V0 > 0) {
                c.V1 = 0x80070000u;
                c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
                c.S0 = mem.ReadU32(c.V1 + 0x8u);
                c.V0 = c.S0 + 0x14u;
                mem.WriteU32(c.V1 + 0x8u, c.V0);
                c.V0 = 0x80070000u;
                c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
                c.V1 = mem.ReadU32(c.V0 + 0x8u);
                c.V0 = mem.ReadU32(c.V0 + 0x4u);
                c.V0 = c.V0 < c.V1 ? 1u : 0u;
                if (c.V0 != 0u) {
                    goto L8001D700;
                }
                c.A0 = c.S0;
                c.RA = 0x8001D4D8u;
                KingsField1_game.SetPolyF3_game(c, mem);
                c.A0 = c.S0;
                c.A1 = 0u | 0x0001u;
                c.RA = 0x8001D4E4u;
                KingsField1_game.SetSemiTrans_game(c, mem);
                c.V0 = mem.ReadU32(c.S2);
                mem.WriteU32(c.S0 + 0x8u, c.V0);
                c.V0 = mem.ReadU32(c.S3);
                mem.WriteU32(c.S0 + 0xCu, c.V0);
                c.V0 = mem.ReadU32(c.S4);
                c.T0 = 0u | 0x0003u;
                mem.WriteU32(c.S0 + 0x10u, c.V0);
                c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
                c.V0 = (uint)(short)mem.ReadU16(c.S3 + 0x6u);
                c.V1 = (uint)(short)mem.ReadU16(c.S4 + 0x6u);
                c.A2 = c.A2 + c.V0;
                c.A2 = c.A2 + c.V1;
                Div(c, c.A2, c.T0); // at = 0x80000000 after
                c.A2 = c.LO;
                c.A1 = c.S1;
                c.A0 = mem.ReadU16(c.S1 + 0x4u);
                c.A3 = c.S0 + 0x4u;
                c.A0 = c.S7 + c.A0;
                c.RA = 0x8001D55Cu;
                KingsField1_game.func_8004DD14(c, mem);
                c.T0 = 0u | 0x0003u;
                c.V0 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
                c.V1 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
                c.A0 = (uint)(short)mem.ReadU16(c.S4 + 0x4u);
                c.V0 = c.V0 + c.V1;
                c.V0 = c.V0 + c.A0;
                Div(c, c.V0, c.T0); // at = 0x80000000 after
                c.V0 = c.LO;
                c.V0 = (uint)((int)c.V0 >> 2);
                goto L8001D6B0;
            }
            goto L8001D6D4;

            // F4 semi-transparent (SetPolyF4 then SetSemiTrans), NCLIP-culled; falls into the shared tail
            // F4 semi-transparent (SetPolyF4 then SetSemiTrans), NCLIP-culled; 0x18-byte packet
            L8001D5A8: ;
            c.V0 = mem.ReadU16(c.S1 + 0x6u);
            c.V1 = mem.ReadU16(c.S1 + 0xAu);
            c.S2 = c.V0 + c.S6;
            c.S4 = c.V1 + c.S6;
            c.V0 = mem.ReadU16(c.S1 + 0x8u);
            c.A0 = mem.ReadU32(c.S2);
            c.A2 = mem.ReadU32(c.S4);
            c.S3 = c.V0 + c.S6;
            c.A1 = mem.ReadU32(c.S3);
            c.RA = 0x8001D5D4u;
            KingsField1_game.func_8004DE4C(c, mem);
            if ((int)c.V0 > 0) {
                c.V1 = 0x80070000u;
                c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
                c.S0 = mem.ReadU32(c.V1 + 0x8u);
                c.A0 = mem.ReadU16(c.S1 + 0xCu);
                c.V0 = c.S0 + 0x18u;
                mem.WriteU32(c.V1 + 0x8u, c.V0);
                c.V0 = 0x80070000u;
                c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
                c.V1 = mem.ReadU32(c.V0 + 0x8u);
                c.V0 = mem.ReadU32(c.V0 + 0x4u);
                c.V0 = c.V0 < c.V1 ? 1u : 0u;
                bool full = c.V0 != 0u;
                c.S5 = c.A0 + c.S6;
                if (full) {
                    goto L8001D700;
                }
                c.A0 = c.S0;
                c.RA = 0x8001D624u;
                KingsField1_game.SetPolyF4_game(c, mem);
                c.A0 = c.S0;
                c.A1 = 0u | 0x0001u;
                c.RA = 0x8001D630u;
                KingsField1_game.SetSemiTrans_game(c, mem);
                goto L8001D630;
            }
            goto L8001D6D4;

            // quad tail: the four corners go into the packet (the F4 paths enter here; FT4 enters at L8001D664)
            L8001D630: ;
            c.V0 = mem.ReadU32(c.S2);
            mem.WriteU32(c.S0 + 0x8u, c.V0);
            c.V0 = mem.ReadU32(c.S3);
            mem.WriteU32(c.S0 + 0xCu, c.V0);
            c.V0 = mem.ReadU32(c.S4);
            c.A1 = c.S1;
            mem.WriteU32(c.S0 + 0x10u, c.V0);
            c.V0 = mem.ReadU32(c.S5);
            c.A3 = c.S0 + 0x4u;
            mem.WriteU32(c.S0 + 0x14u, c.V0);
            c.A0 = mem.ReadU16(c.S1 + 0x4u);
            L8001D664: ;
            c.A2 = (uint)(short)mem.ReadU16(c.S2 + 0x6u);
            c.V0 = (uint)(short)mem.ReadU16(c.S3 + 0x6u);
            c.V1 = (uint)(short)mem.ReadU16(c.S5 + 0x6u);
            c.A2 = c.A2 + c.V0;
            c.V0 = (uint)(short)mem.ReadU16(c.S4 + 0x6u);
            c.A0 = c.S7 + c.A0;
            c.A2 = c.A2 + c.V0;
            c.A2 = c.A2 + c.V1;
            c.A2 = (uint)((int)c.A2 >> 2);
            c.RA = 0x8001D68Cu;
            KingsField1_game.func_8004DD14(c, mem);
            // quad depth: the four corner z values, summed and >> 4 (the mean, divided by 4)
            L8001D68C: ;
            c.V0 = (uint)(short)mem.ReadU16(c.S2 + 0x4u);
            c.V1 = (uint)(short)mem.ReadU16(c.S3 + 0x4u);
            c.V0 = c.V0 + c.V1;
            c.V1 = (uint)(short)mem.ReadU16(c.S4 + 0x4u);
            c.A0 = (uint)(short)mem.ReadU16(c.S5 + 0x4u);
            c.V0 = c.V0 + c.V1;
            c.V0 = c.V0 + c.A0;
            c.V0 = (uint)((int)c.V0 >> 4);
            // depth + FP (the offset) selects the ordering-table slot; slots below 5 are dropped
            L8001D6B0: ;
            c.A0 = c.V0 + c.FP;
            c.V0 = (int)c.A0 < 5 ? 1u : 0u;
            bool dropped = c.V0 != 0u;
            c.A1 = c.S0;
            if (dropped) {
                goto L8001D6D4;
            }
            c.A0 = c.A0 & 0x3FFFu;
            c.V0 = mem.ReadU32(c.S6 - 0x2F4u);
            c.A0 = c.A0 << 2;
            c.A0 = c.A0 + c.V0;
            if (PacketDepth.On) Depth(mem, c.A1, c.S2, c.S3, c.S4, c.S5);
            c.RA = 0x8001D6D4u;
            KingsField1_game.AddPrim_game(c, mem);
            // next primitive: S1 advances by 4 x byte 1 of this primitive's first word
            L8001D6D4: ;
            c.T0 = mem.ReadU32(c.SP + 0x20u);
            c.V0 = c.T0 >> 6;
            c.V0 = c.V0 & 0x03FCu;
            c.S1 = c.S1 + c.V0;
            c.V0 = mem.ReadU32(c.SP + 0x28u);
            c.T0 = c.V0;
            c.T0 = c.T0 - 0x1u;
            bool more = c.V0 != 0u;
            mem.WriteU32(c.SP + 0x28u, c.T0);
            if (more) {
                continue;
            }
            break;
        }

        // epilogue: restore the saved registers and return
        L8001D700: ;
        c.RA = mem.ReadU32(c.SP + 0x54u);
        c.FP = mem.ReadU32(c.SP + 0x50u);
        c.S7 = mem.ReadU32(c.SP + 0x4Cu);
        c.S6 = mem.ReadU32(c.SP + 0x48u);
        c.S5 = mem.ReadU32(c.SP + 0x44u);
        c.S4 = mem.ReadU32(c.SP + 0x40u);
        c.S3 = mem.ReadU32(c.SP + 0x3Cu);
        c.S2 = mem.ReadU32(c.SP + 0x38u);
        c.S1 = mem.ReadU32(c.SP + 0x34u);
        c.S0 = mem.ReadU32(c.SP + 0x30u);
        c.SP = c.SP + 0x58u;
        return;
    }
}
