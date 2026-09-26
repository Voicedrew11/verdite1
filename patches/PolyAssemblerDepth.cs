using System.Runtime.CompilerServices;
using RecompOne.Runtime;
using RecompOne.Runtime.Memory;

namespace Kf2;

/// <summary>
/// Each packet's corner depths, recorded for the depth buffer (<see cref="ZBuffer"/>,
/// <c>GtePacketDepth</c>). Reads only, like the lighting records. The HUD's icons and
/// the first-person arm are not recorded, so they keep painter's order.
/// See "The assemblers write the depth" in docs/RENDERING.md.
/// </summary>
public static partial class PolyAssembler
{
    // SZ3 per vertex-cache slot, with the two cache words it was written beside.
    struct CacheDepth
    {
        public uint W0, W1;
        public float Z;
    }

    static readonly CacheDepth[] _cacheDepth = new CacheDepth[CacheSlots];

    /// <summary>Set while the first-person arm draws.</summary>
    public static bool InArm;

    /// <summary>Set while func_80032588 submits a model. A blended packet of a door
    /// is solid (<c>GtePacketDepth.Rec.Solid</c>, <c>ModelWalk.SolidKind</c>); every
    /// other blended model, the water and the glows included, is not.</summary>
    public static bool InModel;

    /// <summary>The authored material of the tile half being assembled, or 0; set by
    /// <see cref="TileWalk"/> around each half (docs/REMASTER.md).</summary>
    public static byte TileMaterial;

    /// <summary>Blended model packets by the table they came from (ModelKind).</summary>
    public static readonly long[] BlendedByKind = new long[4];

    /// <summary>Blended packets recorded as solid.</summary>
    public static long SolidPackets;

    /// <summary>Clipped fans whose packet did not carry its records' screen words.</summary>
    public static long DepthClipMismatches;

    /// <summary>Corners kept unrounded, and corners left at the game's whole Z.</summary>
    public static long DepthUnrounded, DepthWhole;

    static bool DepthOn() => GtePacketDepth.Active && !InHud && !InArm;

    /// <summary>The transforms, after both cache words are written; <paramref name="src"/>
    /// is the SVECTOR RotTransPers just projected.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void NoteDepth(PSMemory mem, uint dst, uint sxy, uint src)
    {
        uint i = (dst - VertexCache) >> 3;
        if (i >= CacheSlots) return;
        ref var e = ref _cacheDepth[i];
        e.W0 = sxy;
        e.W1 = Peek32(mem, dst + 4u);
        e.Z = Unrounded(mem, src, (int)Gte.Read(19));
    }

    /// <summary>
    /// The view Z of an SVECTOR under the GTE's current rotation and translation, before
    /// RotTrans drops its low twelve bits. Two panels in one plane under different tile
    /// translations round their corners differently, and that is enough to make them
    /// fight. <paramref name="whole"/> is the rounded Z the game computed; anything more
    /// than a unit and a half from it means the matrix has moved, and it is kept instead.
    /// </summary>
    static float Unrounded(PSMemory mem, uint v, int whole)
    {
        if (whole <= 0) return 0f;
        uint w0 = Peek32(mem, v), w1 = Peek32(mem, v + 4u);
        uint c3 = Gte.ReadControl(3);
        long m = (long)(short)c3 * (short)w0 + (long)(short)(c3 >> 16) * (short)(w0 >> 16)
               + (long)(short)Gte.ReadControl(4) * (short)w1;
        float z = (float)((int)Gte.ReadControl(7) + m / 4096.0);
        if (MathF.Abs(z - whole) <= 1.5f) { DepthUnrounded++; return z; }
        DepthWhole++;
        return whole;
    }

    /// <summary>A cached vertex's depth, if the cache still holds what our transform wrote.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static float CacheZ(PSMemory mem, uint p)
    {
        uint i = (p - VertexCache) >> 3;
        if (i >= CacheSlots) return 0f;
        ref var e = ref _cacheDepth[i];
        return e.W0 == Peek32(mem, p) && e.W1 == Peek32(mem, p + 4u) ? e.Z : 0f;
    }

    /// <summary>A packet built from cached vertices; <paramref name="last"/> is the
    /// offset of its last vertex word.</summary>
    static void DepthFace(PSMemory mem, uint pkt, uint last, int n, uint p0, uint p1, uint p2, uint p3)
    {
        ref var r = ref GtePacketDepth.Slot(pkt);
        r.Z0 = CacheZ(mem, p0);
        r.Z1 = CacheZ(mem, p1);
        r.Z2 = CacheZ(mem, p2);
        r.Z3 = n == 4 ? CacheZ(mem, p3) : 0f;
        if (r.Z0 <= 0f || r.Z1 <= 0f || r.Z2 <= 0f || (n == 4 && r.Z3 <= 0f)) { r.Cmd = 0; return; }
        SealDepth(mem, ref r, pkt, last);
    }

    /// <summary>A finished packet: recorded, or its address's old record dropped.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void RecordDepth(ref Frame fr, uint pkt, uint last, int n, uint p0, uint p1, uint p2, uint p3)
    {
        if (fr.Depth) DepthFace(fr.Mem, pkt, last, n, p0, p1, p2, p3);
        else if (fr.DepthTable) NoDepth(pkt);
    }

    /// <summary>Drop whatever record a packet at this address had.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void NoDepth(uint pkt) => GtePacketDepth.Slot(pkt).Cmd = 0;

    static void SealDepth(PSMemory mem, ref GtePacketDepth.Rec r, uint pkt, uint last)
    {
        r.Cmd = Peek32(mem, pkt + 4u);
        r.Xy0 = Peek32(mem, pkt + 8u);
        r.XyLast = Peek32(mem, pkt + last);
        // Nothing more specific named it: the art it draws may have a material.
        r.Material = TileMaterial != 0 || !Remaster.Surfaces.ByTexture ? TileMaterial
                   : Remaster.Surfaces.PacketMaterial(mem, pkt, r.Cmd);
        if (r.Material != 0) Remaster.Surfaces.Packets++;
        if (Remaster.Faces.Recording) Remaster.Faces.Seal(mem, pkt, last, r);
        // Bit 25 of the command word: semi-transparent.
        r.Solid = false;
        if (InModel && (r.Cmd & (1u << 25)) != 0)
        {
            var kind = ModelWalk.SubmitKind;
            BlendedByKind[(int)kind]++;
            r.Solid = kind == ModelKind.Object
                   && ModelWalk.SolidKind(ModelWalk.ObjectKind(mem, ModelWalk.SubmitRecord));
            if (r.Solid) SolidPackets++;
        }
        GtePacketDepth.Recorded++;
    }

    /// <summary>After func_800302E8 and anything that rewrote its colours: packet k is
    /// records 0, k+1 and k+2, each at its view-space Z (+0x10), which is the SZ3 the
    /// clipper projected it with, unrounded from the record's SVECTOR.</summary>
    static void DepthClipped(PSMemory mem, uint before, bool record)
    {
        uint count = ClippedCount(mem, before);
        if (count == 0) return;

        uint r0 = Peek32(mem, ClipOut);
        for (uint k = 0; k < count; k++)
        {
            uint pkt = before + k * 0x28u;
            if (!record) { NoDepth(pkt); continue; }
            uint ra = Peek32(mem, ClipOut + 4u * (k + 1)), rb = Peek32(mem, ClipOut + 4u * (k + 2));
            ref var r = ref GtePacketDepth.Slot(pkt);
            if (Peek32(mem, pkt + 0x08u) != Peek32(mem, r0 + 0x18u) || Peek32(mem, pkt + 0x14u) != Peek32(mem, ra + 0x18u)
                || Peek32(mem, pkt + 0x20u) != Peek32(mem, rb + 0x18u))
            {
                DepthClipMismatches++;
                r.Cmd = 0;
                continue;
            }
            r.Z0 = Unrounded(mem, r0, (int)Peek32(mem, r0 + 0x10u));
            r.Z1 = Unrounded(mem, ra, (int)Peek32(mem, ra + 0x10u));
            r.Z2 = Unrounded(mem, rb, (int)Peek32(mem, rb + 0x10u));
            if (r.Z0 <= 0f || r.Z1 <= 0f || r.Z2 <= 0f) { r.Cmd = 0; continue; }
            if (Remaster.Faces.Recording) r.Z3 = 0f;
            SealDepth(mem, ref r, pkt, 0x20u);
        }
    }
}
