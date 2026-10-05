using System.Runtime.CompilerServices;

namespace RecompOne.Runtime;

/// <summary>
/// 0050. Each packet's corner depths, recorded by the port that built the packet.
///
/// The address map (<see cref="GteVertexMap"/>) recovers a depth by following a
/// screen word from the GTE to the packet, so it answers for anything that projected
/// through the GTE, the skybox included, and misses a corner it could not follow. A
/// port that assembles the packet itself knows every corner's depth and which routine
/// asked for it, so it records only what should occlude: the map tiles, their clipped
/// fans and the models. The GPU finds a record by the address it read the command
/// word from and checks the command word and the first and last vertex words before
/// believing it.
///
/// While <see cref="Active"/>, the depth buffer is fed from here alone, and a packet
/// with no record keeps painter's order. Perspective and sub-pixel still come from the
/// address map. Nothing here writes guest memory or the GTE.
/// </summary>
public static class GtePacketDepth
{
    public struct Rec
    {
        public uint Cmd, Xy0, XyLast;
        public float Z0, Z1, Z2, Z3;
        // A blended packet that is still a solid surface: it writes its depth for
        // the occlusion pass. The port sets it for models (a secret door).
        public bool Solid;
        // 0067. The surface's material (SurfaceMaterial), when the port that built
        // the packet knows it; None leaves the choice to the texture.
        public byte Material;
        // 0079. Built by the model submitter rather than the tile walk; the probe's only.
        public bool Model;
        // The port knows the packet is none of the materials SurfaceMaterial.Rects
        // stand for, whatever its texels: a creature drawn in the water's texture.
        public bool NotRect;
    }

    /// <summary>The port's switch.</summary>
    public static bool Enabled;

    /// <summary>PGXP's CPU tracking sends every assembler back to recompiled code,
    /// which records nothing.</summary>
    public static bool Active => Enabled && GteDepth.DepthWanted && !Pgxp.Pgxp.CpuTracking;

    static Rec[]? _recs;
    static Rec _discard;
    static uint _base, _count;

    /// <summary>The address range packets are built in.</summary>
    public static void SetRange(uint baseAddress, uint bytes)
    {
        _base = baseAddress & 0x1FFFFFFFu;
        _count = bytes >> 2;
        _recs = new Rec[_count];
    }

    /// <summary>The record for the packet at <paramref name="pkt"/>, or a scratch one
    /// when it is outside the range.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static ref Rec Slot(uint pkt)
    {
        uint i = ((pkt & 0x1FFFFFFFu) - _base) >> 2;
        if (_recs == null || i >= _count) return ref _discard;
        return ref _recs[i];
    }

    /// <summary>The record for the packet whose command word the GPU read from
    /// <paramref name="cmdSrc"/>, if it still describes these words.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static ref readonly Rec Find(uint cmdSrc, uint cmd, uint xy0, uint xyLast, out bool found)
    {
        found = false;
        uint i = ((cmdSrc & 0x1FFFFFFFu) - 4u - _base) >> 2;
        if (_recs == null || cmdSrc == 0 || i >= _count) { Misses++; return ref _discard; }
        ref readonly var r = ref _recs[i];
        if (r.Cmd == 0 || r.Cmd != cmd || r.Xy0 != xy0 || r.XyLast != xyLast) { Misses++; return ref _discard; }
        found = true;
        Hits++;
        return ref r;
    }

    /// <summary>0079. <see cref="Find"/> without the hit and miss counters.</summary>
    public static bool Peek(uint cmdSrc, uint cmd, uint xy0, uint xyLast, out Rec rec)
    {
        rec = default;
        uint i = ((cmdSrc & 0x1FFFFFFFu) - 4u - _base) >> 2;
        if (_recs == null || cmdSrc == 0 || i >= _count) return false;
        ref readonly var r = ref _recs[i];
        if (r.Cmd == 0 || r.Cmd != cmd || r.Xy0 != xy0 || r.XyLast != xyLast) return false;
        rec = r;
        return true;
    }

    /// <summary>Records written, and polygons that found theirs or did not.</summary>
    public static long Recorded, Hits, Misses;

    public static void ResetCounters() => Recorded = Hits = Misses = 0;
}
