using RecompOne.Runtime;
using RecompOne.Runtime.Memory;

namespace Kf1;

/// <summary>
/// Each world packet's corner depths, recorded for the Z-buffer
/// (<see cref="ZBuffer"/>, <see cref="GtePacketDepth"/>), Verdite3's
/// PolyAssemblerDepth. The map writer records every packet; the polygon writers
/// record only inside the model walk, so the arm, the compass and the menu's item
/// view keep painter's order. The vertex cache keeps each corner's full SZ at +4,
/// so nothing is lost. A corner at SZ 0 leaves the face unrecorded.
/// </summary>
public static class PacketDepth
{
    static uint _rangeSize;

    /// <summary>The literal bodies call <see cref="Face"/> only while this holds.</summary>
    public static bool On => GtePacketDepth.Active;

    /// <summary>The packet at <paramref name="pkt"/>, its corners the cache entries
    /// <paramref name="p0"/>..<paramref name="p3"/> in packet order (p3 unused for a
    /// triangle), just before it is linked.</summary>
    public static void Face(PSMemory mem, uint pkt, uint p0, uint p1, uint p2, uint p3)
    {
        EnsureRange();
        ref var r = ref GtePacketDepth.Slot(pkt);
        uint code = mem.ReadU8(pkt + 7u);
        bool quad = (code & 0x08u) != 0u;
        // a vertex is xy, then uv when textured, then the next colour when Gouraud
        uint stride = 4u + ((code & 0x04u) != 0u ? 4u : 0u) + ((code & 0x10u) != 0u ? 4u : 0u);
        uint last = 8u + (quad ? 3u : 2u) * stride;
        r.Z0 = Z(mem, p0);
        r.Z1 = Z(mem, p1);
        r.Z2 = Z(mem, p2);
        r.Z3 = quad ? Z(mem, p3) : 0f;
        if (r.Z0 <= 0f || r.Z1 <= 0f || r.Z2 <= 0f || (quad && r.Z3 <= 0f)) { r.Cmd = 0; return; }
        r.Cmd = mem.ReadU32(pkt + 4u);
        r.Xy0 = mem.ReadU32(pkt + 8u);
        r.XyLast = mem.ReadU32(pkt + last);
        GtePacketDepth.Recorded++;
    }

    /// <summary>Drop whatever record a packet at this address had.</summary>
    public static void None(uint pkt)
    {
        EnsureRange();
        GtePacketDepth.Slot(pkt).Cmd = 0;
    }

    static float Z(PSMemory mem, uint p) => mem.ReadU16(p + 4u);

    static void EnsureRange()
    {
        uint ram = RecompOne.Runtime.Runtime.RamSize;
        if (_rangeSize == ram) return;
        GtePacketDepth.SetRange(0, ram);
        _rangeSize = ram;
    }
}
