using System.Runtime.CompilerServices;

namespace RecompOne.Runtime;

/// <summary>
/// 0048. What a primitive's vertex colours were made from, so the fragment shader can
/// make them again at every pixel instead of interpolating the three or four the GTE
/// produced.
///
/// A packet's colour is the end of a short chain: a lit colour (a face's
/// <c>NormalColorCol</c>, or a normal through <c>LLM</c>, <c>LCM</c> and <c>BK</c>),
/// then the depth cue, whose weight is a clamped and curved function of the view
/// depth. Interpolating the result across a polygon is only right where none of the
/// clamps, the curve's knee or the terminator falls inside it. The port that builds
/// the packet records the chain's inputs here, keyed by the packet's address; the GPU
/// finds them by the address it read the command word from, and checks the command
/// word and the first vertex word before believing them.
///
/// Per vertex: a lit colour (or, for <see cref="Directional"/>, the three light dot
/// products before their clamp) and the raw depth-cue value, <c>MAC0 / 4096</c>,
/// which is affine in 1/z and so exact under screen-space interpolation. Per
/// primitive: the curve and the light colour. Per batch: <c>BK</c> and <c>LCM</c>,
/// which the game sets per area, as uniforms.
///
/// Nothing here writes guest memory or the GTE. A miss draws the packet's own
/// colours, as before.
/// </summary>
public static class GteLightMap
{
    /// <summary>Mode byte, in the top byte of <see cref="Rec.Light"/>: set on every record.</summary>
    public const uint Present = 0x40;

    /// <summary>Mode byte: per-vertex light dots and a light colour rather than a lit colour.</summary>
    public const uint Directional = 0x80;

    /// <summary>Depth-cue curves, the low three bits of the mode byte.</summary>
    public const uint CurveNone = 0, CurveOffset = 1, CurveKnee = 2, CurveHalf = 3, CurveWord = 4;

    public struct Rec
    {
        // The packet's command and first vertex words once it was finished.
        public uint Cmd, Xy0;
        // Mode byte on top, the light colour below.
        public uint Light;
        public int Gen;
        public float L0x, L0y, L0z, F0;
        public float L1x, L1y, L1z, F1;
        public float L2x, L2y, L2z, F2;
        public float L3x, L3y, L3z, F3;
    }

    /// <summary>The port's switch.</summary>
    public static bool Enabled;

    /// <summary>The backend can draw it: the core-profile GL shaders.</summary>
    public static bool Supported;

    public static bool Active => Enabled && Supported;

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
        Recorded++;
        return ref _recs[i];
    }

    /// <summary>The record for the packet whose command word the GPU read from
    /// <paramref name="cmdSrc"/>, if it still describes these words.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static ref readonly Rec Find(uint cmdSrc, uint cmd, uint xy0, out bool found)
    {
        found = false;
        uint i = ((cmdSrc & 0x1FFFFFFFu) - 4u - _base) >> 2;
        if (_recs == null || cmdSrc == 0 || i >= _count) { Misses++; return ref _discard; }
        ref readonly var r = ref _recs[i];
        if (r.Light == 0 || r.Cmd != cmd || r.Xy0 != xy0) { Misses++; return ref _discard; }
        found = true;
        Hits++;
        return ref r;
    }

    // BK and LCM by generation, so a batch uploads the pair its primitives were built
    // with. Every model sets its own from its tile's light record, and the planar walk
    // replays them, so a frame starts several; one that started more than the ring holds
    // lit its first models with a later model's BK (0048 amended).
    const int GenRing = 64;
    static readonly int[] _bk = new int[GenRing * 3];
    static readonly short[] _lcm = new short[GenRing * 9];
    static int _gen;

    /// <summary>The generation of the current BK and LCM, starting a new one if they changed.</summary>
    public static int NoteConstants()
    {
        int o3 = (_gen & (GenRing - 1)) * 3, o9 = (_gen & (GenRing - 1)) * 9;
        bool same = true;
        for (int i = 0; i < 3 && same; i++) same = _bk[o3 + i] == (int)Gte.ReadControl(13 + i);
        for (int i = 0; i < 9 && same; i++) same = _lcm[o9 + i] == Lcm(i);
        if (same && _gen != 0) return _gen;

        _gen++;
        Generations++;
        o3 = (_gen & (GenRing - 1)) * 3;
        o9 = (_gen & (GenRing - 1)) * 9;
        for (int i = 0; i < 3; i++) _bk[o3 + i] = (int)Gte.ReadControl(13 + i);
        for (int i = 0; i < 9; i++) _lcm[o9 + i] = Lcm(i);
        return _gen;
    }

    static short Lcm(int i) =>
        i == 8 ? (short)Gte.ReadControl(20) : (short)(Gte.ReadControl(16 + i / 2) >> ((i & 1) * 16));

    /// <summary>Generations started; never reset.</summary>
    public static long Generations;

    public static int Bk(int gen, int c) => _bk[(gen & (GenRing - 1)) * 3 + c];
    public static short LcmAt(int gen, int i) => _lcm[(gen & (GenRing - 1)) * 9 + i];

    /// <summary>Records written, and primitives that found theirs or did not.</summary>
    public static long Recorded, Hits, Misses;

    public static void ResetCounters() => Recorded = Hits = Misses = 0;
}
