using RecompOne.Runtime;
using RecompOne.Runtime.Assets.Textures;
using RecompOne.Runtime.Memory;

namespace Kf2.Remaster;

/// <summary>A piece of the game's art: upstream's index hash of its texels and the hash
/// of the CLUT entries they use (0 for any CLUT). The same art has the same key in
/// every area and on every disc; it names content, and is not a copy of it.</summary>
public readonly record struct TexKey(ulong Index, ulong Clut)
{
    public override string ToString() => Clut == 0 ? $"texture:{Index:x16}" : $"texture:{Index:x16}:{Clut:x16}";

    public TexKey AnyClut => this with { Clut = 0 };

    /// <summary><c>texture:INDEX</c> or <c>texture:INDEX:CLUT</c>, in hex.</summary>
    public static bool TryParse(string s, out TexKey key)
    {
        key = default;
        var p = s.Split(':');
        if (p.Length is < 2 or > 3 || p[0] != "texture") return false;
        if (!ulong.TryParse(p[1], System.Globalization.NumberStyles.HexNumber, null, out ulong i) || i == 0) return false;
        ulong c = 0;
        if (p.Length == 3 && !ulong.TryParse(p[2], System.Globalization.NumberStyles.HexNumber, null, out c)) return false;
        key = new TexKey(i, c);
        return true;
    }
}

/// <summary>
/// The texture a face draws, as a <see cref="TexKey"/>, for a material that follows the
/// art rather than the tile or the model. The face's UV rectangle is widened to the
/// image the game uploaded it in: a map face's 128x128 texture is drawn as four
/// quarter-quads by the subdivider, and every face reads a texel past its edge, so
/// the rectangles the packets carry are not the art. Hashed as upstream's texture
/// packs hash (<see cref="TextureTile.Hash"/>), and remembered per face rectangle
/// until VRAM under it changes. See "Phase 4, the second slice" in
/// docs/REMASTER.md.
/// </summary>
public static class TextureKeys
{
    struct Entry
    {
        public int Clock, Generation;
        public TexKey Key;
        public bool Valid;
    }

    static readonly Dictionary<ulong, Entry> _memo = new();

    public static long Lookups, Hashed, NoUpload;

    /// <summary>While the census is on: each key looked up, and how often.</summary>
    public static Dictionary<TexKey, long>? Census;

    /// <summary>The key of the art under a face rectangle (inclusive), or false for a face
    /// with no texture or VRAM the hash cannot read.</summary>
    public static bool Of(int tpage, int clut, int u0, int v0, int u1, int v1, out TexKey key)
    {
        key = default;
        var vram = Runtime.Gpu?.Vram;
        if (vram == null || u1 < u0 || v1 < v0) return false;
        Lookups++;
        ulong place = (ulong)(tpage & 0x1FF) | (ulong)(clut & 0x7FFF) << 9 | (ulong)u0 << 24 | (ulong)v0 << 32
                    | (ulong)u1 << 40 | (ulong)v1 << 48;
        if (Fluid(tpage, clut, u0, v0, u1, v1, out key)) return true;
        int clock = VramTracker.Clock;
        if (_memo.TryGetValue(place, out var e) && e.Clock == clock)
        {
            Count(e);
            key = e.Key;
            return e.Valid;
        }
        if (_memo.Count > 1 << 16) _memo.Clear();
        bool whole = ArtOf(tpage, u0, v0, u1, v1, out int cu, out int cv, out int cw, out int ch);
        var r = TextureTile.Describe(tpage, clut, cu, cv, cw, ch);
        int gen = VramTracker.Generation(r.VramX, r.VramY, r.VramW, r.H)
                ^ VramTracker.Generation(r.ClutX, r.ClutY, r.ClutCount, 1);
        if (!(e.Clock != 0 && e.Generation == gen))
        {
            Hashed++;
            if (!whole) NoUpload++;
            e.Valid = TextureTile.Hash(vram, r, out ulong index, out ulong clutHash);
            e.Key = new TexKey(index, clutHash);
            e.Generation = gen;
        }
        e.Clock = clock;
        _memo[place] = e;
        Count(e);
        key = e.Key;
        return e.Valid;
    }

    static void Count(in Entry e)
    {
        if (Census != null && e.Valid) Census[e.Key] = Census.GetValueOrDefault(e.Key) + 1;
    }

    // ---- the scrolling textures --------------------------------------------------

    /// <summary><c>func_8002DC78</c>'s eight slots: +0 live, +4 phase, +6 the dest RECT in
    /// VRAM words (x, y, w, h), +0x10 the source image in RAM, w words by h rows.</summary>
    const uint FluidSlots = 0x80192D58, FluidStride = 0x18;

    static readonly Dictionary<(uint Src, int W, int H, int Tpage, int Clut), TexKey> _fluid = new();
    static int _fluidSettle = -1;

    public static long FluidLookups;

    /// <summary>A face on a scrolling texture: its VRAM is rewritten every tick at a new
    /// phase, so its key is the slot's source image in RAM, hashed as
    /// <see cref="TextureTile.Hash"/> hashes the rectangle at phase 0 -- the key the
    /// VRAM would have had before the first scroll.</summary>
    static bool Fluid(int tpage, int clut, int u0, int v0, int u1, int v1, out TexKey key)
    {
        key = default;
        var m = Runtime.Mem;
        if (m == null) return false;
        // The slots, read again only when VRAM has been written since.
        int clock = VramTracker.Clock;
        if (clock != _slotsClock)
        {
            _slotsClock = clock;
            _slotN = 0;
            for (uint i = 0; i < 8; i++)
            {
                uint rec = FluidSlots + i * FluidStride;
                if (m.ReadU8(rec) != 1) continue;
                int x = (short)m.ReadU16(rec + 6u), y = (short)m.ReadU16(rec + 8u);
                int w = (short)m.ReadU16(rec + 0xAu), h = (short)m.ReadU16(rec + 0xCu);
                if (w <= 0 || h <= 0 || w * h > 16384) continue;
                _slots[_slotN++] = (x, y, w, h, m.ReadU32(rec + 0x10u));
            }
        }
        if (_slotN == 0) return false;
        int per = ((tpage >> 7) & 3) switch { 0 => 4, 1 => 2, _ => 1 };
        int wx = (tpage & 0xF) * 64 + (u0 + u1 + 1) / 2 / per, wy = ((tpage >> 4) & 1) * 256 + (v0 + v1 + 1) / 2;
        for (int i = 0; i < _slotN; i++)
        {
            var (x, y, w, h, src) = _slots[i];
            if (wx < x || wy < y || wx >= x + w || wy >= y + h) continue;
            if (_fluidSettle != Identity.Settles) { _fluid.Clear(); _fluidSettle = Identity.Settles; }
            var id = (src, w, h, tpage & 0x180, clut & 0x7FFF);
            FluidLookups++;
            if (!_fluid.TryGetValue(id, out key))
            {
                key = HashSource(m, src, w, h, per, clut);
                _fluid[id] = key;
            }
            if (Census != null && key.Index != 0) Census[key] = Census.GetValueOrDefault(key) + 1;
            return key.Index != 0;
        }
        return false;
    }

    static readonly (int X, int Y, int W, int H, uint Src)[] _slots = new (int, int, int, int, uint)[8];
    static int _slotN, _slotsClock = -1;

    /// <summary><see cref="TextureTile.Hash"/>, over a RAM image instead of VRAM.</summary>
    static TexKey HashSource(RecompOne.Runtime.Memory.IMemory m, uint src, int w, int h, int per, int clut)
    {
        var vram = Runtime.Gpu?.Vram;
        if (vram == null || (src & 0x1FFFFF) + (uint)(w * h * 2) > (uint)(Runtime.Mem as PSMemory)!.Ram.Length) return default;
        int bpp = per == 4 ? 4 : per == 2 ? 8 : 16;
        int tw = w * per;
        Span<ulong> used = stackalloc ulong[4];
        used.Clear();
        ulong hsh = 1469598103934665603UL;
        void Fnv(ref ulong x, byte b) { x ^= b; x *= 1099511628211UL; }
        Fnv(ref hsh, (byte)bpp);
        Fnv(ref hsh, (byte)(tw & 0xFF)); Fnv(ref hsh, (byte)(tw >> 8));
        Fnv(ref hsh, (byte)(h & 0xFF)); Fnv(ref hsh, (byte)(h >> 8));
        for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            uint v = m.ReadU16(src + (uint)((y * w + x) * 2));
            Fnv(ref hsh, (byte)(v & 0xFF));
            Fnv(ref hsh, (byte)(v >> 8));
            if (bpp == 4)
                for (int k = 0; k < 16; k += 4) used[0] |= 1UL << (int)((v >> k) & 0xF);
            else if (bpp == 8)
            {
                int a = (int)(v & 0xFF), b = (int)(v >> 8);
                used[a >> 6] |= 1UL << (a & 63);
                used[b >> 6] |= 1UL << (b & 63);
            }
        }
        if (bpp == 16) return new TexKey(hsh, 0);
        ulong c = 1469598103934665603UL;
        int cx = (clut & 0x3F) * 16, row = ((clut >> 6) & 0x1FF) * 1024;
        int count = bpp == 4 ? 16 : 256;
        for (int i = 0; i < count; i++)
        {
            if ((used[i >> 6] & (1UL << (i & 63))) == 0) continue;
            ushort e = vram[row + ((cx + i) & 1023)];
            Fnv(ref c, (byte)(i & 0xFF));
            Fnv(ref c, (byte)(e & 0xFF));
            Fnv(ref c, (byte)(e >> 8));
        }
        return new TexKey(hsh, c);
    }

    /// <summary>A packet's texture, from its words: its command code says how many
    /// corners, and whether each carries a colour word.</summary>
    public static bool OfPacket(IMemory m, uint pkt, uint cmd, out TexKey key)
    {
        key = default;
        uint code = cmd >> 24;
        if ((code & 0xE4u) != 0x24u) return false;
        int n = (code & 0x08u) != 0 ? 4 : 3;
        uint stride = (code & 0x10u) != 0 ? 12u : 8u;
        int u0 = 255, v0 = 255, u1 = 0, v1 = 0;
        uint w0 = 0, w1 = 0;
        for (int i = 0; i < n; i++)
        {
            uint w = m.ReadU32(pkt + 12u + stride * (uint)i);
            if (i == 0) w0 = w;
            else if (i == 1) w1 = w;
            int u = (int)(w & 0xFF), v = (int)((w >> 8) & 0xFF);
            u0 = Math.Min(u0, u); u1 = Math.Max(u1, u); v0 = Math.Min(v0, v); v1 = Math.Max(v1, v);
        }
        return Of((int)(w1 >> 16), (int)(w0 >> 16), u0, v0, u1, v1, out key);
    }

    /// <summary>The rectangle of art a face rectangle names: the image the game
    /// uploaded it in, as the texture packs key it (<see cref="TextureResolver.ToUpload"/>),
    /// else the face's own.</summary>
    static bool ArtOf(int tpage, int u0, int v0, int u1, int v1, out int cu, out int cv, out int cw, out int ch)
    {
        (cu, cv, cw, ch) = (u0, v0, u1 - u0 + 1, v1 - v0 + 1);
        return TextureResolver.ToUpload(tpage, ref cu, ref cv, ref cw, ref ch);
    }
}
