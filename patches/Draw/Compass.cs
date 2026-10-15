using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001F8B0</c>, the compass, turned by the view's yaw. Takes no arguments. Saves
/// the colour matrix, installs the one at <c>0x80095720</c>, then walks the compass table at
/// <c>0x80055D74</c> in 0x1C-byte entries while each entry's first byte is 1. Each entry
/// gets a rotation matrix from the vector at its +0xE, scaled by its u16 at +4, set with a
/// translation, and is posed (<c>func_800205D4</c>). A non-zero pose also runs the object's
/// vertex cache (<c>func_8001C754</c>) and polygon writer (<c>func_8001C7F8</c>). The saved
/// colour matrix is restored at the end. <c>KF1_COMPASS</c>.
/// </summary>
public static class Compass
{
    public static readonly Literal R = new(typeof(Compass), "compass", "COMPASS", 0x8001F8B0, Run,
        "Draws the compass, turned by the view's yaw.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x78u;
        mem.WriteU32(c.SP + 0x74u, c.RA);
        mem.WriteU32(c.SP + 0x70u, c.S2);
        mem.WriteU32(c.SP + 0x6Cu, c.S1);
        mem.WriteU32(c.SP + 0x68u, c.S0);
        // save the current colour matrix at SP+0x48, then install the compass's
        c.A0 = c.SP + 0x48u;
        c.RA = 0x8001F8CCu;
        KingsField1_game.ReadColorMatrix_game(c, mem);
        c.A0 = 0x80090000u;
        c.A0 = c.A0 + 0x5720u;
        c.RA = 0x8001F8DCu;
        KingsField1_game.SetColorMatrix_game(c, mem);
        c.V0 = 0u | 0x1000u;
        mem.WriteU32(c.SP + 0x40u, c.V0);
        c.S2 = 0x80050000u;
        c.S2 = c.S2 + 0x5D74u;
        c.V1 = mem.ReadU8(c.S2);
        c.V0 = 0u | 0x0001u;
        if (c.V1 == c.V0) {
            c.S0 = c.S2 + 0x2u;
            // one entry per pass; the loop runs while the entry's first byte is 1
            do {
                Poll(c, mem);
                c.V0 = (uint)(short)mem.ReadU16(c.S0 + 0x4u);
                mem.WriteU32(c.SP + 0x2Cu, c.V0);
                c.V0 = (uint)(short)mem.ReadU16(c.S0 + 0x6u);
                c.A0 = c.S2 + 0xEu;
                mem.WriteU32(c.SP + 0x30u, c.V0);
                c.V0 = (uint)(short)mem.ReadU16(c.S0 + 0x8u);
                c.A1 = c.SP + 0x18u;
                mem.WriteU32(c.SP + 0x34u, c.V0);
                // rotation matrix from the vector at entry+0xE, then scaled by the u16 at +4
                c.RA = 0x8001F928u;
                KingsField1_game.func_8004E9B8(c, mem);
                c.A0 = c.SP + 0x18u;
                c.V0 = mem.ReadU16(c.S0 + 0x2u);
                c.A1 = c.SP + 0x38u;
                mem.WriteU32(c.SP + 0x3Cu, c.V0);
                mem.WriteU32(c.SP + 0x38u, c.V0);
                c.RA = 0x8001F940u;
                KingsField1_game.ScaleMatrix_game(c, mem);
                c.A0 = c.SP + 0x18u;
                c.RA = 0x8001F948u;
                KingsField1_game.SetRotMatrix_game(c, mem);
                c.A0 = c.SP + 0x18u;
                c.RA = 0x8001F950u;
                KingsField1_game.SetTransMatrix_game(c, mem);
                c.A0 = 0u | 0x0015u;
                c.RA = 0x8001F958u;
                KingsField1_game.func_8002059C(c, mem);
                // the selected TMD object, posed from the entry
                c.A0 = 0u;
                c.RA = 0x8001F960u;
                KingsField1_game.func_8001C114(c, mem);
                c.A0 = c.S2 + 0x18u;
                c.S1 = c.V0;
                c.A2 = mem.ReadU8(c.S0 - 0x1u);
                c.A3 = mem.ReadU16(c.S0);
                c.V0 = mem.ReadU32(c.S1 + 0x4u);
                c.A1 = 0u | 0x0015u;
                mem.WriteU32(c.SP + 0x10u, c.V0);
                c.RA = 0x8001F980u;
                KingsField1_game.func_800205D4(c, mem);
                // both paths step to the next entry; the draw runs only when the pose returned non-zero
                bool drawn = c.V0 != 0u;
                c.S0 = c.S0 + 0x1Cu;
                if (drawn) {
                    c.A0 = mem.ReadU32(c.S1 + 0x4u);
                    c.RA = 0x8001F994u;
                    KingsField1_game.func_8001C754(c, mem);
                    c.A0 = 0u;
                    c.A1 = 0u;
                    c.RA = 0x8001F9A0u;
                    KingsField1_game.func_8001C7F8(c, mem);
                }
                c.S2 = c.S2 + 0x1Cu;
                c.V1 = mem.ReadU8(c.S2);
                c.V0 = 0u | 0x0001u;
            } while (c.V1 == c.V0);
        }
        // restore the colour matrix saved at SP+0x48
        c.A0 = c.SP + 0x48u;
        c.RA = 0x8001F9BCu;
        KingsField1_game.SetColorMatrix_game(c, mem);
        c.RA = mem.ReadU32(c.SP + 0x74u);
        c.S2 = mem.ReadU32(c.SP + 0x70u);
        c.S1 = mem.ReadU32(c.SP + 0x6Cu);
        c.S0 = mem.ReadU32(c.SP + 0x68u);
        c.SP = c.SP + 0x78u;
    }
}
