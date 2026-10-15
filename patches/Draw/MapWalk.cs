using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001E83C</c>, the map walk, transcribed literally. <c>KF1_MAPWALK</c>.
/// Arguments: none. Reads its window from the 0x80090000 page (u16 0x5754,
/// 0x575C, 0x575E) and the cell table pointer at u32 0x80095860. Walks rows and
/// columns of that table and calls <c>func_8001E5EC</c> for each cell whose
/// entry is non-zero.
/// </summary>
public static class MapWalk
{
    public static readonly Literal R = new(typeof(MapWalk), "map walk", "MAPWALK", 0x8001E83C, Run,
        "Walks the cells around the eye through the 16 stencils.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m) => R.Replace(orig, c, m);

    static void Run(CpuContext c, PSMemory mem)
    {
        c.V0 = 0x80090000u;
        c.V0 = mem.ReadU16(c.V0 + 0x5754u);
        c.SP = c.SP - 0x30u;
        mem.WriteU32(c.SP + 0x28u, c.RA);
        mem.WriteU32(c.SP + 0x24u, c.S5);
        mem.WriteU32(c.SP + 0x20u, c.S4);
        mem.WriteU32(c.SP + 0x1Cu, c.S3);
        mem.WriteU32(c.SP + 0x18u, c.S2);
        mem.WriteU32(c.SP + 0x14u, c.S1);
        c.V0 = c.V0 + 0x1FFu;
        c.V0 = c.V0 & 0xFFFFu;
        c.V0 = c.V0 < 0x000003FFu ? 1u : 0u;
        // (value + 0x1FF) & 0xFFFF < 0x3FF: the eye is inside the window.
        bool inWindow = c.V0 != 0u;
        mem.WriteU32(c.SP + 0x10u, c.S0);
        if (!inWindow)
        {
            c.V0 = 0x80050000u;
            c.V0 = c.V0 + 0x5E9Cu;
        }
        else
        {
            // Window size from the signed byte at 0x80095757 (15 - v).
            c.V0 = 0x80090000u;
            c.V0 = (uint)(sbyte)mem.ReadU8(c.V0 + 0x5757u);
            c.V1 = 0u | 0x000Fu;
            c.V1 = c.V1 - c.V0;
            c.V0 = c.V1 << 1;
            c.V0 = c.V0 + c.V1;
            c.V1 = c.V0 << 4;
            c.V0 = c.V0 + c.V1;
            c.V0 = c.V0 << 2;
            c.V1 = 0x80060000u;
            c.V1 = c.V1 + 0x5BE8u;
            c.V0 = c.V0 + c.V1;
        }
        c.At = 0x80090000u;
        mem.WriteU32(c.At + 0x5860u, c.V0);
        c.A0 = 0u;
        c.V0 = 0x80090000u;
        c.V0 = mem.ReadU32(c.V0 + 0x5860u);
        c.A2 = 0x80090000u;
        c.A2 = mem.ReadU16(c.A2 + 0x575Eu);
        c.A1 = 0x80090000u;
        c.A1 = mem.ReadU16(c.A1 + 0x575Cu);
        c.V1 = mem.ReadU16(c.V0 + 0x6u);
        c.S2 = c.V0 + 0x8u;
        c.V0 = mem.ReadU16(c.V0 + 0x4u);
        c.S3 = c.A2 - c.V1;
        c.S5 = c.A1 - c.V0;
        c.RA = 0x8001E8F0u;
        KingsField1_game.func_8001C0E8(c, mem);
        c.V0 = 0x80090000u;
        c.V0 = mem.ReadU32(c.V0 + 0x5860u);
        c.S4 = mem.ReadU8(c.V0 + 0x2u);

        // Rows: S3 runs from the first row to the window's far edge.
        while (true)
        {
            Poll(c, mem);
            c.V0 = c.S3 < 0x00000064u ? 1u : 0u;
            bool rowInRange = c.V0 != 0u;
            c.S0 = c.S5;
            if (rowInRange)
            {
                // Columns: S0 from the first column; S1 counts down the row's cells.
                c.V0 = 0x80090000u;
                c.V0 = mem.ReadU32(c.V0 + 0x5860u);
                c.S1 = mem.ReadU8(c.V0);
                while (true)
                {
                    Poll(c, mem);
                    c.V0 = c.S0 < 0x00000064u ? 1u : 0u;
                    bool colInRange = c.V0 != 0u;
                    c.S1 = c.S1 - 0x1u;
                    if (colInRange)
                    {
                        // Entry at S2; a non-zero byte means the cell is drawn.
                        c.A2 = mem.ReadU8(c.S2);
                        bool empty = c.A2 == 0u;
                        c.A0 = c.S0;
                        if (!empty)
                        {
                            c.A1 = c.S3;
                            c.RA = 0x8001E940u;
                            KingsField1_game.func_8001E5EC(c, mem);
                        }
                    }
                    c.S2 = c.S2 + 0x1u;
                    c.V0 = c.S1 & 0x00FFu;
                    bool moreCols = c.V0 != 0u;
                    c.S0 = c.S0 + 0x1u;
                    if (!moreCols) break;
                }
                c.S4 = c.S4 - 0x1u;
            }
            else
            {
                // Past the window: skip this row's entries in the table.
                c.V0 = 0x80090000u;
                c.V0 = mem.ReadU32(c.V0 + 0x5860u);
                c.V0 = mem.ReadU16(c.V0);
                c.S2 = c.S2 + c.V0;
                c.S4 = c.S4 - 0x1u;
            }
            c.V0 = c.S4 & 0x00FFu;
            bool moreRows = c.V0 != 0u;
            c.S3 = c.S3 + 0x1u;
            if (!moreRows) break;
        }

        c.RA = mem.ReadU32(c.SP + 0x28u);
        c.S5 = mem.ReadU32(c.SP + 0x24u);
        c.S4 = mem.ReadU32(c.SP + 0x20u);
        c.S3 = mem.ReadU32(c.SP + 0x1Cu);
        c.S2 = mem.ReadU32(c.SP + 0x18u);
        c.S1 = mem.ReadU32(c.SP + 0x14u);
        c.S0 = mem.ReadU32(c.SP + 0x10u);
        c.SP = c.SP + 0x30u;
        return;
    }
}
