using RecompOne.Runtime.Memory;

namespace Kf1;

/// <summary>
/// The menu's drawing written plainly over memory, for features that add to a menu page
/// (Verdite2's MenuDraw, on this disc's routines). Writes the same POLY_FT4 packets, in the
/// same order, as the routines in patches/Menu; each of those runs its entry here as a
/// twin under <c>KF1_MENU_*=verify</c>, compared on RAM. No CpuContext and no stack: only
/// the packets, the cursor words and the ordering table. See "The menu path in C#" in
/// docs/GPU_RENDERER.md.
/// </summary>
public static class MenuDraw
{
    public const uint Cursor = 0x80057E88;
    public const uint Record = 0x80070EB8;
    public const uint OrderingTable = 0x80090EBC;
    public const uint Buffer = 0x80070E98;
    public const uint WindowTemplate = 0x80058400;
    public const uint WindowPackets = 0x800580E8;

    public const int TextSlot = 1000, PanelSlot = 2000, WindowSlot = 2900, WindowPacketSlot = 3000;
    const uint QuadSize = 0x28;

    // func_8002ACCC: SetPolyFT4, colour 0x60.
    public static uint NewQuad(IMemory m)
    {
        uint p = m.ReadU32(Cursor);
        m.WriteU8(p + 3, 9);
        m.WriteU8(p + 7, 0x2C);
        m.WriteU8(p + 4, 0x60);
        m.WriteU8(p + 5, 0x60);
        m.WriteU8(p + 6, 0x60);
        return p;
    }

    // AddPrim(ot + slot, p).
    public static void AddPrim(IMemory m, uint p, int slot)
    {
        uint entry = m.ReadU32(OrderingTable) + (uint)(slot * 4);
        uint tag = m.ReadU32(p), head = m.ReadU32(entry);
        m.WriteU32(p, (tag & 0xFF000000) | (head & 0x00FFFFFF));
        m.WriteU32(entry, (head & 0xFF000000) | (p & 0x00FFFFFF));
    }

    // func_8002AD1C: links the cursor's packet and moves the cursor, mirrored into the record.
    public static void Link(IMemory m, int slot)
    {
        AddPrim(m, m.ReadU32(Cursor), slot);
        uint next = m.ReadU32(Cursor) + QuadSize;
        m.WriteU32(Cursor, next);
        m.WriteU32(m.ReadU32(Record) + 8, next);
    }

    static void SetSemiTrans(IMemory m, uint p) => m.WriteU8(p + 7, (byte)(m.ReadU8(p + 7) | 2));

    // Corners at (x, y) and (x + w, y + h); u0/u1 and v0/v1 swapped give the mirrored quads.
    static void Place(IMemory m, uint p, uint tmpl, int x, int y, int w, int h)
    {
        m.WriteU16(p + 0x16, m.ReadU16(tmpl));
        m.WriteU16(p + 0x0E, m.ReadU16(tmpl + 2));
        m.WriteU16(p + 0x08, (ushort)x);
        m.WriteU16(p + 0x0A, (ushort)y);
        m.WriteU16(p + 0x10, (ushort)(x + w));
        m.WriteU16(p + 0x12, (ushort)y);
        m.WriteU16(p + 0x18, (ushort)x);
        m.WriteU16(p + 0x1A, (ushort)(y + h));
        m.WriteU16(p + 0x20, (ushort)(x + w));
        m.WriteU16(p + 0x22, (ushort)(y + h));
    }

    static void Uv(IMemory m, uint p, int u0, int v0, int u1, int v1)
    {
        m.WriteU8(p + 0x0C, (byte)u0);
        m.WriteU8(p + 0x0D, (byte)v0);
        m.WriteU8(p + 0x14, (byte)u1);
        m.WriteU8(p + 0x15, (byte)v0);
        m.WriteU8(p + 0x1C, (byte)u0);
        m.WriteU8(p + 0x1D, (byte)v1);
        m.WriteU8(p + 0x24, (byte)u1);
        m.WriteU8(p + 0x25, (byte)v1);
    }

    static int W(IMemory m, uint t) => m.ReadU16(t + 8);
    static int H(IMemory m, uint t) => m.ReadU16(t + 0xA);

    // func_80029DE0. Codes are the font's, not ASCII; 0xFFFF ends them.
    public static void Text(IMemory m, uint tmpl, int x, int y, ReadOnlySpan<ushort> codes)
    {
        int step = 0;
        foreach (ushort code in codes)
        {
            if (code == 0xFFFF) break;
            int g = code & 0xFFF;
            int u = g % 16 * 14, v = g / 16 * 12;
            int uw = m.ReadU8(tmpl + 8), vh = m.ReadU8(tmpl + 0xA);

            uint p = NewQuad(m);
            Place(m, p, tmpl, x + step, y, W(m, tmpl), H(m, tmpl));
            Uv(m, p, u, v, u + uw, v + vh);
            Link(m, TextSlot);

            // Dakuten and handakuten marks over the same cell.
            foreach (var (bit, mu) in new[] { (0x1000, 196), (0x2000, 210) })
            {
                if ((code & bit) == 0) continue;
                p = NewQuad(m);
                Place(m, p, tmpl, x + step, y, W(m, tmpl), H(m, tmpl));
                Uv(m, p, mu, 24, mu + uw, 24 + vh);
                Link(m, TextSlot);
            }
            step += 14;
        }
    }

    // func_8002A310: digit d from v = d * 11 under the template's u, 7 px a step.
    public static void Digits(IMemory m, uint tmpl, int x, int y, ReadOnlySpan<ushort> digits)
    {
        int step = 0;
        foreach (ushort d in digits)
        {
            if (d == 0xFFFF) break;
            int u = m.ReadU8(tmpl + 4), v = (short)d * 11;
            uint p = NewQuad(m);
            Place(m, p, tmpl, x + step, y, W(m, tmpl), H(m, tmpl));
            Uv(m, p, u, v, u + m.ReadU8(tmpl + 8), v + m.ReadU8(tmpl + 0xA));
            Link(m, TextSlot);
            step += 7;
        }
    }

    // func_8002ADF8: width digits, right-aligned, blank (10) or zero padded, then 0xFFFF.
    public static ushort[] FormatNumber(int value, int width, bool zeroPad)
    {
        var digits = new ushort[width + 1];
        Array.Fill(digits, zeroPad ? (ushort)0 : (ushort)10);
        digits[width] = 0xFFFF;
        for (int i = width - 1; i >= 0; i--)
        {
            digits[i] = (ushort)(value % 10);
            value /= 10;
            if (value == 0) break;
        }
        return digits;
    }

    public static void Number(IMemory m, uint tmpl, int x, int y, int value, int width, bool zeroPad = false) =>
        Digits(m, tmpl, x, y, FormatNumber(value, width, zeroPad));

    // func_80029AB0 (semi-transparent, 4/3 px in) and func_80029C50 (opaque, 18/2 px in).
    public static void Panel(IMemory m, uint tmpl, int x, int y, bool inset = false)
    {
        int dx = inset ? 18 : 4, dy = inset ? 2 : 3;
        int u = m.ReadU8(tmpl + 4), v = m.ReadU8(tmpl + 6);
        uint p = NewQuad(m);
        Place(m, p, tmpl, x - dx, y - dy, W(m, tmpl), H(m, tmpl));
        Uv(m, p, u, v, u + m.ReadU8(tmpl + 8), v + m.ReadU8(tmpl + 0xA));
        if (!inset) SetSemiTrans(m, p);
        Link(m, PanelSlot);
    }

    // func_8002A510: one quarter of the window, mirrored into four at (166, 16), then the
    // current buffer's four prebuilt packets.
    public static void Window(IMemory m)
    {
        uint t = WindowTemplate;
        int u = m.ReadU8(t + 4), v = m.ReadU8(t + 6);
        int u1 = u + m.ReadU8(t + 8), v1 = v + m.ReadU8(t + 0xA);
        foreach (var (x, y, flipU, flipV) in new[] { (166, 16, false, false), (237, 16, true, false),
                                                     (166, 120, false, true), (237, 120, true, true) })
        {
            uint p = NewQuad(m);
            SetSemiTrans(m, p);
            Place(m, p, t, x, y, W(m, t), H(m, t));
            Uv(m, p, flipU ? u1 : u, flipV ? v1 : v, flipU ? u : u1, flipV ? v : v1);
            Link(m, WindowSlot);
        }
        uint set = WindowPackets + m.ReadU8(Buffer) * 0xA0u;
        for (int i = 3; i >= 0; i--) AddPrim(m, set + (uint)i * QuadSize, WindowPacketSlot);
    }

    /// <summary>A routine's record (<c>s16 x, s16 y</c>, then u16s to a 0xFFFF), as the twins read it.</summary>
    internal static (int X, int Y, ushort[] Codes) ReadRecord(IMemory m, uint rec)
    {
        var codes = new List<ushort>();
        for (uint a = rec + 4; ; a += 2)
        {
            ushort code = m.ReadU16(a);
            if (code == 0xFFFF) break;
            codes.Add(code);
        }
        return (m.ReadU16(rec), m.ReadU16(rec + 2), [.. codes]);
    }
}
