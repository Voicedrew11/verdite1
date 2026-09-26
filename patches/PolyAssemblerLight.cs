using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using RecompOne.Runtime;
using RecompOne.Runtime.Memory;

namespace Kf2;

/// <summary>
/// What each packet's colours were made from, recorded for per-pixel lighting
/// (<see cref="PerPixelLighting"/>, <c>GteLightMap</c>). Reads only: nothing here
/// writes guest memory or the GTE, so the verify mode sees the same routine.
/// See "Per-pixel lighting" in docs/RENDERING.md.
/// </summary>
public static partial class PolyAssembler
{
    // The raw depth cue per vertex-cache slot, beside the cache itself.
    struct CacheLight
    {
        public uint Sxy, Serial;
        public ushort Fog;
        public byte Curve;
        public float Raw;
    }

    // Bumped by the tile assemblers before their own transform, so a slot that
    // transform wrote is known fresh without reading the cache back.
    static uint _cacheSerial;

    const int CacheSlots = 1 << 13;
    static readonly CacheLight[] _cacheLight = new CacheLight[CacheSlots];

    // The same for the clipper's records, which the clipped emitter fogs at IR0 / 2.
    struct RecordLight
    {
        public uint Sxy, Ir0;
        public float Raw;
    }

    const int RecordSlots = 64;
    static readonly RecordLight[] _recordLight = new RecordLight[RecordSlots];

    /// <summary>Authored lights or a glowing material are applied: a face drawn unfogged
    /// keeps its record, since both terms are evaluated from it (0071).</summary>
    public static bool KeepUnfogged;

    /// <summary>The two reasons, each owned by the remaster feature that has it.</summary>
    public static bool KeepForLights, KeepForGlow;

    public static void Keep() => KeepUnfogged = KeepForLights || KeepForGlow;

    /// <summary>The area's fog has a colour or a curve (0074): a face fogged all the way
    /// to black keeps its record, since it is drawn in the fog's colour, or not all the
    /// way.</summary>
    public static bool KeepFogged;

    /// <summary>Set while the HUD builder runs, whose icons go through the lit assembler
    /// too and are left to draw their own colours.</summary>
    public static bool InHud;

    /// <summary>Per-pixel lighting is on and the far colour is black, which is the only
    /// far colour the shader draws. The game never sets another.</summary>
    static bool LightingOn() =>
        GteLightMap.Active && !InHud && Gte.ReadControl(21) == 0 && Gte.ReadControl(22) == 0 && Gte.ReadControl(23) == 0;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static uint Peek32(PSMemory mem, uint a)
    {
        uint o = a & 0x1FFFFFFFu;
        var ram = mem.Ram;
        if (o + 4u > (uint)ram.Length) return 0u;
        return Unsafe.ReadUnaligned<uint>(ref Unsafe.Add(ref MemoryMarshal.GetReference(ram), (nint)o));
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static short Peek16(PSMemory mem, uint a) => (short)Peek32(mem, a);

    /// <summary>The transforms: MAC0 is the depth cue before its clamp.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void NoteCache(uint dst, uint sxy, int fog, uint curve, bool word = false)
    {
        uint i = (dst - VertexCache) >> 3;
        if (i >= CacheSlots) return;
        ref var e = ref _cacheLight[i];
        e.Sxy = sxy;
        e.Serial = _cacheSerial;
        e.Fog = (ushort)fog;
        e.Curve = (byte)curve;
        e.Raw = word ? fog : (int)Gte.Read(24) / 4096f;
    }

    /// <summary>A tile assembler's vertex: written by the transform it just called, or not at all.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static bool TileFog(uint p, out float raw, out uint curve)
    {
        uint i = (p - VertexCache) >> 3;
        raw = 0f;
        curve = 0;
        if (i >= CacheSlots) return false;
        ref var e = ref _cacheLight[i];
        if (e.Serial != _cacheSerial) return false;
        raw = e.Raw;
        curve = e.Curve;
        return true;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static bool CacheFog(PSMemory mem, uint p, out float raw, out uint curve)
    {
        uint i = (p - VertexCache) >> 3;
        raw = 0f;
        curve = 0;
        if (i >= CacheSlots) { if (PerPixelLighting.Check) PerPixelLighting.Fallback[0]++; return false; }
        ref var e = ref _cacheLight[i];
        if (e.Sxy != Peek32(mem, p)) { if (PerPixelLighting.Check) PerPixelLighting.Fallback[1]++; return false; }
        if (e.Fog != (ushort)Peek16(mem, p + 6u)) { if (PerPixelLighting.Check) PerPixelLighting.Fallback[2]++; return false; }
        raw = e.Raw;
        curve = e.Curve;
        return true;
    }

    /// <summary>Each vertex's raw depth cue and the curve they share; the cache's fog
    /// words, taken as they are, when the transform that wrote them was not ours.</summary>
    static uint Fogs(PSMemory mem, ref GteLightMap.Rec r, int n, uint p0, uint p1, uint p2, uint p3)
    {
        bool ok = CacheFog(mem, p0, out r.F0, out uint c0);
        ok = ok && CacheFog(mem, p1, out r.F1, out uint c1) && c1 == c0;
        ok = ok && CacheFog(mem, p2, out r.F2, out uint c2) && c2 == c0;
        if (n == 4) ok = ok && CacheFog(mem, p3, out r.F3, out uint c3) && c3 == c0;
        return ok ? c0 : WordFogs(mem, ref r, n, p0, p1, p2, p3);
    }

    /// <summary>The same for a tile, whose cache its own assembler's transform just filled.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static uint TileFogs(PSMemory mem, ref GteLightMap.Rec r, int n, uint p0, uint p1, uint p2, uint p3)
    {
        bool ok = TileFog(p0, out r.F0, out uint c0);
        ok = ok && TileFog(p1, out r.F1, out uint c1) && c1 == c0;
        ok = ok && TileFog(p2, out r.F2, out uint c2) && c2 == c0;
        if (n == 4) ok = ok && TileFog(p3, out r.F3, out uint c3) && c3 == c0;
        return ok ? c0 : WordFogs(mem, ref r, n, p0, p1, p2, p3);
    }

    static uint WordFogs(PSMemory mem, ref GteLightMap.Rec r, int n, uint p0, uint p1, uint p2, uint p3)
    {
        if (PerPixelLighting.Check) PerPixelLighting.Fallback[3]++;

        r.F0 = Peek16(mem, p0 + 6u);
        r.F1 = Peek16(mem, p1 + 6u);
        r.F2 = Peek16(mem, p2 + 6u);
        if (n == 4) r.F3 = Peek16(mem, p3 + 6u);
        return GteLightMap.CurveWord;
    }

    /// <summary>
    /// A face whose every corner is unfogged, or fogged all the way to black, comes out
    /// the same interpolated as per pixel, so it is not recorded: nothing to look up,
    /// and a batch of nothing else never uploads the light buffer or runs the lighting.
    /// The area's fog keeps the second (<see cref="KeepFogged"/>).
    /// </summary>
    static bool Uniform(ref GteLightMap.Rec r, uint curve, int n)
    {
        float lo = MathF.Min(MathF.Min(r.F0, r.F1), r.F2), hi = MathF.Max(MathF.Max(r.F0, r.F1), r.F2);
        if (n == 4) { lo = MathF.Min(lo, r.F3); hi = MathF.Max(hi, r.F3); }
        float black = curve switch
        {
            GteLightMap.CurveNone => float.NegativeInfinity,
            GteLightMap.CurveOffset => 2848f,
            GteLightMap.CurveKnee => 3232f,
            GteLightMap.CurveWord => 4096f,
            _ => float.PositiveInfinity,
        };
        return (hi <= 0f && !KeepUnfogged) || (lo >= black && !KeepFogged);
    }

    static void Seal(PSMemory mem, ref GteLightMap.Rec r, uint pkt, uint mode, uint rgb)
    {
        r.Light = ((GteLightMap.Present | mode) << 24) | (rgb & 0xFFFFFFu);
        r.Cmd = Peek32(mem, pkt + 4u);
        r.Xy0 = Peek32(mem, pkt + 8u);
        if (PerPixelLighting.Check) CheckCorners(mem, in r, pkt);
    }

    /// <summary>KF2_PERPIXEL_PROBE=2: the shader's colour at each corner of a gouraud
    /// packet against the colour the GTE wrote there. A flat model face was fogged
    /// once at its mean, so it is not expected to agree and is only counted; a gouraud
    /// one at its first corner's weight, so every corner is checked at that one.</summary>
    static void CheckCorners(PSMemory mem, in GteLightMap.Rec r, uint pkt)
    {
        uint code = (r.Cmd >> 24) & 0xFCu;
        if (code != 0x34u && code != 0x3Cu) { PerPixelLighting.CheckFlat++; return; }
        int n = code == 0x3Cu ? 4 : 3;
        for (int i = 0; i < n; i++)
        {
            uint word = Peek32(mem, pkt + 4u + 0xCu * (uint)i);
            float lx = i switch { 0 => r.L0x, 1 => r.L1x, 2 => r.L2x, _ => r.L3x };
            float ly = i switch { 0 => r.L0y, 1 => r.L1y, 2 => r.L2y, _ => r.L3y };
            float lz = i switch { 0 => r.L0z, 1 => r.L1z, 2 => r.L2z, _ => r.L3z };
            float fog = (r.Light & (GteLightMap.Directional << 24)) != 0 ? r.F0
                      : i switch { 0 => r.F0, 1 => r.F1, 2 => r.F2, _ => r.F3 };
            int err = 0;
            for (int ch = 0; ch < 3; ch++)
            {
                int want = (int)(word >> (8 * ch)) & 0xFF;
                int got = ShadeCorner(r.Light, r.Gen, ch == 0 ? lx : ch == 1 ? ly : lz, lx, ly, lz, fog, ch);
                err = Math.Max(err, Math.Abs(got - want));
            }
            PerPixelLighting.NoteCorner(err, r.Light >> 24);
        }
    }

    /// <summary>The prim shader's shade8(), one channel.</summary>
    static int ShadeCorner(uint lightWord, int gen, float lit, float ax, float ay, float az, float fog, int ch)
    {
        uint mode = lightWord >> 24;
        if ((mode & GteLightMap.Directional) != 0)
        {
            float a0 = Math.Clamp(ax, 0f, 32767f), a1 = Math.Clamp(ay, 0f, 32767f), a2 = Math.Clamp(az, 0f, 32767f);
            float ir = GteLightMap.Bk(gen, ch) + (GteLightMap.LcmAt(gen, ch * 3) * a0 + GteLightMap.LcmAt(gen, ch * 3 + 1) * a1
                                                 + GteLightMap.LcmAt(gen, ch * 3 + 2) * a2) / 4096f;
            lit = ((lightWord >> (8 * ch)) & 0xFF) * Math.Clamp(ir, 0f, 32767f) / 4096f;
        }
        float ir0 = Math.Clamp(fog, 0f, 4096f);
        float w = (mode & 7u) switch
        {
            1 => Math.Max(ir0 - 800f, 0f) * 2f,
            2 => ir0 < 2800f ? ir0 : 3f * ir0 - 5600f,
            3 => ir0 * 0.5f,
            4 => fog,
            _ => 0f,
        };
        return (int)Math.Clamp(MathF.Floor(lit * (1f - w / 4096f)), 0f, 255f);
    }

    /// <summary>A map tile: a lit colour per corner (one for the face unless its light
    /// is blended), fogged per vertex. The record keeps the colour it was lit with, which
    /// only an authored light reads (0071).</summary>
    static void LightTile(PSMemory mem, uint pkt, uint rgbc, uint c0, uint c1, uint c2, uint c3, int n,
                          uint p0, uint p1, uint p2, uint p3)
    {
        ref var r = ref GteLightMap.Slot(pkt);
        Corner(c0, out r.L0x, out r.L0y, out r.L0z);
        Corner(c1, out r.L1x, out r.L1y, out r.L1z);
        Corner(c2, out r.L2x, out r.L2y, out r.L2z);
        Corner(c3, out r.L3x, out r.L3y, out r.L3z);
        uint curve = TileFogs(mem, ref r, n, p0, p1, p2, p3);
        if (Uniform(ref r, curve, n)) { r.Light = 0; return; }
        Seal(mem, ref r, pkt, curve, rgbc);
    }

    /// <summary>A flat model face: the NormalColorDpq colour before its depth cue and
    /// before it saturates.</summary>
    static void LightFlat(PSMemory mem, uint pkt, uint light, uint normal, int n, uint p0, uint p1, uint p2, uint p3)
    {
        ref var r = ref GteLightMap.Slot(pkt);
        Gte.LightProducts(Peek16(mem, normal), Peek16(mem, normal + 2u), Peek16(mem, normal + 4u),
                          out int i1, out int i2, out int i3);
        float cr = (light & 0xFF) * i1 / 4096f, cg = ((light >> 8) & 0xFF) * i2 / 4096f, cb = ((light >> 16) & 0xFF) * i3 / 4096f;
        r.L0x = r.L1x = r.L2x = r.L3x = cr;
        r.L0y = r.L1y = r.L2y = r.L3y = cg;
        r.L0z = r.L1z = r.L2z = r.L3z = cb;
        uint curve = Fogs(mem, ref r, n, p0, p1, p2, p3);
        if (Uniform(ref r, curve, n)) { r.Light = 0; return; }
        Seal(mem, ref r, pkt, curve, light);
    }

    /// <summary>A gouraud model face: the light dots of each vertex's normal, lit in
    /// the shader with the light colour and this generation's BK and LCM.</summary>
    static void LightGouraud(PSMemory mem, uint pkt, uint light, int gen, int n,
                             uint n0, uint n1, uint n2, uint n3, uint p0, uint p1, uint p2, uint p3)
    {
        ref var r = ref GteLightMap.Slot(pkt);
        Gte.LightDots(Peek16(mem, n0), Peek16(mem, n0 + 2u), Peek16(mem, n0 + 4u), out r.L0x, out r.L0y, out r.L0z);
        Gte.LightDots(Peek16(mem, n1), Peek16(mem, n1 + 2u), Peek16(mem, n1 + 4u), out r.L1x, out r.L1y, out r.L1z);
        Gte.LightDots(Peek16(mem, n2), Peek16(mem, n2 + 2u), Peek16(mem, n2 + 4u), out r.L2x, out r.L2y, out r.L2z);
        if (n == 4)
            Gte.LightDots(Peek16(mem, n3), Peek16(mem, n3 + 2u), Peek16(mem, n3 + 4u), out r.L3x, out r.L3y, out r.L3z);
        r.Gen = gen;
        Seal(mem, ref r, pkt, GteLightMap.Directional | Fogs(mem, ref r, n, p0, p1, p2, p3), light);
    }

    /// <summary>Survivors: a clipper record's raw depth cue.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void NoteRecord(PSMemory mem, uint rec, uint sxy, uint ir0)
    {
        uint i = (rec - Peek32(mem, ClipRecords)) / 0x2Cu;
        if (i >= RecordSlots) return;
        ref var e = ref _recordLight[i];
        e.Sxy = sxy;
        e.Ir0 = ir0;
        e.Raw = (int)Gte.Read(24) / 4096f;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static bool RecordFog(PSMemory mem, uint rec, out float raw)
    {
        uint i = (rec - Peek32(mem, ClipRecords)) / 0x2Cu;
        raw = 0f;
        if (i >= RecordSlots) return false;
        ref var e = ref _recordLight[i];
        if (e.Sxy != Peek32(mem, rec + 0x18u) || e.Ir0 != Peek32(mem, rec + 0x14u)) return false;
        raw = e.Raw;
        return true;
    }

    /// <summary>
    /// After func_800302E8: the fan it emitted, packet k being records 0, k+1 and k+2,
    /// lit by NormalColorCol and fogged at IR0 / 2, or on the near curve when
    /// <paramref name="refogged"/>. <paramref name="before"/> is the buffer cursor
    /// before the call; a packet past the buffer's end was allocated and never filled
    /// or linked, so it is left alone.
    /// </summary>
    static void LightClipped(PSMemory mem, uint before, uint normal, bool refogged)
    {
        uint count = ClippedCount(mem, before);
        if (count == 0) return;

        uint colour = ClippedColour(mem, normal);
        uint rgbc = Peek32(mem, LightColour);
        bool far = (int)Peek32(mem, FogMode) >= 32000;
        uint fogCurve = !refogged ? GteLightMap.CurveHalf : far ? GteLightMap.CurveNone : GteLightMap.CurveKnee;

        uint r0 = Peek32(mem, ClipOut);
        for (uint k = 0; k < count; k++)
        {
            uint pkt = before + k * 0x28u;
            uint ra = Peek32(mem, ClipOut + 4u * (k + 1)), rb = Peek32(mem, ClipOut + 4u * (k + 2));
            ref var r = ref GteLightMap.Slot(pkt);
            Corner(RecordColour(mem, r0, normal, colour), out r.L0x, out r.L0y, out r.L0z);
            Corner(RecordColour(mem, ra, normal, colour), out r.L1x, out r.L1y, out r.L1z);
            Corner(RecordColour(mem, rb, normal, colour), out r.L2x, out r.L2y, out r.L2z);
            uint curve = fogCurve;
            bool blended = false;
            if (refogged && _tileFog)
            {
                r.F0 = RecordFogWeight(mem, r0, far, out bool b0);
                r.F1 = RecordFogWeight(mem, ra, far, out bool b1);
                r.F2 = RecordFogWeight(mem, rb, far, out bool b2);
                blended = b0 | b1 | b2;
            }
            if (blended) curve = GteLightMap.CurveWord;
            else if (!(RecordFog(mem, r0, out r.F0) & RecordFog(mem, ra, out r.F1) & RecordFog(mem, rb, out r.F2)))
            {
                r.F0 = WordFog(mem, r0, refogged, far);
                r.F1 = WordFog(mem, ra, refogged, far);
                r.F2 = WordFog(mem, rb, refogged, far);
                curve = GteLightMap.CurveWord;
            }
            if (Uniform(ref r, curve, 3)) { r.Light = 0; continue; }
            Seal(mem, ref r, pkt, curve, rgbc);
        }
    }

    static void Corner(uint colour, out float r, out float g, out float b)
    {
        r = colour & 0xFF;
        g = (colour >> 8) & 0xFF;
        b = (colour >> 16) & 0xFF;
    }

    /// <summary>A record's fog weight as the emitter's DPCS saw it.</summary>
    static float WordFog(PSMemory mem, uint rec, bool refogged, bool far)
    {
        uint p = Peek32(mem, rec + 0x14u);
        return refogged ? NearFog((int)p, far) : p >> 1;
    }

    /// <summary>NormalColorCol's saturated colour byte from the light colour and IR.</summary>
    static float Nccs(uint rgbc, int ir) => Math.Clamp((int)(((long)rgbc * ir << 4) >> 12) >> 4, 0, 255);
}
