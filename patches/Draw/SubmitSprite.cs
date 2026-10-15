using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001ED90(a0 = sprite record)</c>, the flipbook sprites, transcribed literally.
/// Builds the rotation and translation matrices from 0x800956A0, takes the record's
/// x, y, z (+4, +8, +C) less the origin words at 0x80095744..0x8009574C into a short
/// vector on the stack, and draws it through <c>func_8001E230</c> with the frame
/// picked from the record's +0x14 counter, which then advances and wraps at the
/// low nibble of +2. <c>KF1_SUBMIT_SPRITE</c>.
/// </summary>
public static class SubmitSprite
{
    public static readonly Literal R = new(typeof(SubmitSprite), "flipbook sprites", "SUBMIT_SPRITE", 0x8001ED90, Run,
        "Draws the counted sprite list at 0x80095098.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x50u;
        mem.WriteU32(c.SP + 0x4Cu, c.RA);
        mem.WriteU32(c.SP + 0x48u, c.S2);
        mem.WriteU32(c.SP + 0x44u, c.S1);
        c.S1 = c.A0;
        c.A0 = 0x80090000u;
        c.A0 = c.A0 + 0x56A0u;
        mem.WriteU32(c.SP + 0x40u, c.S0);
        c.RA = 0x8001EDB4u;
        KingsField1_game.SetRotMatrix_game(c, mem);
        c.A0 = 0x80090000u;
        c.A0 = c.A0 + 0x56A0u;
        c.RA = 0x8001EDC4u;
        KingsField1_game.SetTransMatrix_game(c, mem);
        c.S2 = 0x80090000u;
        c.S2 = c.S2 + 0x5744u;

        // Record x, y, z (+4, +8, +C) less the origin at 0x80095744, 0x80095748, 0x8009574C.
        c.V0 = mem.ReadU16(c.S1 + 0x4u);
        c.V1 = mem.ReadU16(c.S2);
        c.A0 = c.SP + 0x10u;
        c.V0 = c.V0 - c.V1;
        mem.WriteU16(c.SP + 0x10u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x8u);
        c.V1 = 0x80090000u;
        c.V1 = mem.ReadU16(c.V1 + 0x5748u);
        c.A1 = c.SP + 0x2Cu;
        c.V0 = c.V0 - c.V1;
        mem.WriteU16(c.SP + 0x12u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0xCu);
        c.V1 = 0x80090000u;
        c.V1 = mem.ReadU16(c.V1 + 0x574Cu);
        c.A2 = c.SP + 0x38u;
        c.V0 = c.V0 - c.V1;
        mem.WriteU16(c.SP + 0x14u, (ushort)c.V0);
        c.RA = 0x8001EE14u;
        KingsField1_game.func_8004DADC(c, mem);

        // High nibble of record +2 selects the path: nonzero uses the entry at
        // (nibble - 0x10) << 6 and draws with S0 = 0x96; zero uses S2 - 0x84 and 0xC8.
        c.V0 = mem.ReadU8(c.S1 + 0x2u);
        c.V0 = c.V0 & 0x00F0u;
        c.V0 = c.V0 & 0x00FFu;
        bool none = c.V0 == 0u;
        c.S0 = c.SP + 0x18u;
        if (!none) {
            c.A0 = c.V0 - 0x10u;
            c.A0 = c.A0 << 6;
            c.A1 = c.S0;
            c.RA = 0x8001EE3Cu;
            KingsField1_game.func_80014BEC(c, mem);
            c.A0 = c.S2 - 0xA4u;
            c.A1 = c.S0;
            c.RA = 0x8001EE48u;
            KingsField1_game.func_8004D11C(c, mem);
            c.A0 = c.S0;
            c.RA = 0x8001EE50u;
            KingsField1_game.SetRotMatrix_game(c, mem);
            c.S0 = 0u | 0x0096u;
        } else {
            c.A0 = c.S2 - 0x84u;
            c.RA = 0x8001EE60u;
            KingsField1_game.SetRotMatrix_game(c, mem);
            c.S0 = 0u | 0x00C8u;
        }
        c.A0 = c.SP + 0x18u;
        c.RA = 0x8001EE6Cu;
        KingsField1_game.SetTransMatrix_game(c, mem);
        c.A1 = c.S0;
        c.V0 = mem.ReadU16(c.S1);
        c.V1 = mem.ReadU8(c.S1 + 0x14u);
        c.A2 = 0u | 0x0001u;
        c.V0 = c.V0 + c.V1;
        c.A0 = c.V0 << 1;
        c.A0 = c.A0 + c.V0;
        c.A0 = c.A0 << 2;
        c.V0 = 0x80050000u;
        c.V0 = c.V0 + 0x5B00u;
        c.A0 = c.A0 + c.V0;
        c.RA = 0x8001EE9Cu;
        KingsField1_game.func_8001E230(c, mem);

        // Advance the frame counter (+0x14); it resets to 0 once it reaches the low nibble of +2.
        c.V0 = mem.ReadU8(c.S1 + 0x14u);
        c.V1 = mem.ReadU8(c.S1 + 0x2u);
        c.V0 = c.V0 + 0x1u;
        mem.WriteU8(c.S1 + 0x14u, (byte)c.V0);
        c.V0 = c.V0 & 0x00FFu;
        c.V1 = c.V1 & 0x000Fu;
        c.V0 = c.V0 < c.V1 ? 1u : 0u;
        if (c.V0 == 0u) {
            mem.WriteU8(c.S1 + 0x14u, (byte)0u);
        }
        c.RA = mem.ReadU32(c.SP + 0x4Cu);
        c.S2 = mem.ReadU32(c.SP + 0x48u);
        c.S1 = mem.ReadU32(c.SP + 0x44u);
        c.S0 = mem.ReadU32(c.SP + 0x40u);
        c.SP = c.SP + 0x50u;
        return;
    }
}
