using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001E5EC</c>, the map cell, transcribed literally. <c>KF1_MAPCELL</c>.
/// Arguments: a0, a1 index the cell byte at <c>u8[0x800A58B8 + 100*a1 + a0]</c>
/// (0xFF means empty); a2 selects one of the two offsets. Writes the cell's
/// matrices at 0x800956A0 and 0x80095760, then calls <c>func_8001DE18</c>.
/// The stencil-type chain from <c>L6A8</c> to <c>L6AC</c> keeps its
/// <c>goto</c>s: its paths join at shared tails.
/// </summary>
public static class MapCell
{
    public static readonly Literal R = new(typeof(MapCell), "map cell", "MAPCELL", 0x8001E5EC, Run,
        "Draws one map cell: its matrix, light and object.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        uint col = c.A0, row = c.A1;
        c.SP = c.SP - 0x78u;
        mem.WriteU32(c.SP + 0x74u, c.RA);
        mem.WriteU32(c.SP + 0x70u, c.S2);
        mem.WriteU32(c.SP + 0x6Cu, c.S1);
        mem.WriteU32(c.SP + 0x68u, c.S0);
        c.T0 = c.A0;
        c.V0 = c.A1 << 1;
        c.V0 = c.V0 + c.A1;
        c.V0 = c.V0 << 3;
        c.V0 = c.V0 + c.A1;
        c.V1 = 0x800A0000u;
        c.V1 = c.V1 - 0x58B8u;
        c.V0 = c.V0 << 2;
        c.V0 = c.V0 + c.V1;
        c.V0 = c.V0 + c.T0;
        c.S2 = mem.ReadU8(c.V0);

        // Every early exit is a break to the epilogue below.
        do
        {
            c.V0 = 0u | 0x00FFu;
            c.A3 = c.S2 & 0x00FFu;
            bool empty = c.A3 == c.V0;
            c.V0 = 0xFFFFFFFFu;
            if (empty) break;

            // Stencil type from the signed halfword at 0x800A07D2; -1 means none.
            c.V1 = 0x800A0000u;
            c.V1 = (uint)(short)mem.ReadU16(c.V1 + 0x7D2u);
            c.A0 = c.V1;
            if (c.V1 == c.V0) goto L6A8;
            c.V0 = c.A0 & 0x0003u;
            c.V0 = (int)c.V0 < 2 ? 1u : 0u;
            bool lowBits = c.V0 != 0u;
            c.V0 = 0u | 0x0045u;
            if (!lowBits) goto L6A8;
            bool is45 = c.A3 == c.V0;
            c.V0 = (int)c.A3 < 70 ? 1u : 0u;
            if (is45) goto L69C;
            bool ge70 = c.V0 == 0u;
            c.V0 = 0u | 0x0044u;
            if (ge70) goto L680;
            c.A0 = c.S2 + 0xFFu;
            if (c.A3 == c.V0) goto L694;
            goto L6AC;
            L680: ;
            c.V0 = 0u | 0x0046u;
            c.A0 = c.S2 + 0xFFu;
            if (c.A3 == c.V0) goto L6A4;
            goto L6AC;
            L694: ;
            c.S2 = 0u | 0x0017u;
            goto L6A8;
            L69C: ;
            c.S2 = 0u | 0x0018u;
            goto L6A8;
            L6A4: ;
            c.S2 = 0u | 0x0019u;
            L6A8: ;
            c.A0 = c.S2 + 0xFFu;
            L6AC: ;
            c.S2 = c.A0;

            // Type index below 100 draws; otherwise leave.
            c.V0 = c.S2 & 0x00FFu;
            c.V0 = c.V0 < 0x00000064u ? 1u : 0u;
            bool inRange = c.V0 != 0u;
            c.V0 = c.A1 << 1;
            if (!inRange) break;
            c.V0 = c.V0 + c.A1;
            c.V0 = c.V0 << 3;
            c.V0 = c.V0 + c.A1;
            c.V1 = 0x80070000u;
            c.V1 = c.V1 - 0x6FE8u;
            c.A3 = c.V0 << 2;
            c.V1 = c.A3 + c.V1;
            c.V1 = c.V1 + c.T0;
            c.V0 = mem.ReadU8(c.V1);
            c.T1 = 0u | 0x0001u;
            c.S1 = c.V0 - 0x1u;
            c.V0 = c.A2 & 0x00FFu;
            bool a2Is1 = c.V0 == c.T1;
            c.V1 = c.A1 << 5;
            if (a2Is1)
            {
                c.S2 = c.A0 + 0x64u;
            }

            // Matrix and light setup for the cell.
            c.V0 = c.T0 << 5;
            c.V0 = c.V0 - c.T0;
            c.V0 = c.V0 << 2;
            c.V0 = c.V0 + c.T0;
            c.V1 = c.V1 - c.A1;
            c.V1 = c.V1 << 2;
            c.V1 = c.V1 + c.A1;
            c.V0 = c.V0 << 4;
            c.V1 = c.V1 << 4;
            c.A0 = 0x80090000u;
            c.A0 = mem.ReadU16(c.A0 + 0x5744u);
            c.A1 = 0x80090000u;
            c.A1 = mem.ReadU16(c.A1 + 0x574Cu);
            c.A2 = c.V0 - c.A0;
            mem.WriteU16(c.SP + 0x50u, (ushort)c.A2);
            c.V0 = 0x80090000u;
            c.V0 = c.V0 + 0x5900u;
            c.V0 = c.A3 + c.V0;
            c.V0 = c.V0 + c.T0;
            c.A0 = mem.ReadU8(c.V0);
            c.A1 = c.V1 - c.A1;
            mem.WriteU16(c.SP + 0x54u, (ushort)c.A1);
            c.V1 = 0x80090000u;
            c.V1 = mem.ReadU16(c.V1 + 0x5748u);
            c.V0 = c.A0 << 1;
            c.V0 = c.V0 + c.A0;
            c.V0 = c.V0 << 3;
            c.V0 = c.V0 + c.A0;
            c.V0 = c.V0 << 2;
            c.V0 = 0u - c.V0;
            c.V0 = c.V0 - c.V1;

            // Screen offsets for the cell; the S1 tests pick which of them is biased by 0x7D0.
            bool s1Is1 = c.S1 == c.T1;
            mem.WriteU16(c.SP + 0x52u, (ushort)c.V0);
            bool s1Is2 = false;
            if (!s1Is1)
            {
                c.V0 = 0u | 0x0002u;
                s1Is2 = c.S1 == c.V0;
                c.V0 = 0u | 0x0003u;
                if (s1Is2)
                {
                    c.V0 = c.A2 + 0x7D0u;
                    mem.WriteU16(c.SP + 0x50u, (ushort)c.V0);
                }
            }
            if (s1Is1 || s1Is2)
            {
                c.V0 = c.A1 + 0x7D0u;
                mem.WriteU16(c.SP + 0x54u, (ushort)c.V0);
            }
            else
            {
                bool s1Is3 = c.S1 == c.V0;
                c.V0 = c.A2 + 0x7D0u;
                if (s1Is3)
                {
                    mem.WriteU16(c.SP + 0x50u, (ushort)c.V0);
                }
            }

            // Rotation, translation and light matrices, then the object.
            c.A0 = 0x80090000u;
            c.A0 = c.A0 + 0x56A0u;
            c.S1 = c.S1 << 5;
            c.RA = 0x8001E7BCu;
            KingsField1_game.SetRotMatrix_game(c, mem);
            c.A0 = 0x80090000u;
            c.A0 = c.A0 + 0x56A0u;
            c.RA = 0x8001E7CCu;
            KingsField1_game.SetTransMatrix_game(c, mem);
            c.A0 = c.SP + 0x50u;
            c.A1 = c.SP + 0x24u;
            c.A2 = c.SP + 0x58u;
            c.RA = 0x8001E7DCu;
            KingsField1_game.func_8004DADC(c, mem);
            c.S0 = 0x80090000u;
            c.S0 = c.S0 + 0x5760u;
            c.A0 = c.S0 - 0xC0u;
            c.A1 = c.S1 + c.S0;
            c.A2 = c.SP + 0x10u;
            c.RA = 0x8001E7F4u;
            KingsField1_game.func_8004CDD4(c, mem);
            c.A0 = c.SP + 0x10u;
            c.RA = 0x8001E7FCu;
            KingsField1_game.SetRotMatrix_game(c, mem);
            c.A0 = c.SP + 0x10u;
            c.RA = 0x8001E804u;
            KingsField1_game.SetTransMatrix_game(c, mem);
            c.S0 = c.S0 + 0x80u;
            c.A0 = c.S1 + c.S0;
            c.RA = 0x8001E810u;
            KingsField1_game.SetLightMatrix_game(c, mem);
            c.S0 = c.S2 & 0x00FFu;
            // The retained map draws the cell (GpuWorld): the writer is skipped.
            if (RetainedMap.Submit(mem, col, row, c.S0)) break;
            bool check = RetainedMap.Checking;
            if (check) RetainedMap.CheckCell(col, row, c.S0);
            c.A0 = c.S0;
            c.RA = 0x8001E81Cu;
            KingsField1_game.func_8001C148(c, mem);
            c.A0 = c.S0;
            c.RA = 0x8001E824u;
            KingsField1_game.func_8001DE18(c, mem);
            if (check) { RetainedMap.CheckVertices(mem); RetainedMap.CheckCell(0, 0, 0xFFFFFFFFu); }
        } while (false);

        c.RA = mem.ReadU32(c.SP + 0x74u);
        c.S2 = mem.ReadU32(c.SP + 0x70u);
        c.S1 = mem.ReadU32(c.SP + 0x6Cu);
        c.S0 = mem.ReadU32(c.SP + 0x68u);
        c.SP = c.SP + 0x78u;
        return;
    }
}
