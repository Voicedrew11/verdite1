using System.Runtime.CompilerServices;

namespace RecompOne.Runtime;

/// <summary>
/// 0060. The texture rectangle a packet samples, for the texture filters.
///
/// A filter that reads more than one texel must stay on the texture the polygon is
/// on: past its edge is other art in the same page, read through this polygon's
/// CLUT. The GPU takes the rectangle from the polygon's own UVs, which is exact for
/// a whole face. A clipped fan carries only the part of its face's UVs that survived
/// the clip, so the port records the face's rectangle by packet address, and a
/// fan's filter and its texture cache entry are then the same as its neighbours'.
/// Checked by the command word and the first and last vertex words, as
/// <see cref="GtePacketDepth"/> is.
/// </summary>
public static class GteTexRect
{
    public struct Rec
    {
        public uint Cmd, Xy0, XyLast;
        /// <summary>u0 | v0 &lt;&lt; 8 | u1 &lt;&lt; 16 | v1 &lt;&lt; 24, inclusive.</summary>
        public uint Rect;
    }

    /// <summary>Set while a texture filter is on.</summary>
    public static bool Active => GteDepth.Anisotropy > 1 || GteDepth.Mipmaps;

    static Rec[]? _recs;
    static Rec _discard;
    static uint _base, _count;

    public static void SetRange(uint baseAddress, uint bytes)
    {
        _base = baseAddress & 0x1FFFFFFFu;
        _count = bytes >> 2;
        _recs = new Rec[_count];
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static ref Rec Slot(uint pkt)
    {
        uint i = ((pkt & 0x1FFFFFFFu) - _base) >> 2;
        if (_recs == null || i >= _count) return ref _discard;
        return ref _recs[i];
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static bool Find(uint cmdSrc, uint cmd, uint xy0, uint xyLast, out uint rect)
    {
        rect = 0;
        uint i = ((cmdSrc & 0x1FFFFFFFu) - 4u - _base) >> 2;
        if (_recs == null || cmdSrc == 0 || i >= _count) return false;
        ref readonly var r = ref _recs[i];
        if (r.Cmd == 0 || r.Cmd != cmd || r.Xy0 != xy0 || r.XyLast != xyLast) return false;
        rect = r.Rect;
        Hits++;
        return true;
    }

    /// <summary>Records written, and polygons that took one.</summary>
    public static long Recorded, Hits;
}
