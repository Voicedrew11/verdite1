namespace RecompOne.Runtime;

/// <summary>
/// 0058. The frame's depth-carrying triangles, kept per display target.
///
/// <para>The occlusion pass needs a normal at every pixel and has never had one: it
/// differences the depth buffer across four neighbours and takes a cross product,
/// which straddles a silhouette wherever two surfaces meet and is quantised by the
/// depth everywhere else. That was the only source available while the geometry was
/// something that went past — GP0 words arriving in a stream nothing held.</para>
///
/// <para>It is held now. The port assembles the polygons itself
/// (<c>patches/PolyAssembler.cs</c>) and enumerates the scene
/// (<c>patches/TileWalk.cs</c>, <c>patches/ModelWalk.cs</c>), so the frame's geometry
/// can simply be kept as it is submitted and drawn again into a normal buffer once
/// the frame is finished. A triangle's normal is then the plane's own, exact and
/// constant across the face, rather than a guess made from its neighbours' depths.</para>
///
/// <para>The list is <b>per render target</b> and not per frame, because with two
/// display buffers the target being presented is the one drawn a frame ago — the same
/// reason the depth attachment lives on the target. It is dropped the first time a
/// target is drawn in a new frame, which is the condition its depth is cleared on.</para>
///
/// <para>Order is the whole of the correctness argument. The triangles are kept in
/// submission order and redrawn with no depth test, so the last normal written at a
/// pixel belongs to the last triangle drawn there — which under painter's order is
/// the same surface whose depth the pass reads. No test, no sort, and nothing to
/// disagree with the depth buffer about.</para>
/// </summary>
public sealed class AoGeometry
{
    /// <summary>A vertex as the colour pass drew it: the target's own coordinates
    /// and the view depth. The depth is also the W the redraw projects with, which
    /// is what makes the reconstructed surface the polygon's own plane.</summary>
    public struct V
    {
        public float X, Y, Z;
        // 0067. The surface's material (SurfaceMaterial); every opaque triangle is
        // at least Opaque, and a translucent one is kept only when it has one.
        public float M;
        // A veil's texel, for its semi-transparency bit: the UV, and the texpage
        // and CLUT with bit 31 set when it is textured.
        public float Tu, Tv;
        public uint Tex;
    }

    /// <summary>The port's switch.</summary>
    public static bool Enabled = true;

    /// <summary>Collecting costs nothing when nothing will read it. The reflection
    /// pass reads the surface buffer whatever the occlusion pass's normals are set to,
    /// and 0085's probes read it with both of those passes off.</summary>
    public static bool Active => (Enabled && GteDepth.AmbientOcclusion) || GteDepth.Reflections
                                 || RetainedScene.SurfaceCheck || RetainedScene.DepthStageProbe;

    /// <summary>Triangles kept, triangles refused for want of room, and the normal
    /// passes actually drawn. Never reset except by the probe.</summary>
    public static long Triangles, Dropped, Passes;

    public static void ResetCounters() => Triangles = Dropped = Passes = 0;

    // A view of fdat02's water is about 2,000 triangles; the ceiling is well past
    // anything measured and exists so a pathological frame drops geometry rather
    // than growing the buffer without bound.
    const int MaxVerts = 3 * 32768;

    V[] _v = new V[3 * 2048];
    int _n;
    long _frame = -1;
    int _gen = -1;

    public int Count => _n;

    public ReadOnlySpan<V> Verts => _v.AsSpan(0, _n);

    // Where the list turns from surfaces to veils and back (SurfaceMaterial.VeilHalf),
    // as vertex indices, starting with surfaces: the normal pass blends a veil.
    readonly List<int> _breaks = new();
    bool _inVeil;

    public List<int> Breaks => _breaks;

    /// <summary>0085. The retained scene's frame whose map the GPU drew into this
    /// target this frame (0 for none), and the projection centre it was drawn with,
    /// in the target's 1x pixels. The normal pass draws that map first.</summary>
    public int WorldSerial;
    public float WorldCx, WorldCy;

    /// <summary>0085. Where the map's water went in among the list's triangles: the
    /// vertex count when the colour pass drew it, and its slice of view depth.</summary>
    public readonly List<(int At, float Lo, float Hi)> Water = new();

    /// <summary>0085. Where the first-person arm went in among the list's triangles
    /// (-1 for nowhere), and the frame it was drawn from.</summary>
    public int ArmAt = -1, ArmSerial;

    /// <summary>Start this target's list over when it is first drawn in a new frame,
    /// or when the depth generation moved under it.</summary>
    public void Frame(long frame, int gen)
    {
        if (frame == _frame && gen == _gen) return;
        _frame = frame;
        _gen = gen;
        _n = 0;
        WorldSerial = 0;
        Water.Clear();
        ArmAt = -1;
        _breaks.Clear();
        _inVeil = false;
    }

    public void Add(in V a, in V b, in V c)
    {
        if (_n + 3 > MaxVerts) { Dropped++; return; }
        if (_n + 3 > _v.Length) Array.Resize(ref _v, Math.Min(MaxVerts, _v.Length * 2));
        bool veil = a.M >= SurfaceMaterial.VeilHalf;
        if (veil != _inVeil) { _breaks.Add(_n); _inVeil = veil; }
        _v[_n++] = a;
        _v[_n++] = b;
        _v[_n++] = c;
        Triangles++;
    }
}
