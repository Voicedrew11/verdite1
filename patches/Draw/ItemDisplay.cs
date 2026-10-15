using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;
using static Kf1.Mips;

namespace Kf1;

/// <summary>
/// <c>func_8001FAFC</c>, the selected item's display, transcribed literally.
/// <c>KF1_ITEMDISPLAY</c>. Called by the renderer; takes no arguments.
/// The state byte is <c>u8[0x80095088]</c>. State 0 waits for an item in the
/// selected slot (<c>u8[0x8009506E + u8[0x80095086]]</c>, <c>0xFF</c> empty) and
/// starts turning it. States 2 and 3 turn it in: state 2 counts down
/// <c>u8[0x80095089]</c>, state 3 steps the angle <c>u16[0x8009508A]</c> by 0x80
/// up to 0x200. Item <c>0x13</c> draws four digit models instead of one. The
/// display records are at <c>0x80055D20</c> and <c>0x80055D2E..0x80055D66</c>.
/// </summary>
public static class ItemDisplay
{
    public static readonly Literal R = new(typeof(ItemDisplay), "item display", "ITEMDISPLAY", 0x8001FAFC, Run,
        "Draws the selected item, turning.");

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        if (HeldThisFrame()) return;
        R.Replace(orig, c, m);
    }

    // The tick the display last stepped on.
    static long _stepped = -1;

    /// <summary>The routine steps its state once a call, and the renderer calls it once
    /// a frame. Under pacing it runs on a tick's first frame only; a frame it skips
    /// leaves its records as that tick left them. KF1_ITEMDISPLAY_HOLD=0 to compare.</summary>
    static bool HeldThisFrame()
    {
        if (!Hold || !FramePacing.Enabled) return false;
        if (!FramePacing.Frozen && _stepped != FramePacing.Ticks)
        {
            _stepped = FramePacing.Ticks;
            return false;
        }
        return true;
    }

    static readonly bool Hold = Environment.GetEnvironmentVariable("KF1_ITEMDISPLAY_HOLD")?.Trim() != "0";

    static void Run(CpuContext c, PSMemory mem)
    {
        c.SP = c.SP - 0x38u;
        mem.WriteU32(c.SP + 0x30u, c.RA);
        mem.WriteU32(c.SP + 0x2Cu, c.S1);
        mem.WriteU32(c.SP + 0x28u, c.S0);
        c.T0 = 0x80090000u;
        c.T0 = c.T0 + 0x5088u;
        c.V1 = mem.ReadU8(c.T0);
        c.V0 = 0u | 0x0002u;
        bool two = c.V1 == c.V0;
        c.V0 = (int)c.V1 < 3 ? 1u : 0u;
        bool low = c.V0 != 0u;
        // Each state's branch is set here in the order the code lays them out,
        // and run below. Only one of them runs.
        bool zero = false;
        bool three = false;
        if (!two && low)
        {
            zero = c.V1 == 0u;
            c.V0 = 0u | 0x00FFu;
        }
        if (!two && !low)
        {
            c.V0 = 0u | 0x0003u;
            three = c.V1 == c.V0;
        }
        if (zero)
        {
            // State 0: an item in the selected slot starts turning (state 2).
            c.A3 = 0x80090000u;
            c.A3 = mem.ReadU8(c.A3 + 0x5086u);
            c.At = 0x80090000u;
            c.At = c.At + 0x506Eu;
            c.At = c.At + c.A3;
            c.V1 = mem.ReadU8(c.At);
            c.A0 = c.V1 & 0x00FFu;
            bool empty = c.A0 == c.V0;
            c.V0 = 0u | 0x0002u;
            if (!empty)
            {
                mem.WriteU8(c.T0, (byte)c.V0);
                c.At = 0x80090000u;
                mem.WriteU16(c.At + 0x508Au, (ushort)0u);
                c.V0 = 0u | 0x000Fu;
                c.At = 0x80090000u;
                mem.WriteU8(c.At + 0x5089u, (byte)c.V0);
                c.V0 = 0u | 0x0013u;
                bool notDigits = c.A0 != c.V0;
                c.V0 = 0u | 0x0001u;
                if (!notDigits)
                {
                    // Item 0x13: four digit models, one per record.
                    c.S1 = 0x80050000u;
                    c.S1 = c.S1 + 0x5D20u;
                    mem.WriteU8(c.S1, (byte)0u);
                    c.S0 = 0u | 0x0001u;
                    c.At = 0x80050000u;
                    mem.WriteU8(c.At + 0x5D2Eu, (byte)c.S0);
                    c.V0 = c.V1 & 0x00F0u;
                    c.V0 = c.V0 << 3;
                    c.At = 0x80050000u;
                    mem.WriteU8(c.At + 0x5D30u, (byte)c.V0);
                    c.V0 = c.V1 & 0x000Fu;
                    c.V0 = c.V0 << 4;
                    c.At = 0x80050000u;
                    mem.WriteU8(c.At + 0x5D31u, (byte)c.V0);
                    c.A1 = 0u | 0x0004u;
                    c.A2 = 0u;
                    c.V0 = c.A3 << 1;
                    c.V0 = c.T0 + c.V0;
                    c.A0 = mem.ReadU16(c.V0 - 0x12u);
                    c.A3 = c.SP + 0x10u;
                    c.RA = 0x8001FBF4u;
                    KingsField1_game.func_8002ADF8(c, mem);
                    c.At = 0x80050000u;
                    mem.WriteU8(c.At + 0x5D3Cu, (byte)c.S0);
                    c.A1 = mem.ReadU16(c.SP + 0x16u);
                    c.A0 = c.S1 + 0x1Eu;
                    c.RA = 0x8001FC08u;
                    KingsField1_game.func_8001FAE4(c, mem);
                    c.At = 0x80050000u;
                    mem.WriteU8(c.At + 0x5D4Au, (byte)c.S0);
                    c.A1 = mem.ReadU16(c.SP + 0x14u);
                    c.A0 = c.S1 + 0x2Cu;
                    c.RA = 0x8001FC1Cu;
                    KingsField1_game.func_8001FAE4(c, mem);
                    c.At = 0x80050000u;
                    mem.WriteU8(c.At + 0x5D58u, (byte)c.S0);
                    c.A1 = mem.ReadU16(c.SP + 0x12u);
                    c.A0 = c.S1 + 0x3Au;
                    c.RA = 0x8001FC30u;
                    KingsField1_game.func_8001FAE4(c, mem);
                    c.At = 0x80050000u;
                    mem.WriteU8(c.At + 0x5D66u, (byte)c.S0);
                    c.A1 = mem.ReadU16(c.SP + 0x10u);
                    c.A0 = c.S1 + 0x48u;
                    c.RA = 0x8001FC44u;
                    KingsField1_game.func_8001FAE4(c, mem);
                }
                if (notDigits)
                {
                    c.At = 0x80050000u;
                    mem.WriteU8(c.At + 0x5D20u, (byte)c.V0);
                    c.V0 = c.V1 & 0x00F0u;
                    c.V0 = c.V0 << 3;
                    c.At = 0x80050000u;
                    mem.WriteU8(c.At + 0x5D22u, (byte)c.V0);
                    c.V0 = c.V1 & 0x000Fu;
                    c.V0 = c.V0 << 4;
                    c.At = 0x80050000u;
                    mem.WriteU8(c.At + 0x5D23u, (byte)c.V0);
                    c.At = 0x80050000u;
                    mem.WriteU8(c.At + 0x5D66u, (byte)0u);
                    c.At = 0x80050000u;
                    mem.WriteU8(c.At + 0x5D58u, (byte)0u);
                    c.At = 0x80050000u;
                    mem.WriteU8(c.At + 0x5D4Au, (byte)0u);
                    c.At = 0x80050000u;
                    mem.WriteU8(c.At + 0x5D3Cu, (byte)0u);
                    c.At = 0x80050000u;
                    mem.WriteU8(c.At + 0x5D2Eu, (byte)0u);
                }
            }
        }
        if (two)
        {
            // State 2: count down, and at zero move on to state 3.
            c.V0 = 0x80090000u;
            c.V0 = mem.ReadU8(c.V0 + 0x5089u);
            c.V0 = c.V0 - 0x1u;
            c.At = 0x80090000u;
            mem.WriteU8(c.At + 0x5089u, (byte)c.V0);
            c.V0 = c.V0 & 0x00FFu;
            bool busy = c.V0 != 0u;
            c.V0 = 0u | 0x0003u;
            if (!busy)
            {
                mem.WriteU8(c.T0, (byte)c.V0);
            }
        }
        if (three)
        {
            // State 3: step the angle, and once it is full, clear the records and step the slot index.
            c.V0 = 0x80090000u;
            c.V0 = mem.ReadU16(c.V0 + 0x508Au);
            c.V0 = c.V0 + 0x80u;
            c.At = 0x80090000u;
            mem.WriteU16(c.At + 0x508Au, (ushort)c.V0);
            c.V0 = c.V0 << 16;
            c.V0 = (uint)((int)c.V0 >> 16);
            c.V0 = (int)c.V0 < 512 ? 1u : 0u;
            bool within = c.V0 != 0u;
            c.V0 = 0u | 0x0200u;
            if (!within)
            {
                c.At = 0x80090000u;
                mem.WriteU16(c.At + 0x508Au, (ushort)c.V0);
                c.At = 0x80050000u;
                mem.WriteU8(c.At + 0x5D66u, (byte)0u);
                c.At = 0x80050000u;
                mem.WriteU8(c.At + 0x5D58u, (byte)0u);
                c.At = 0x80050000u;
                mem.WriteU8(c.At + 0x5D4Au, (byte)0u);
                c.At = 0x80050000u;
                mem.WriteU8(c.At + 0x5D3Cu, (byte)0u);
                c.At = 0x80050000u;
                mem.WriteU8(c.At + 0x5D2Eu, (byte)0u);
                c.At = 0x80050000u;
                mem.WriteU8(c.At + 0x5D20u, (byte)0u);
                c.V1 = c.T0 - 0x2u;
                c.A2 = 0u | 0x00FFu;
                c.V0 = 0x80090000u;
                c.V0 = mem.ReadU8(c.V0 + 0x5086u);
                c.A1 = 0u | 0x0013u;
                c.At = 0x80090000u;
                c.At = c.At + 0x506Eu;
                c.At = c.At + c.V0;
                c.A0 = mem.ReadU8(c.At);
                while (true)
                {
                    Poll(c, mem);
                    c.V0 = mem.ReadU8(c.V1);
                    c.At = 0x80090000u;
                    c.At = c.At + 0x506Eu;
                    c.At = c.At + c.V0;
                    mem.WriteU8(c.At, (byte)c.A2);
                    c.V0 = mem.ReadU8(c.V1);
                    c.V0 = c.V0 + 0x1u;
                    c.V0 = c.V0 & 0x0007u;
                    mem.WriteU8(c.V1, (byte)c.V0);
                    c.V0 = mem.ReadU8(c.V1);
                    c.At = 0x80090000u;
                    c.At = c.At + 0x506Eu;
                    c.At = c.At + c.V0;
                    c.V0 = mem.ReadU8(c.At);
                    if (c.A0 != c.V0) break;
                    if (c.A0 == c.A1) break;
                }
                mem.WriteU8(c.V1 + 0x2u, (byte)0u);
            }
        }
        c.RA = mem.ReadU32(c.SP + 0x30u);
        c.S1 = mem.ReadU32(c.SP + 0x2Cu);
        c.S0 = mem.ReadU32(c.SP + 0x28u);
        c.SP = c.SP + 0x38u;
    }
}
