namespace RecompOne.Runtime;

/// <summary>
/// 0068. Planar reflections: the scene drawn a second time from a camera mirrored
/// in the water's plane, into a texture of the render target's own, which the
/// reflection pass reads in place of its screen-space march wherever the surface
/// lies on that plane.
///
/// <para>The runtime cannot draw the scene twice: the geometry arrives as GP0 words
/// and nothing here knows the camera. The port can, because it owns the walks that
/// enumerate the world (<c>patches/TileWalk.cs</c>, <c>patches/ModelWalk.cs</c>):
/// it re-runs them under the mirrored camera into an ordering table of its own and
/// hands that table to <c>LibGpu.DrawOTag</c> while <see cref="Capturing"/> is set.
/// Everything the backend does to a primitive is unchanged; only the target is
/// swapped for the current one's planar texture, and fragments on the camera's side
/// of the water are discarded.</para>
///
/// <para>A mirror is a reflection, and a reflection turns every winding round, so
/// the port does not mirror the world: it moves the camera. With the world's Y
/// flipped about the plane, the view matrix <c>R</c> becomes <c>M R M</c> -- the
/// same yaw, the pitch and roll negated -- composed with a flip of the camera's own
/// Y. The game renders the first half as an ordinary camera, so its culling, its
/// clipper and its winding tests stay right, and the flip is done where the pass
/// samples: the mirrored image of screen row <c>y</c> is row <c>2*OFY - y</c>.</para>
///
/// <para>The plane is found in the picture. Every triangle the reflection pass
/// classifies as water (<see cref="SurfaceMaterial"/>) is taken back to world space
/// with the camera the port published, and the heights are binned by screen area;
/// the port reads the heaviest bin once a frame, so the plane follows whatever
/// water is on screen, one frame late.</para>
/// </summary>
public static class PlanarReflections
{
    /// <summary>The port's switch. The reflection pass reads the planar texture,
    /// and runs for it whether or not the screen march is on.</summary>
    public static bool Enabled
    {
        get => _on;
        set { _on = value; ScreenReflections.Refresh(); }
    }
    static bool _on;

    /// <summary>Set by the GL core backend when its prim program has the clip plane;
    /// nothing else can draw into a planar texture, so the port walks nothing
    /// without it.</summary>
    public static bool Supported;

    /// <summary>Set by the port around its own <c>DrawOTag</c> of the mirrored
    /// table. Every primitive drawn meanwhile goes to the current target's planar
    /// texture.</summary>
    public static bool Capturing;

    /// <summary>Bumped by the port before each capture; the backend clears a
    /// target's planar texture the first time it sees a new one.</summary>
    public static int Serial;

    /// <summary>The water's plane in the mirrored camera's view space, as a
    /// fragment test: a view position <c>p</c> is kept while
    /// <c>dot(xyz, p) + w &gt;= 0</c>, which is above the water.</summary>
    public static readonly float[] ClipPlane = new float[4];

    /// <summary>The same plane in the real camera's view space, as a signed
    /// distance, for the reflection pass to tell which surfaces lie on it.</summary>
    public static readonly float[] ViewPlane = new float[4];

    /// <summary>The view's forward with its world height taken out, in the
    /// mirrored camera's view space. The game fogs by view depth and culls its
    /// map by a level cone, so the cone's far edge is black only while the
    /// camera looks level; from a mirrored camera looking up it reflected lit.
    /// A capture fogs a fragment at the larger of its own depth and its depth
    /// along this axis. Zero leaves the game's fog alone.</summary>
    public static readonly float[] LevelAxis = new float[3];

    /// <summary>The switch for <see cref="LevelAxis"/>: <c>KF2_PLANAR_FOG=0</c> is the comparison.</summary>
    public static bool LevelFog = true;

    /// <summary>How far a surface may lie off the plane, in world units, and still
    /// take the planar reflection rather than the march.</summary>
    public static float Tolerance = 48f;

    /// <summary>How far the water's own texture bends the reflection, in the game's
    /// pixels per unit of brightness change across a pixel; 0 is a flat mirror.</summary>
    public static float Ripple = 4f;

    /// <summary>The probe's switch.</summary>
    public static bool Probe;

    /// <summary>Captures started, targets cleared for one, primitives refused for
    /// want of a target, and reflection passes that read a planar texture.</summary>
    public static long Captures, Cleared, Dropped, Read;

    public static void ResetCounters() => Captures = Cleared = Dropped = Read = 0;

    // ---- the real camera, for finding the water ------------------------------

    // R transposed (world axes from a view position) and the camera's position.
    static float _rx0, _rx1, _rx2, _ry0, _ry1, _ry2, _rz0, _rz1, _rz2, _camX, _camY, _camZ;
    static bool _camera;

    /// <summary>The camera the frame is being drawn with: <paramref name="r"/> is
    /// the GTE rotation, world to view, row-major at the GTE's 4096 scale, and
    /// the rest its world position. Published by the port before the frame's
    /// <c>DrawOTag</c>.</summary>
    public static void SetCamera(ReadOnlySpan<short> r, float camX, float camY, float camZ)
    {
        _rx0 = r[0] / 4096f; _rx1 = r[3] / 4096f; _rx2 = r[6] / 4096f;
        _ry0 = r[1] / 4096f; _ry1 = r[4] / 4096f; _ry2 = r[7] / 4096f;
        _rz0 = r[2] / 4096f; _rz1 = r[5] / 4096f; _rz2 = r[8] / 4096f;
        _camX = camX; _camY = camY; _camZ = camZ;
        _camera = true;
    }

    /// <summary>Where the water rests at a world X and Z, given the height it was
    /// drawn at, or NaN for water the port does not move. Set by a port that moves
    /// the water's vertices itself (a swell): a moved triangle is neither level nor
    /// at the height to mirror in, and the rest height is both.</summary>
    public static Func<float, float, float, float>? RestHeight;

    public static void ClearCamera() => _camera = false;

    const int Bins = 16;
    // A bin is a 16-unit band of world Y: its key, the area in it, and the
    // area-weighted sum of the heights, so the plane is their mean rather than the
    // band's edge.
    static readonly int[] _key = new int[Bins];
    static readonly double[] _weight = new double[Bins], _sum = new double[Bins];
    static int _bins;

    /// <summary>Water triangles binned, those refused as not level, and those binned
    /// at the height <see cref="RestHeight"/> gave.</summary>
    public static long WaterTris, WaterTilted, WaterRested;

    /// <summary>One water triangle, as the backend drew it: target coordinates
    /// relative to the GTE's centre, the view depth, and the picture's own
    /// rectangle in the same coordinates. The walks submit water past the edge of
    /// the picture as well, and a pool nobody can see is no reason to walk the
    /// world twice, so only what lands inside the rectangle counts.</summary>
    public static void NoteWater(float x0, float y0, float z0, float x1, float y1, float z1,
                                 float x2, float y2, float z2,
                                 float left, float top, float right, float bottom)
    {
        if (!_camera || z0 <= 0f || z1 <= 0f || z2 <= 0f) return;
        if (Math.Max(x0, Math.Max(x1, x2)) < left || Math.Min(x0, Math.Min(x1, x2)) > right
            || Math.Max(y0, Math.Max(y1, y2)) < top || Math.Min(y0, Math.Min(y1, y2)) > bottom)
            return;
        float h = Math.Max(1f, GteDepth.ProjH);
        float w0 = WorldY(x0, y0, z0, h), w1 = WorldY(x1, y1, z1, h), w2 = WorldY(x2, y2, z2, h);
        float lo = Math.Min(w0, Math.Min(w1, w2)), hi = Math.Max(w0, Math.Max(w1, w2));
        float y = (w0 + w1 + w2) / 3f;
        float rest = float.NaN;
        if (RestHeight is { } restAt)
        {
            float sx = (x0 + x1 + x2) / 3f, sy = (y0 + y1 + y2) / 3f, sz = (z0 + z1 + z2) / 3f;
            rest = restAt(_rx0 * sx * sz / h + _rx1 * sy * sz / h + _rx2 * sz + _camX,
                          _rz0 * sx * sz / h + _rz1 * sy * sz / h + _rz2 * sz + _camZ, y);
        }
        if (!float.IsNaN(rest)) { y = rest; WaterRested++; }
        // A waterfall is water too, and has no plane to mirror in.
        else if (hi - lo > 64f) { WaterTilted++; return; }
        // The area on screen, near enough for a weight: the corners pulled into
        // the rectangle.
        float cx0 = Math.Clamp(x0, left, right), cy0 = Math.Clamp(y0, top, bottom);
        float cx1 = Math.Clamp(x1, left, right), cy1 = Math.Clamp(y1, top, bottom);
        float cx2 = Math.Clamp(x2, left, right), cy2 = Math.Clamp(y2, top, bottom);
        double area = Math.Abs((cx1 - cx0) * (cy2 - cy0) - (cy1 - cy0) * (cx2 - cx0)) * 0.5;
        if (area <= 0.0) return;
        int key = (int)MathF.Round(y / 16f);
        int i = 0;
        while (i < _bins && _key[i] != key) i++;
        if (i == _bins)
        {
            if (_bins == Bins) return;
            _key[i] = key;
            _weight[i] = _sum[i] = 0.0;
            _bins++;
        }
        _weight[i] += area;
        _sum[i] += area * y;
        WaterTris++;
    }

    static float WorldY(float sx, float sy, float z, float h)
        => _ry0 * sx * z / h + _ry1 * sy * z / h + _ry2 * z + _camY;

    static float _plane;
    static bool _hasPlane;

    /// <summary>The plane to mirror in, from the water drawn since the last call,
    /// and the bins emptied. The previous plane is kept while its own bin is still
    /// most of the heaviest, so two pools of one area cannot trade places frame by
    /// frame.</summary>
    public static bool TakePlane(out float worldY, out double area)
    {
        worldY = 0f;
        area = 0.0;
        int best = -1;
        for (int i = 0; i < _bins; i++)
            if (best < 0 || _weight[i] > _weight[best]) best = i;
        if (best < 0) { _bins = 0; _hasPlane = false; return false; }

        int keep = -1;
        if (_hasPlane)
        {
            int prev = (int)MathF.Round(_plane / 16f);
            for (int i = 0; i < _bins; i++)
                if (Math.Abs(_key[i] - prev) <= 1 && _weight[i] >= 0.6 * _weight[best]) { keep = i; break; }
        }
        int pick = keep >= 0 ? keep : best;
        _plane = (float)(_sum[pick] / _weight[pick]);
        _hasPlane = true;
        worldY = _plane;
        area = _weight[pick];
        _bins = 0;
        return true;
    }

    /// <summary>The last readback's share of reflective pixels that took the planar
    /// texture, from <see cref="ScreenReflections.SetMap"/>.</summary>
    public static float PlanarPct;

    /// <summary>The check on the mirror, from the same readback: the share of planar
    /// pixels whose march found an on-screen surface too, the mean brightness
    /// difference between the two (0..255), and the same against the planar
    /// texture read unmirrored. A mirror sampled in the right place reads well
    /// under its control; one sampled in the wrong place reads like it.</summary>
    public static float ComparedPct, MirrorDiff, ControlDiff;

    /// <summary>The probe's fog check: asked for by the port, filled by the next
    /// reflection pass that reads a planar texture.</summary>
    public static bool WantFogCensus;
    public static string? FogCensus;
}
