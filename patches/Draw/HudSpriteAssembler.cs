using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001E480</c>, the HUD sprite writer, transcribed literally. <c>KF1_HUDSPRITEASSEMBLER</c>.
/// </summary>
public static class HudSpriteAssembler
{
    public static readonly Literal R = new(typeof(HudSpriteAssembler), "HUD sprite writer", "HUDSPRITEASSEMBLER", 0x8001E480, Run,
        "Writes one screen-space FT4 HUD sprite at slot 1.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.V1 = 0x80070000u;
        c.V1 = mem.ReadU32(c.V1 + 0xEB8u);
        c.SP = c.SP - 0x20u;
        mem.WriteU32(c.SP + 0x18u, c.RA);
        mem.WriteU32(c.SP + 0x14u, c.S1);
        mem.WriteU32(c.SP + 0x10u, c.S0);
        c.S0 = mem.ReadU32(c.V1 + 0x8u);
        c.V0 = c.S0 + 0x28u;
        mem.WriteU32(c.V1 + 0x8u, c.V0);
        c.V0 = 0x80070000u;
        c.V0 = mem.ReadU32(c.V0 + 0xEB8u);
        c.V1 = mem.ReadU32(c.V0 + 0x8u);
        c.V0 = mem.ReadU32(c.V0 + 0x4u);
        c.V0 = c.V0 < c.V1 ? 1u : 0u;
        if (c.V0 != 0u) {
            c.S1 = c.A0;
            goto L8001E5D8;
        }
        c.S1 = c.A0;
        c.A0 = c.S0;
        c.RA = 0x8001E4D4u;
        KingsField1_game.SetPolyFT4_game(c, mem);
        c.V0 = mem.ReadU16(c.S1 + 0x4u);
        mem.WriteU16(c.S0 + 0x18u, (ushort)c.V0);
        mem.WriteU16(c.S0 + 0x8u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x4u);
        c.V1 = mem.ReadU16(c.S1 + 0x8u);
        c.V0 = c.V0 + c.V1;
        mem.WriteU16(c.S0 + 0x20u, (ushort)c.V0);
        mem.WriteU16(c.S0 + 0x10u, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x6u);
        mem.WriteU16(c.S0 + 0x12u, (ushort)c.V0);
        mem.WriteU16(c.S0 + 0xAu, (ushort)c.V0);
        c.V0 = mem.ReadU16(c.S1 + 0x6u);
        c.V1 = mem.ReadU16(c.S1 + 0xAu);
        c.V0 = c.V0 + c.V1;
        mem.WriteU16(c.S0 + 0x22u, (ushort)c.V0);
        mem.WriteU16(c.S0 + 0x1Au, (ushort)c.V0);
        c.V0 = 0x80090000u;
        c.V0 = mem.ReadU16(c.V0 + 0x5058u);
        mem.WriteU16(c.S0 + 0xEu, (ushort)c.V0);
        c.V0 = 0x80090000u;
        c.V0 = mem.ReadU16(c.V0 + 0x505Au);
        mem.WriteU16(c.S0 + 0x16u, (ushort)c.V0);
        c.V0 = mem.ReadU8(c.S1);
        mem.WriteU8(c.S0 + 0x1Cu, (byte)c.V0);
        mem.WriteU8(c.S0 + 0xCu, (byte)c.V0);
        c.V0 = mem.ReadU8(c.S1);
        c.V1 = mem.ReadU8(c.S1 + 0x2u);
        c.V0 = c.V0 + c.V1;
        mem.WriteU8(c.S0 + 0x24u, (byte)c.V0);
        mem.WriteU8(c.S0 + 0x14u, (byte)c.V0);
        c.V0 = mem.ReadU8(c.S1 + 0x1u);
        mem.WriteU8(c.S0 + 0x15u, (byte)c.V0);
        mem.WriteU8(c.S0 + 0xDu, (byte)c.V0);
        c.V0 = mem.ReadU8(c.S1 + 0x1u);
        c.V1 = mem.ReadU8(c.S1 + 0x3u);
        c.V0 = c.V0 + c.V1;
        mem.WriteU8(c.S0 + 0x25u, (byte)c.V0);
        mem.WriteU8(c.S0 + 0x1Du, (byte)c.V0);
        c.V0 = 0x80090000u;
        c.V0 = mem.ReadU8(c.V0 + 0x505Cu);
        mem.WriteU8(c.S0 + 0x4u, (byte)c.V0);
        c.V0 = 0x80090000u;
        c.V0 = mem.ReadU8(c.V0 + 0x505Du);
        mem.WriteU8(c.S0 + 0x5u, (byte)c.V0);
        c.V0 = 0x80090000u;
        c.V0 = mem.ReadU8(c.V0 + 0x505Eu);
        mem.WriteU8(c.S0 + 0x6u, (byte)c.V0);
        c.A0 = 0x80090000u;
        c.A0 = mem.ReadU32(c.A0 + 0xEBCu);
        c.A1 = c.S0;
        c.A0 = c.A0 + 0x4u;
        c.RA = 0x8001E5D8u;
        KingsField1_game.AddPrim_game(c, mem);
        L8001E5D8: ;
        c.RA = mem.ReadU32(c.SP + 0x18u);
        c.S1 = mem.ReadU32(c.SP + 0x14u);
        c.S0 = mem.ReadU32(c.SP + 0x10u);
        c.SP = c.SP + 0x20u;
        return;
    }
}
