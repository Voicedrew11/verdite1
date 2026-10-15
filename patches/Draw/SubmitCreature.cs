using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001EEDC(a0 = creature record)</c>, the creatures, transcribed literally.
/// Skipped when the record's byte +3 is 0xFF. Otherwise it builds the matrices from
/// 0x800956A0 and the record's position and scale, then draws the model through
/// <c>func_8001E230</c> (byte +4 equal to 0xFF) or through the object selected by
/// <c>func_8001C114</c>, projecting its vertices with <c>func_8001C60C</c> and writing
/// the polygons with <c>func_8001C7F8</c>. <c>KF1_SUBMIT_CREATURE</c>.
/// </summary>
public static class SubmitCreature
{
    public static readonly Literal R = new(typeof(SubmitCreature), "creatures", "SUBMIT_CREATURE", 0x8001EEDC, Run,
        "Draws the creature table (60-byte records at 0x8009D040).");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x70u;
        mem.WriteU32(c.SP + 0x68u, c.RA);
        mem.WriteU32(c.SP + 0x64u, c.S3);
        mem.WriteU32(c.SP + 0x60u, c.S2);
        mem.WriteU32(c.SP + 0x5Cu, c.S1);
        mem.WriteU32(c.SP + 0x58u, c.S0);
        c.S1 = c.A0;
        c.V0 = mem.ReadU8(c.S1 + 0x3u);
        c.S3 = 0u | 0x00FFu;
        if (c.V0 != c.S3) {
            c.A0 = 0x80090000u;
            c.A0 = c.A0 + 0x56A0u;
            c.RA = 0x8001EF18u;
            KingsField1_game.SetRotMatrix_game(c, mem);
            c.A0 = 0x80090000u;
            c.A0 = c.A0 + 0x56A0u;
            c.RA = 0x8001EF28u;
            KingsField1_game.SetTransMatrix_game(c, mem);
            c.S2 = 0x80090000u;
            c.S2 = c.S2 + 0x5744u;

            // Record x, y, z (+0xC, +0x10, +0x14) less the origin words at 0x80095744..0x8009574C.
            c.V0 = mem.ReadU16(c.S1 + 0xCu);
            c.V1 = mem.ReadU16(c.S2);
            c.A0 = c.SP + 0x18u;
            c.V0 = c.V0 - c.V1;
            mem.WriteU16(c.SP + 0x18u, (ushort)c.V0);
            c.V0 = mem.ReadU16(c.S1 + 0x10u);
            c.V1 = 0x80090000u;
            c.V1 = mem.ReadU16(c.V1 + 0x5748u);
            c.A1 = c.SP + 0x44u;
            c.V0 = c.V0 - c.V1;
            mem.WriteU16(c.SP + 0x1Au, (ushort)c.V0);
            c.V0 = mem.ReadU16(c.S1 + 0x14u);
            c.V1 = 0x80090000u;
            c.V1 = mem.ReadU16(c.V1 + 0x574Cu);
            c.A2 = c.SP + 0x50u;
            c.V0 = c.V0 - c.V1;
            mem.WriteU16(c.SP + 0x1Cu, (ushort)c.V0);
            c.RA = 0x8001EF78u;
            KingsField1_game.func_8004DADC(c, mem);
            c.A0 = c.S1 + 0x1Cu;
            c.S0 = c.SP + 0x30u;
            c.A1 = c.S0;
            c.RA = 0x8001EF88u;
            KingsField1_game.func_80014CCC(c, mem);

            // Scale from the signed halfwords at +0x24, +0x26, +0x28.
            c.V0 = (uint)(short)mem.ReadU16(c.S1 + 0x24u);
            mem.WriteU32(c.SP + 0x20u, c.V0);
            c.V0 = (uint)(short)mem.ReadU16(c.S1 + 0x26u);
            c.A0 = c.S0;
            mem.WriteU32(c.SP + 0x24u, c.V0);
            c.V0 = (uint)(short)mem.ReadU16(c.S1 + 0x28u);
            c.A1 = c.SP + 0x20u;
            mem.WriteU32(c.SP + 0x28u, c.V0);
            c.RA = 0x8001EFB0u;
            KingsField1_game.ScaleMatrix_game(c, mem);

            c.V0 = mem.ReadU8(c.S1 + 0x4u);
            bool plain = c.V0 == c.S3;
            c.A0 = c.S2 - 0xA4u;
            if (plain) {
                c.A0 = c.S2 - 0x84u;
                c.A1 = c.S0;
                c.RA = 0x8001EFCCu;
                KingsField1_game.func_8004D11C(c, mem);
                c.A0 = c.S0;
                c.RA = 0x8001EFD4u;
                KingsField1_game.SetRotMatrix_game(c, mem);
                c.A0 = c.S0;
                c.RA = 0x8001EFDCu;
                KingsField1_game.SetTransMatrix_game(c, mem);
                c.A1 = 0u;
                c.V0 = mem.ReadU8(c.S1 + 0x3u);
                c.A2 = 0u;
                c.A0 = c.V0 << 1;
                c.A0 = c.A0 + c.V0;
                c.A0 = c.A0 << 2;
                c.V0 = 0x80050000u;
                c.V0 = c.V0 + 0x5B54u;
                c.A0 = c.A0 + c.V0;
                c.RA = 0x8001F004u;
                KingsField1_game.func_8001E230(c, mem);
            } else {
                c.A1 = c.S0;
                c.RA = 0x8001F014u;
                KingsField1_game.func_8004D11C(c, mem);
                c.A0 = c.S0;
                c.RA = 0x8001F01Cu;
                KingsField1_game.SetRotMatrix_game(c, mem);
                c.A0 = c.S0;
                c.RA = 0x8001F024u;
                KingsField1_game.SetTransMatrix_game(c, mem);
                c.S0 = mem.ReadU8(c.S1 + 0x3u);
                c.S0 = c.S0 + 0x1Eu;
                c.S0 = c.S0 & 0xFFFFu;
                c.A0 = c.S0;
                c.RA = 0x8001F03Cu;
                KingsField1_game.func_8002059C(c, mem);
                c.A0 = 0u;
                c.RA = 0x8001F044u;
                KingsField1_game.func_8001C114(c, mem);
                c.A0 = c.S1 + 0x34u;
                c.S2 = c.V0;
                c.A2 = mem.ReadU8(c.S1 + 0x4u);
                c.A3 = mem.ReadU16(c.S1 + 0x8u);
                c.V0 = mem.ReadU32(c.S2 + 0x4u);
                c.A1 = c.S0;
                mem.WriteU32(c.SP + 0x10u, c.V0);
                c.RA = 0x8001F064u;
                KingsField1_game.func_800205D4(c, mem);
                if (c.V0 == 0u) {
                    c.A0 = 0u;
                    c.RA = 0x8001F074u;
                    KingsField1_game.func_8001C148(c, mem);
                    c.A0 = 0u;
                    c.RA = 0x8001F07Cu;
                    KingsField1_game.func_8001C114(c, mem);
                    c.A0 = mem.ReadU32(c.V0 + 0x4u);
                    c.RA = 0x8001F088u;
                    KingsField1_game.func_8001C60C(c, mem);
                    c.A0 = 0u;
                } else {
                    c.A0 = mem.ReadU32(c.S2 + 0x4u);
                    c.RA = 0x8001F09Cu;
                    KingsField1_game.func_8001C60C(c, mem);
                    c.A0 = 0u;
                }
                c.A1 = 0u | 0x0064u;
                c.RA = 0x8001F0A8u;
                KingsField1_game.func_8001C7F8(c, mem);
            }
        }
        c.RA = mem.ReadU32(c.SP + 0x68u);
        c.S3 = mem.ReadU32(c.SP + 0x64u);
        c.S2 = mem.ReadU32(c.SP + 0x60u);
        c.S1 = mem.ReadU32(c.SP + 0x5Cu);
        c.S0 = mem.ReadU32(c.SP + 0x58u);
        c.SP = c.SP + 0x70u;
        return;
    }
}
