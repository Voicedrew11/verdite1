using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001E9A4</c>, the objects, transcribed literally. <c>KF1_SUBMIT_OBJECT</c>.
/// Argument a0 is the object record (72 bytes), kept in S2. The routine sets the
/// camera matrices from 0x800956A0, positions the record's three shorts (at +0x1C,
/// +0x20, +0x24) less the origin at 0x80095744 into the frame at SP+0x18, runs the
/// TMD code on the record's object (<c>func_8001C114</c>, <c>func_8001C60C</c>,
/// <c>func_8001C7F8</c>), and reads the record's flags byte from the table at
/// 0x80070000 - 0x4267 indexed by its type.
/// </summary>
public static class SubmitObject
{
    public static readonly Literal R = new(typeof(SubmitObject), "objects", "SUBMIT_OBJECT", 0x8001E9A4, Run,
        "Draws the object table (72-byte records at 0x8006C4B8).");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0xA8u;
        mem.WriteU32(c.SP + 0xA0u, c.RA);
        mem.WriteU32(c.SP + 0x9Cu, c.S3);
        mem.WriteU32(c.SP + 0x98u, c.S2);
        mem.WriteU32(c.SP + 0x94u, c.S1);
        c.S2 = c.A0;
        c.A0 = 0x80090000u;
        c.A0 = c.A0 + 0x56A0u;
        mem.WriteU32(c.SP + 0x90u, c.S0);

        // Camera: rotation and translation matrices from 0x800956A0.
        c.RA = 0x8001E9CCu;
        KingsField1_game.SetRotMatrix_game(c, mem);
        c.A0 = 0x80090000u;
        c.A0 = c.A0 + 0x56A0u;
        c.RA = 0x8001E9DCu;
        KingsField1_game.SetTransMatrix_game(c, mem);

        // Record position less the origin at 0x80095744, three shorts at SP+0x18.
        c.S3 = 0x80090000u;
        c.S3 = c.S3 + 0x5744u;
        c.V0 = mem.ReadU16(c.S2 + 0x1Cu);
        c.V1 = mem.ReadU16(c.S3);
        c.A0 = c.SP + 0x18u;
        c.V0 = c.V0 - c.V1;
        mem.WriteU16(c.SP + 0x18u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S2 + 0x20u);
        c.V1 = 0x80090000u;
        c.V1 = mem.ReadU16(c.V1 + 0x5748u);
        c.A1 = c.SP + 0x54u;
        c.V0 = c.V0 - c.V1;
        mem.WriteU16(c.SP + 0x1Au, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S2 + 0x24u);
        c.V1 = 0x80090000u;
        c.V1 = mem.ReadU16(c.V1 + 0x574Cu);
        c.A2 = c.SP + 0x80u;
        c.V0 = c.V0 - c.V1;
        mem.WriteU16(c.SP + 0x1Cu, (ushort)c.V0);
        c.RA = 0x8001EA2Cu;
        KingsField1_game.func_8004DADC(c, mem);

        c.S1 = c.SP + 0x40u;
        c.A0 = (uint)(short)mem.ReadU16(c.S2 + 0x2Cu);
        c.A1 = c.S1;
        c.RA = 0x8001EA3Cu;
        KingsField1_game.func_80014B7C(c, mem);
        c.S0 = c.SP + 0x20u;
        c.A0 = (uint)(short)mem.ReadU16(c.S2 + 0x2Eu);
        c.A1 = c.S0;
        c.A0 = 0u - c.A0;
        c.RA = 0x8001EA50u;
        KingsField1_game.func_80014BEC(c, mem);
        c.A0 = c.S0;
        c.A1 = c.S1;
        c.RA = 0x8001EA5Cu;
        KingsField1_game.func_8004D11C(c, mem);
        c.A0 = 0x80050000u;
        c.A0 = c.A0 + 0x5F68u;
        c.A1 = c.S1;
        c.S0 = c.SP + 0x60u;
        c.A2 = c.S0;
        c.RA = 0x8001EA74u;
        KingsField1_game.func_8004CDD4(c, mem);
        c.A0 = c.S3 - 0xA4u;
        c.A1 = c.S1;
        c.RA = 0x8001EA80u;
        KingsField1_game.func_8004D11C(c, mem);
        c.A0 = c.S1;
        c.RA = 0x8001EA88u;
        KingsField1_game.SetRotMatrix_game(c, mem);
        c.A0 = c.S1;
        c.RA = 0x8001EA90u;
        KingsField1_game.SetTransMatrix_game(c, mem);
        c.A0 = c.S0;
        c.RA = 0x8001EA98u;
        KingsField1_game.SetLightMatrix_game(c, mem);

        // Flags byte of the record's type, from the table at 0x80070000 - 0x4267.
        c.V1 = mem.ReadU8(c.S2 + 0x1u);
        c.V0 = c.V1 << 2;
        c.V0 = c.V0 + c.V1;
        c.V0 = c.V0 << 2;
        c.V0 = c.V0 - c.V1;
        c.V0 = c.V0 << 3;
        c.At = 0x80070000u;
        c.At = c.At - 0x4267u;
        c.At = c.At + c.V0;
        c.S1 = mem.ReadU8(c.At);
        c.S0 = c.S1 & 0x000Fu;
        c.A0 = c.S0;
        c.RA = 0x8001EAD4u;
        KingsField1_game.func_8002059C(c, mem);
        c.A0 = 0u;
        c.RA = 0x8001EADCu;
        KingsField1_game.func_8001C114(c, mem);
        c.A0 = c.S2 + 0x34u;
        c.S3 = c.V0;
        c.A2 = mem.ReadU8(c.S2 + 0xAu);
        c.A3 = mem.ReadU16(c.S2 + 0x12u);
        c.V0 = mem.ReadU32(c.S3 + 0x4u);
        c.A1 = c.S0;
        mem.WriteU32(c.SP + 0x10u, c.V0);
        c.RA = 0x8001EAFCu;
        KingsField1_game.func_800205D4(c, mem);

        // func_800205D4's result (V0) picks which record's vertex list is cached.
        if (c.V0 == 0u) {
            c.A0 = 0u;
            c.RA = 0x8001EB0Cu;
            KingsField1_game.func_8001C148(c, mem);
            c.A0 = 0u;
            c.RA = 0x8001EB14u;
            KingsField1_game.func_8001C114(c, mem);
            c.A0 = mem.ReadU32(c.V0 + 0x4u);
            c.RA = 0x8001EB20u;
            KingsField1_game.func_8001C60C(c, mem);
            c.V0 = c.S1 & 0x00FFu;
        } else {
            c.A0 = mem.ReadU32(c.S3 + 0x4u);
            c.RA = 0x8001EB34u;
            KingsField1_game.func_8001C60C(c, mem);
            c.V0 = c.S1 & 0x00FFu;
        }

        // High nibble of the flags byte: zero draws one polygon set, nonzero draws
        // a table-driven set (halfwords at 0x80095038, indexed by S1).
        c.V0 = c.V0 >> 4;
        bool upper = c.V0 != 0u;
        c.S1 = c.V0 - 0x1u;
        if (!upper) {
            c.A0 = 0u;
            c.A1 = 0u;
            c.RA = 0x8001EB50u;
            KingsField1_game.func_8001C7F8(c, mem);
        } else {
            c.A0 = 0u;
            c.V1 = c.S1 & 0x00FFu;
            c.V1 = c.V1 << 1;
            c.At = 0x80090000u;
            c.At = c.At + 0x5038u;
            c.At = c.At + c.V1;
            c.A1 = mem.ReadU16(c.At);
            c.V0 = 0x80090000u;
            c.V0 = c.V0 + 0x505Au;
            mem.WriteU16(c.V0, (ushort)c.A1);
            c.V0 = c.V0 + c.V1;
            c.V0 = mem.ReadU16(c.V0 - 0x12u);
            c.At = 0x80090000u;
            mem.WriteU16(c.At + 0x5058u, (ushort)c.V0);
            c.A1 = 0u;
            c.RA = 0x8001EB9Cu;
            KingsField1_game.func_8001D730(c, mem);
        }

        c.RA = mem.ReadU32(c.SP + 0xA0u);
        c.S3 = mem.ReadU32(c.SP + 0x9Cu);
        c.S2 = mem.ReadU32(c.SP + 0x98u);
        c.S1 = mem.ReadU32(c.SP + 0x94u);
        c.S0 = mem.ReadU32(c.SP + 0x90u);
        c.SP = c.SP + 0xA8u;
        return;
    }
}
