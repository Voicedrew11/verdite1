using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001E230</c>, the sprite writer. <c>KF1_SPRITEASSEMBLER</c>.
/// a0 is the sprite record (its shorts at +4, +6, +8, +A give the position and
/// size), a1 a signed halfword added to the ordering-table index, and a2 == 1
/// scales the size by 3/2. The FT4 packet is written at the cursor of the record
/// at 0x80070EB8 and added to the ordering table at 0x80090EBC.
/// </summary>
public static class SpriteAssembler
{
    public static readonly Literal R = new(typeof(SpriteAssembler), "sprite writer", "SPRITEASSEMBLER", 0x8001E230, Run,
        "Writes one projected FT4 sprite: flipbooks, creatures, the HUD models.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x88u;
        mem.WriteU32(c.SP + 0x84u, c.RA);
        mem.WriteU32(c.SP + 0x80u, c.S4);
        mem.WriteU32(c.SP + 0x7Cu, c.S3);
        mem.WriteU32(c.SP + 0x78u, c.S2);
        mem.WriteU32(c.SP + 0x74u, c.S1);
        mem.WriteU32(c.SP + 0x70u, c.S0);
        c.S2 = c.A0;
        c.S4 = c.A1;
        c.S3 = c.A2;
        c.A0 = c.SP + 0x48u;
        c.A1 = c.SP + 0x50u;
        c.V0 = mem.ReadU16(c.S2 + 0x4u);
        c.S1 = c.SP + 0x54u;
        mem.WriteU16(c.SP + 0x38u, (ushort)c.V0);
        mem.WriteU16(c.SP + 0x28u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S2 + 0x4u);
        c.V1 = mem.ReadU16(c.S2 + 0x8u);
        c.A2 = c.S1;
        c.V0 = c.V0 + c.V1;
        mem.WriteU16(c.SP + 0x40u, (ushort)c.V0);
        mem.WriteU16(c.SP + 0x30u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S2 + 0x6u);
        c.S0 = c.SP + 0x58u;
        mem.WriteU16(c.SP + 0x32u, (ushort)c.V0);
        mem.WriteU16(c.SP + 0x2Au, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S2 + 0x6u);
        c.V1 = mem.ReadU16(c.S2 + 0xAu);
        c.A3 = c.S0;
        mem.WriteU16(c.SP + 0x44u, (ushort)0u);
        mem.WriteU16(c.SP + 0x3Cu, (ushort)0u);
        mem.WriteU16(c.SP + 0x34u, (ushort)0u);
        mem.WriteU16(c.SP + 0x2Cu, (ushort)0u);
        mem.WriteU16(c.SP + 0x4Cu, (ushort)0u);
        mem.WriteU16(c.SP + 0x4Au, (ushort)0u);
        mem.WriteU16(c.SP + 0x48u, (ushort)0u);
        c.V0 = c.V0 + c.V1;
        mem.WriteU16(c.SP + 0x42u, (ushort)c.V0);
        mem.WriteU16(c.SP + 0x3Au, (ushort)c.V0);
        c.RA = 0x8001E2D0u;
        KingsField1_game.func_8004DA4C(c, mem);
        c.V1 = c.SP + 0x5Cu;
        mem.WriteU32(c.SP + 0x10u, c.V1);
        c.V1 = c.SP + 0x60u;
        mem.WriteU32(c.SP + 0x14u, c.V1);
        c.V1 = c.SP + 0x64u;
        mem.WriteU32(c.SP + 0x18u, c.V1);
        c.V1 = c.SP + 0x68u;
        mem.WriteU32(c.SP + 0x1Cu, c.V1);
        mem.WriteU32(c.SP + 0x20u, c.S1);
        mem.WriteU32(c.SP + 0x24u, c.S0);
        c.A0 = c.SP + 0x28u;
        c.A1 = c.SP + 0x30u;
        c.A2 = c.SP + 0x38u;
        c.A3 = c.SP + 0x40u;
        c.S1 = c.V0;
        c.RA = 0x8001E310u;
        KingsField1_game.func_8004E0B4(c, mem);
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
        // out of buffer space: the sprite is not written and not added
        bool outOfSpace = c.V0 != 0u;
        if (!outOfSpace)
        {
            c.A0 = c.S0;
            c.RA = 0x8001E358u;
            KingsField1_game.SetPolyFT4_game(c, mem);
            c.A1 = 0x80090000u;
            c.A1 = c.A1 + 0x5058u;
            c.V0 = mem.ReadU16(c.A1);
            mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
            c.V0 = mem.ReadU32(c.SP + 0x5Cu);
            c.V1 = 0x80090000u;
            c.V1 = mem.ReadU16(c.V1 + 0x505Au);
            mem.WriteU32(c.S0 + 0x8u, c.V0);
            c.V0 = mem.ReadU32(c.SP + 0x60u);
            mem.WriteU32(c.S0 + 0x10u, c.V0);
            c.V0 = mem.ReadU32(c.SP + 0x64u);
            mem.WriteU32(c.S0 + 0x18u, c.V0);
            c.V0 = mem.ReadU32(c.SP + 0x68u);
            mem.WriteU16(c.S0 + 0x16u, (ushort)c.V1);
            mem.WriteU32(c.S0 + 0x20u, c.V0);
            c.V0 = mem.ReadU8(c.S2);
            mem.WriteU8(c.S0 + 0x1Cu, (byte)c.V0);
            mem.WriteU8(c.S0 + 0xCu, (byte)c.V0);
            c.V0 = mem.ReadU8(c.S2);
            c.V1 = mem.ReadU8(c.S2 + 0x2u);
            c.V0 = c.V0 + c.V1;
            mem.WriteU8(c.S0 + 0x24u, (byte)c.V0);
            mem.WriteU8(c.S0 + 0x14u, (byte)c.V0);
            c.V0 = mem.ReadU8(c.S2 + 0x1u);
            mem.WriteU8(c.S0 + 0x15u, (byte)c.V0);
            mem.WriteU8(c.S0 + 0xDu, (byte)c.V0);
            c.V0 = mem.ReadU8(c.S2 + 0x1u);
            c.V1 = mem.ReadU8(c.S2 + 0x3u);
            c.V0 = c.V0 + c.V1;
            mem.WriteU8(c.S0 + 0x25u, (byte)c.V0);
            mem.WriteU8(c.S0 + 0x1Du, (byte)c.V0);
            c.V1 = mem.ReadU8(c.S0 + 0x7u);
            c.V0 = 0u | 0x0001u;
            c.At = 0x80090000u;
            mem.WriteU8(c.At + 0x505Fu, (byte)c.V1);
            // a2 == 1: the size at SP+0x54 is scaled by 3/2
            bool notHalf = c.S3 != c.V0;
            c.A1 = c.A1 + 0x4u;
            if (!notHalf)
            {
                c.V1 = mem.ReadU32(c.SP + 0x54u);
                c.V0 = (uint)((int)c.V1 >> 1);
                c.V0 = c.V0 + c.V1;
                mem.WriteU32(c.SP + 0x54u, c.V0);
            }
            c.A0 = 0x80050000u;
            c.A0 = c.A0 + 0x7B64u;
            c.A2 = mem.ReadU32(c.SP + 0x54u);
            c.A3 = c.S0 + 0x4u;
            c.RA = 0x8001E430u;
            KingsField1_game.func_8004DD14(c, mem);
            c.V0 = c.S4 << 16;
            c.V0 = (uint)((int)c.V0 >> 16);
            c.A0 = c.S1 + c.V0;
            // the ordering-table index must be inside the table (< 0x4000, and at least 5)
            c.V0 = (int)c.A0 < 5 ? 1u : 0u;
            bool badSlot = c.V0 != 0u;
            c.A1 = c.S0;
            if (!badSlot)
            {
                c.A0 = c.A0 & 0x3FFFu;
                c.V0 = 0x80090000u;
                c.V0 = mem.ReadU32(c.V0 + 0xEBCu);
                c.A0 = c.A0 << 2;
                c.A0 = c.A0 + c.V0;
                c.RA = 0x8001E460u;
                KingsField1_game.AddPrim_game(c, mem);
            }
        }
        c.RA = mem.ReadU32(c.SP + 0x84u);
        c.S4 = mem.ReadU32(c.SP + 0x80u);
        c.S3 = mem.ReadU32(c.SP + 0x7Cu);
        c.S2 = mem.ReadU32(c.SP + 0x78u);
        c.S1 = mem.ReadU32(c.SP + 0x74u);
        c.S0 = mem.ReadU32(c.SP + 0x70u);
        c.SP = c.SP + 0x88u;
        return;
    }
}
