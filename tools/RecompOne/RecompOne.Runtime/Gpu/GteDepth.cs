namespace RecompOne.Runtime;

/// <summary>
/// Per-vertex view depth and sub-pixel position, recovered from the GTE and looked
/// up again by screen position — the two pieces of information the GPU never gets,
/// and the reason PlayStation textures swim and its vertices wobble.
///
/// The GTE does the perspective divide itself: <c>RTPS</c>/<c>RTPT</c> project a
/// vertex to a 2D screen coordinate and the game copies that coordinate into a
/// GP0 packet. By the time the GPU sees a polygon there is no depth left in it, so
/// it can only interpolate U and V linearly across the screen — affine mapping,
/// which is exact only for a surface parallel to the screen and increasingly wrong
/// the more a surface is foreshortened. That is the swimming, and the diagonal
/// crease down a quad where its two triangles disagree.
///
/// So the depth has to be caught upstream and carried forward. <c>Gte.Rtp</c> knows
/// both halves at once — it computes the screen coordinate and the view depth SZ3
/// in the same call — and the screen coordinate is exactly what turns up in the
/// packet: <c>SatX</c>/<c>SatY</c> clamp it to 11 bits signed, which is the same
/// 11 bits <c>GpuRaster.CoordX</c> decodes. That makes the screen position a key
/// the two ends can share without tracking a single register or store.
///
/// The sub-pixel position is the same story told about the same number. The GTE
/// projects to 16.16 fixed point and then throws the low sixteen bits away:
/// <c>SX2</c> is a whole pixel, so a vertex sliding slowly across the screen sits
/// still and then jumps, and the polygon it belongs to shears as its corners jump
/// at different moments. That is the wobble. The fraction is right there in the
/// same expression as the depth, one shift earlier, and it is dropped for the same
/// reason — so it is caught in the same place and keyed the same way.
///
/// Hence a small hash table: the GTE writes (x, y) -> (z, fx, fy) as it projects,
/// and the GPU asks for it again as it decodes a vertex word. A hit gives the
/// polygon a W per vertex, so both renderers switch to perspective-correct
/// interpolation, and the fraction of a pixel the GTE truncated, so the vertex
/// lands where it was actually projected; a miss leaves the vertex exactly as it
/// was. **2D work therefore corrects itself** — a HUD sprite whose coordinates the
/// CPU computed was never in the table, so it keeps the affine mapping and the
/// pixel grid that 2D wants.
///
/// The two halves are read independently: <see cref="Enabled"/> serves the depth
/// and <see cref="Subpixel"/> the fraction, one lookup either way.
///
/// Screen position is not a unique key. Two vertices of different depths land on
/// the same pixel all the time — a distant wall behind a nearby column, or several
/// off-screen vertices clamped to ±1024 — and last-write-wins then hands one
/// polygon the other's W. A nearby floor that inherits a far Z looks as if the
/// camera jumped to the horizon; a vertex that inherits the wrong fraction jumps
/// by up to a pixel every time the winner changes. The table therefore keeps the
/// last few samples at each key, and <see cref="Apply"/> picks the set whose
/// depths belong together on the primitive being drawn. A leftover that is still
/// an obvious high outlier (one corner tens of times further than the other two,
/// with no geometric progression toward it) is dropped, so that triangle stays
/// affine rather than tearing.
///
/// Saturated coordinates are recorded too. Dropping them made every large nearby
/// wall and floor — the polygons that want correction most — fall back to affine
/// the moment one vertex left the ±1024 window, which is the pop that looks like
/// the vantage point jumped. The clamp is still the key, because that is what the
/// packet carries; uniqueness for W comes from keeping several samples and from
/// the primitive-level pick. **The position is not moved past the clamp.** Every
/// off-screen vertex the GPU would have stuck at ±1024 stays there, so a shared
/// edge whose other end is still on the wall does not open a hole. Only the
/// [0, 1) fraction of an on-screen vertex is served, and it is a function of the
/// key alone — two triangles that share a vertex look up the same fraction even
/// when they pick different depths.
///
/// The other properties of the table are unchanged:
///
///   * **Nothing is cleared per frame.** Entries carry a monotonic sequence number
///     and expire once the table's own capacity of vertices has been written past
///     them, which is a frame or two of geometry. That keeps the frame boundary
///     out of this file entirely — there is no hook to place and nothing to keep
///     in step with double buffering.
///   * **A primitive is all-or-nothing for W.** The caller only interpolates
///     perspectively when *every* vertex of the triangle hit, so a polygon can
///     never mix a real W with a fallback of 1 and shear itself apart. The
///     fraction needs no such rule and does not have one — see below.
///   * **A vertex behind the eye is not recorded at all.** <c>SZ3</c> of zero is a
///     vertex the divide could not place, and it is dropped for both halves
///     rather than for one, so turning the fraction on cannot change which
///     vertices carry a depth.
///
/// Why the fraction is per vertex where W is per primitive: W is an interpolation
/// parameter, and one corner disagreeing about it tears the texture across the
/// whole triangle. A fraction is just where the corner is. A triangle with one
/// corner moved by half a pixel is a triangle with one corner moved by half a
/// pixel, and — because two triangles sharing a vertex look up the same key and
/// get the same answer — a shared edge still has identical endpoints on both
/// sides of it, so no crack can open along one.
/// </summary>
public static class GteDepth
{
    // Screen position turned out not to be an identity at all, and everything below
    // that picks between samples is a heuristic standing in for one. GteVertexMap
    // has the real thing -- the address of the word the coordinate lives at -- and
    // it serves both halves now. What is left here is the switches, the
    // accounting, and this table, which answers only when PositionFallback asks it
    // to so that the two mechanisms can be compared in one build.

    static bool _enabled, _subpixel, _zbuffer, _ao, _ssr;

    /// <summary>
    /// While false the depth is not served, so both renderers interpolate affinely
    /// exactly as they did before this existed. Safe to change at run time: the
    /// map simply starts or stops being consulted for it.
    /// </summary>
    public static bool Enabled
    {
        get => _enabled;
        set { _enabled = value; GteVertexMap.SetActive(Active); }
    }

    /// <summary>
    /// While false the recovered fraction is not served, so every vertex snaps to
    /// the whole pixel the GTE truncated it to, exactly as the console does. Also
    /// safe to change at run time.
    /// </summary>
    public static bool Subpixel
    {
        get => _subpixel;
        set { _subpixel = value; GteVertexMap.SetActive(Active); }
    }

    /// <summary>
    /// While false both renderers keep the console's painter's algorithm: the
    /// ordering table is the whole of occlusion. On, a triangle whose vertices
    /// all recovered a view depth is depth-tested per pixel against that depth,
    /// so two surfaces that actually interpenetrate stop taking turns in front
    /// of each other. A miss is the old behaviour — 2D never hits, so the HUD
    /// still draws on top in table order. Safe to change at run time: the next
    /// triangle starts or stops testing.
    /// </summary>
    public static bool ZBuffer
    {
        get => _zbuffer;
        set
        {
            if (_zbuffer == value) return;
            _zbuffer = value;
            Generation++;
            GteVertexMap.SetActive(Active);
        }
    }

    /// <summary>Bumped when <see cref="ZBuffer"/> is flipped, so a render target
    /// that already has a depth buffer from last frame knows to clear it rather
    /// than test against whatever was sitting there while the setting was off.</summary>
    public static int Generation;

    /// <summary>
    /// How far the mean view depth of a primitive may fall below the previous
    /// primitive's before the depth buffer is cleared, in GTE view-depth units.
    /// DuckStation's <c>pgxp_depth_clear_threshold</c>, and its default of 300.
    ///
    /// A frame is not one scene. A game that draws the world, then a weapon in the
    /// player's hand, then a menu behind the same projection hands the depth buffer
    /// three ranges that have nothing to do with each other, and testing the second
    /// against the first hides it. The console had no depth buffer and so no such
    /// problem: everything took its turn on the ordering table. The break is
    /// detected the only way it can be without knowing what the game means -- a
    /// large step towards the camera between consecutive primitives -- and the
    /// clear is the existing per-frame one, reached by bumping
    /// <see cref="Generation"/> rather than by a clear path of its own.
    ///
    /// Zero or less turns it off, which is one depth buffer for the whole frame,
    /// **and that is the default here.** DuckStation's 300 is for games that draw
    /// several 3D scenes in one frame; King's Field draws one world and a 2D HUD,
    /// and 2D never recovers a depth so it never enters the mean. Measured over a
    /// walk through area 2, the forward steps between consecutive primitives are a
    /// single smoothly decaying population with no gap in it — 39.4% under 10
    /// units, 47.7% under 50, 9.2% under 150, 2.8% under 300, 0.9% beyond, widest
    /// 318 — which is ordinary depth sorting inside one scene and nothing else.
    /// With no second population there is no scene break to detect, and at 300 the
    /// guard fired **2575 times a second**, some twenty times a frame, throwing the
    /// world's own depth away mid-frame and taking the frame rate from 144 to
    /// 34-76 fps with it (each clear flushes the GL batch). Left in the code
    /// because it is the right mechanism for a game that needs it; set to zero
    /// because this one does not.
    /// </summary>
    public static float DepthClearThreshold;

    /// <summary>0051. How far behind the stored depth a fragment may be and still draw,
    /// so two coplanar surfaces go to the later table entry instead of fighting: a
    /// constant in SZ units, plus a multiple of the depth's change across one pixel
    /// (GL only). Depths are still written unbiased.</summary>
    public static float DepthBias = 1f, DepthSlope = 0.5f;

    /// <summary>How many times the threshold above has fired. A rate rather than a
    /// picture: several a frame means the threshold is too small and the buffer is
    /// being thrown away, none at all means it is doing nothing.</summary>
    public static long ZClears;

    /// <summary>Every positive step towards the camera between consecutive
    /// primitives, bucketed at &lt;10, &lt;50, &lt;150, &lt;300, &lt;1000, &gt;=1000 view-depth
    /// units, and the largest seen. Censused whether or not the threshold fired,
    /// so the two populations — ordinary sorting inside one scene, and a genuine
    /// scene break — can be told apart if they exist.</summary>
    public static readonly long[] ZDrops = new long[6];

    /// <summary>Largest drop seen in the window, in view-depth units.</summary>
    public static float ZDropMax;

    /// <summary>
    /// Ambient occlusion. While false nothing below runs and the depth attachment
    /// is written only when <see cref="ZBuffer"/> asks for it.
    ///
    /// On, every triangle whose corners all recovered a view depth *writes* that
    /// depth — with the test left at <c>GL_ALWAYS</c> unless the Z-buffer is also
    /// on, so nothing is ever rejected and the ordering table stays the whole of
    /// occlusion. **Painter's order is what makes that a usable G-buffer**: the
    /// table is walked back to front, so the last write at a pixel is the nearest
    /// visible surface, which is exactly the depth a screen-space pass wants and
    /// is arrived at without a depth test, a prepass or a second submission of the
    /// frame's geometry. Everything without a recovered depth writes the far plane
    /// instead of the interpolated clip Z it used to, so the 2D HUD, the menus and
    /// any triangle the map missed read as "no surface here" and are left alone by
    /// the pass — semi-transparent primitives write nothing at all, so a death fade
    /// or a damage flash does not erase the world's depth underneath it.
    ///
    /// Safe to change at run time: <see cref="Generation"/> is bumped so the next
    /// draw onto a target clears the attachment rather than shading against
    /// whatever was left in it while the setting was off. GL backend only — the
    /// software rasterizer keeps its own float-per-pixel buffer and has no
    /// full-screen pass to run this in.
    /// </summary>
    public static bool AmbientOcclusion
    {
        get => _ao;
        set
        {
            if (_ao == value) return;
            _ao = value;
            Generation++;
            GteVertexMap.SetActive(Active);
        }
    }

    /// <summary>Whether the depth attachment is worth writing at all. The two
    /// consumers want the same numbers in the same buffer and differ only in what
    /// is done with them — one rejects fragments, the other reads the finished
    /// buffer back in a full-screen pass — so the write is shared and the test is
    /// not.</summary>
    public static bool DepthWanted => _zbuffer || _ao || _ssr;

    /// <summary>0067. The reflection pass, the second reader of the finished frame's
    /// depth and the first reader of its surface buffer: on while anything it
    /// composites is (<see cref="ScreenReflections.Refresh"/>), the screen march,
    /// the murk, the planar walk or the retained scene.</summary>
    public static bool Reflections
    {
        get => _ssr;
        set
        {
            if (_ssr == value) return;
            _ssr = value;
            Generation++;
            GteVertexMap.SetActive(Active);
        }
    }

    /// <summary>0067. Whether a pass wants the frame's surfaces: the far-plane mask
    /// under the HUD, the projection read off the GTE, and the geometry kept for
    /// the surface buffer. Ambient occlusion was the only such pass, and every site
    /// that asked for it by name meant this.</summary>
    public static bool SurfacesWanted => _ao || _ssr;

    /// <summary>How far a sample may sit from the shaded point and still occlude
    /// it, in GTE view-depth units, which are the game's own world units — a floor
    /// tile is 2048 of them. This is the whole of the look: too small and only the
    /// creases in a wall darken, too large and a corridor goes grey end to end.
    ///
    /// A quarter of a tile, which is the scale of the things it is meant to find —
    /// the recess of a doorframe, the base of a pillar, the join of a wall and a
    /// floor. Measured over one view in area 1, the share of the picture the pass
    /// darkens goes 9.2% / 18.0% / 33.9% / 39.6% at 192 / 512 / 1024 / 2048, so by
    /// a tile it is shading a third of everything in sight and has stopped being
    /// contact shading. Nobody has looked at any of the four; this is the one whose
    /// *scale* matches what the effect is for.</summary>
    public static float AoRadius = 512f;

    /// <summary>How dark a fully occluded pixel goes, 0 (nothing) to 1 (black).</summary>
    public static float AoStrength = 0.8f;

    /// <summary>The angular bias, as a cosine: a sample within this much of the
    /// shaded surface's own plane is treated as part of that surface rather than as
    /// something standing in front of it. It is what keeps a flat wall from
    /// shading itself out of the recovered depth's own quantisation.</summary>
    public static float AoBias = 0.08f;

    /// <summary>Samples per pixel in the occlusion pass. Rotated by a 4x4
    /// interleaved pattern and blurred by a 4x4 kernel that cancels it exactly, so
    /// this buys smoothness rather than the absence of a visible grid.</summary>
    public static int AoSamples = 16;

    /// <summary>The occlusion pass's resolution in multiples of the game's own
    /// pixels, capped at the render scale; 0 is the render scale. The pass, its blur
    /// and the normal buffer all run at it, and the present upsamples.</summary>
    public static int AoResolution;

    /// <summary>Beyond this view depth the pass returns unoccluded. The game's own
    /// fog has taken the picture by then, and the projected radius of a world-unit
    /// sphere has fallen under a pixel, so the samples would all land in the same
    /// texel and produce nothing but noise.</summary>
    public static float AoMaxDepth = 24000f;

    // ---- 0059: occluders the camera cannot see --------------------------------

    /// <summary>0059. Occlude against the area's own floor plan as well as against
    /// the picture. A screen-space pass can only be occluded by what is on screen,
    /// so standing in a corner facing one wall, the wall behind the camera
    /// contributes nothing and the shading changes as the view turns. The port reads
    /// the game's 80x80 tile grid, so the geometry that is not on screen is
    /// available to it. Off by default: it is a look nobody has seen.</summary>
    public static bool AoWorld;

    /// <summary>How dark the world term goes, and how far it reaches in world units
    /// (a floor tile is 2048).</summary>
    public static float AoWorldStrength = 0.6f, AoWorldRadius = 3072f;

    /// <summary>The camera's rotation as the game loaded it (world to view, the
    /// GTE's 4096 scale removed) and its world position. Published by the port from
    /// the view matrix the tile walk loads, which is the camera's own: the game hands
    /// the GTE positions already relative to the camera, so this matrix carries no
    /// translation and no per-object rotation.</summary>
    public static readonly float[] AoViewR = new float[9];
    public static float AoCamX, AoCamY, AoCamZ;

    /// <summary>Whether the two above have been filled since the last area load.</summary>
    public static bool AoWorldReady;

    /// <summary>The area's floor plan: 80x80 tiles, four bytes each -- the lower
    /// half's height byte, the upper's, whether the tile stops sight, and padding.
    /// Height is up in units of 128, as the game's own <c>(0 - height) &lt;&lt; 7</c>
    /// makes it. The generation is bumped when the port refills it, which is what
    /// makes the backend re-upload.</summary>
    public static byte[]? AoHeight;
    public static int AoHeightGen;

    /// <summary>The grid's span and a tile's world size, so nothing has to agree with
    /// the port about them by coincidence.</summary>
    public const int AoHeightSpan = 80, AoTileUnits = 2048;

    /// <summary>World-term pixels shaded, and frames the term could not run for want
    /// of a transform or a grid.</summary>
    public static long AoWorldFrames, AoWorldUnready;

    /// <summary>Transform reads the port refused as not the camera's, holding the
    /// previous one instead. A rate near the frame rate means the term is being fed
    /// a matrix that is mostly stale and the reading needs another look.</summary>
    public static long AoWorldStale;

    /// <summary>Tiles in the last grid read that stop sight. Zero is a grid that
    /// cannot occlude anything and the term will measure as doing nothing.</summary>
    public static int AoHeightBlockers;

    /// <summary>How tall a blocking tile is taken to be, in world units above its
    /// own floor. A tile is 2048 across.</summary>
    public static float AoWallHeight = 2048f;

    /// <summary>0058. Take the pass's normals from the frame's own geometry, redrawn
    /// into a normal buffer once the frame is finished (<see cref="AoGeometry"/>),
    /// instead of differencing four depth texels. A pixel the normal buffer did not
    /// reach falls back to the depth cross product, so this is additive: turning it
    /// off is the picture exactly as it was.</summary>
    public static bool AoNormals = true;

    /// <summary>The projection the GTE is actually using, published from
    /// <c>Gte.Rtp</c>: the projection distance H and the screen-space centre
    /// OFX/OFY the divide is offset by. The pass has to undo the game's own
    /// projection to get a view position back out of a depth texel, and these are
    /// that projection rather than an assumption about it — a wrong H tilts every
    /// reconstructed normal and a wrong centre tilts them more towards the edges of
    /// the picture, which is exactly the kind of error a screenshot cannot tell
    /// from a look.</summary>
    public static float ProjH = 320f, ProjCx = 160f, ProjCy = 120f;

    // There was a ProjOffX/ProjOffY here -- the GP0 drawing offset of the last
    // depth-writing triangle -- on the reasoning that the GTE's centre is in
    // packet coordinates while the target holds VRAM ones, so the offset is the
    // difference. It is not, and it made the reconstruction wrong on half the
    // frames: the offset it recorded belonged to the buffer being *drawn*, while
    // the pass runs against the buffer being *presented*, and with two display
    // buffers those differ by a screen on alternate frames. The right answer is
    // that a display target's own origin already is that offset -- if it were not,
    // the prim shader's uPosBias of -rt.X/-rt.Y would put every polygon in the
    // wrong place and the picture would be broken long before this pass ran. So
    // the target answers, and nothing per-triangle is recorded at all.

    /// <summary>Where the pass put the projection centre, as a fraction of the
    /// display area, published so the probe can print it. It should be 0.5, 0.5 on
    /// every frame; anything else is the display origin and the drawing offset
    /// disagreeing, which is silent, intermittent (the two display buffers differ
    /// by a screen) and shows up only as the shading being subtly wrong.</summary>
    public static float AoCentreX = 0.5f, AoCentreY = 0.5f;

    /// <summary>Vertices whose projection was read for <see cref="ProjH"/>. A rate
    /// of zero with the setting on means the GTE is not projecting and the numbers
    /// above are the defaults, not a reading.</summary>
    public static long ProjSeen;

    /// <summary>Full-screen occlusion passes run, and presents that had no target
    /// to run one against (the VRAM fallback, or an MDEC frame).</summary>
    public static long AoPasses, AoNoTarget;

    /// <summary>KF2_AO_PROBE=1: the coverage, the projection and the pass count.</summary>
    public static bool AoProbe;

    public static void ResetAoCounters()
    {
        AoPasses = AoNoTarget = ProjSeen = 0;
    }

    /// <summary>KF2_AO_PROBE=2: read the blurred occlusion texture back after the
    /// next pass and reduce it to <see cref="AoMap"/> and the three numbers below.
    /// One frame, on request, for the same reason the depth map is.
    ///
    /// **It is the only counter that can tell the pass working from the pass
    /// running.** Every other number here says the depth arrived and the two
    /// draws were issued; a shader that returns white on every pixel produces
    /// exactly the same report and exactly no shading, and this is a project where
    /// nobody is allowed to go and look.</summary>
    public static bool WantAoMap;

    public const int AoMapCols = 32, AoMapRows = 16;

    /// <summary>The mean occlusion factor in each cell of the finished frame, 1
    /// being unshaded. Cells over the HUD and over anything the vertex map missed
    /// read exactly 1, which is what the far-plane mask is for.</summary>
    public static float[]? AoMap;

    /// <summary>The share of each cell the pass found a surface in, from the mask
    /// channel. It is the other half of reading <see cref="AoMap"/>: a cell at 1.00
    /// occlusion with no surface under it is the mask doing its job, and the same
    /// cell with a surface under it is the pass finding nothing to shade with —
    /// which is a radius that is too small, or a reconstruction that is wrong.
    /// Without this the two are indistinguishable and every blank patch is an
    /// argument.</summary>
    public static float[]? AoCoverage;

    /// <summary>The darkest factor anywhere in the frame, the mean over the whole
    /// frame, the share of pixels the pass actually darkened, and the share it
    /// found a surface at all. The last is the denominator the third wants: 9% of
    /// the picture shaded is a different claim when 60% of it carries a surface
    /// than when 100% does.</summary>
    public static float AoMin = 1f, AoMean = 1f, AoShadedPct, AoCoveragePct;

    /// <summary>0058. The share of the surfaces the pass shaded whose normal came
    /// from the geometry rather than from the depth cross product. It is the one
    /// number that says whether the normal buffer reached the picture: everything
    /// else reads the same with an empty one, because the fallback is the old
    /// mechanism and produces a perfectly plausible frame.</summary>
    public static float AoGeoNormalPct;

    /// <summary>0058. How far the old depth-difference normal was from the
    /// geometry's, over the surfaces that have both: the mean in degrees, the share
    /// more than 30 degrees out, and the worst. This is what says whether the change
    /// is worth anything *here* -- a view of one flat wall square-on agrees to a
    /// degree, and a doorway full of edges does not.</summary>
    public static float AoNormalMeanDeg, AoNormalBadPct, AoNormalMaxDeg;

    /// <summary>The same disagreement, by cell, so the map says *where* to look.</summary>
    public static float[]? AoNormalMap;

    /// <summary>Four bytes a pixel: red the occlusion factor, green the mask, blue
    /// whether the normal came from the geometry, alpha how far the old
    /// depth-difference normal was from it, in right angles.</summary>
    public static void SetAoMap(ReadOnlySpan<byte> ao, int w, int h)
    {
        var map = AoMap ??= new float[AoMapCols * AoMapRows];
        var cov = AoCoverage ??= new float[AoMapCols * AoMapRows];
        var nrm = AoNormalMap ??= new float[AoMapCols * AoMapRows];
        Array.Fill(map, 1f);
        Array.Fill(cov, 0f);
        Array.Fill(nrm, 0f);
        AoNormalMeanDeg = AoNormalBadPct = AoNormalMaxDeg = 0f;
        AoMin = 1f; AoMean = 1f; AoShadedPct = 0f; AoCoveragePct = 0f; AoGeoNormalPct = 0f;
        WantAoMap = false;
        if (w <= 0 || h <= 0) return;

        Span<int> cellN = stackalloc int[AoMapCols * AoMapRows];
        Span<int> cellHit = stackalloc int[AoMapCols * AoMapRows];
        Span<float> cellSum = stackalloc float[AoMapCols * AoMapRows];
        Span<int> cellNrmN = stackalloc int[AoMapCols * AoMapRows];
        Span<float> cellNrmSum = stackalloc float[AoMapCols * AoMapRows];
        double sum = 0, nrmSum = 0;
        long shaded = 0, covered = 0, geoNormal = 0, nrmN = 0, nrmBad = 0, n = 0;

        for (int y = 0; y < h; y++)
        {
            // The readback is bottom-up, the picture is top-down.
            int row = (h - 1 - y) * AoMapRows / h;
            int rowBase = y * w * 4;
            for (int x = 0; x < w; x++)
            {
                float v = ao[rowBase + x * 4] * (1f / 255f);
                bool surface = ao[rowBase + x * 4 + 1] >= 128;
                bool geo = surface && ao[rowBase + x * 4 + 2] >= 128;
                if (geo) geoNormal++;
                int cellOf = row * AoMapCols + x * AoMapCols / w;
                if (geo)
                {
                    // Alpha is the angle over 90 degrees.
                    float deg = ao[rowBase + x * 4 + 3] * (90f / 255f);
                    nrmSum += deg;
                    nrmN++;
                    if (deg > 30f) nrmBad++;
                    if (deg > AoNormalMaxDeg) AoNormalMaxDeg = deg;
                    cellNrmSum[cellOf] += deg;
                    cellNrmN[cellOf]++;
                }
                sum += v;
                n++;
                // A pixel is "shaded" once it is more than one 8-bit step from
                // white, so the quantisation of the target is not counted as
                // occlusion.
                if (v < 254f / 255f) shaded++;
                if (surface) covered++;
                if (v < AoMin) AoMin = v;
                int cell = row * AoMapCols + x * AoMapCols / w;
                cellSum[cell] += v;
                cellN[cell]++;
                if (surface) cellHit[cell]++;
            }
        }

        for (int i = 0; i < map.Length; i++)
        {
            if (cellN[i] > 0) { map[i] = cellSum[i] / cellN[i]; cov[i] = (float)cellHit[i] / cellN[i]; }
            if (cellNrmN[i] > 0) nrm[i] = cellNrmSum[i] / cellNrmN[i];
        }
        if (nrmN > 0)
        {
            AoNormalMeanDeg = (float)(nrmSum / nrmN);
            AoNormalBadPct = 100f * nrmBad / nrmN;
        }
        if (n > 0)
        {
            AoMean = (float)(sum / n);
            AoShadedPct = 100f * shaded / n;
            AoCoveragePct = 100f * covered / n;
            AoGeoNormalPct = covered > 0 ? 100f * geoNormal / covered : 0f;
        }
    }

    /// <summary>Called from <c>Gte.Rtp</c> while the pass is on. Last writer wins,
    /// which is right: a frame is projected under one H and one centre, and the
    /// pass runs after the last vertex of it.</summary>
    public static void NoteProjection(float h, float cx, float cy)
    {
        if (h > 0f) { ProjH = h; ProjCx = cx; ProjCy = cy; ProjSeen++; }
    }

    /// <summary>0067. The depth cue the last projection was made with, DQA and DQB
    /// as the GTE holds them: <c>IR0 = (DQA * H/SZ + DQB) >> 12</c>. The reflection
    /// pass fogs a reflected ray's longer path on the same curve.</summary>
    public static int ProjDqa, ProjDqb;

    public static void NoteDepthCue(int dqa, int dqb) { ProjDqa = dqa; ProjDqb = dqb; }

    /// <summary>
    /// True color (24-bit). While false the GL backend renders into an RGB5A1
    /// display target and the fragment shader crushes every shaded pixel to five
    /// bits per channel, exactly as the PlayStation's 15-bit VRAM does — which
    /// bands a smooth fog gradient. On, the display target is RGBA8 and the shader
    /// keeps eight bits, so the gradient is smooth without the dither crosshatch.
    /// Textures are still sampled at five bits (they live in 15-bit VRAM), so only
    /// the shaded gradient gains precision, not the texture palette. GL backend
    /// only; the software rasterizer is always 15-bit. Safe to change at run time:
    /// <see cref="GlCore"/> rebuilds its display targets on the next present when
    /// this differs from the format they were built with.
    /// </summary>
    public static bool TrueColor;

    /// <summary>Nothing is recorded and every lookup misses while every consumer
    /// is off, which is what makes this cost nothing when none of them is wanted.</summary>
    public static bool Active => _enabled || _subpixel || _zbuffer || _ao || _ssr;

    /// <summary>Consult the screen-position table below for vertices the exact map
    /// could not answer for. Off by default — it is the guess this was all built to
    /// stop making, and it is kept only so the two can be measured against each
    /// other. Wired to KF2_PERSPECTIVE_FALLBACK.</summary>
    public static bool PositionFallback;

    /// <summary>
    /// Anisotropic filtering: how many texels of the pixel's footprint may be
    /// sampled along its longest axis. 1 is off and is the console's own single
    /// point sample.
    ///
    /// The footprint of a screen pixel in texture space is the parallelogram
    /// spanned by the two screen derivatives of the texture coordinate. Square-on
    /// to a wall it is roughly a square; on a floor running away to the horizon it
    /// is long and thin, covering many texels along one axis and barely one across
    /// the other. Reading a single texel out of that is what makes a receding floor
    /// crawl and sparkle as the camera moves. The fragment shader takes up to this
    /// many samples along the long axis instead and averages them, which is the
    /// only mip-free way to do it — the VRAM sheet cannot carry a mip chain, since
    /// one page's lower level would average in its neighbours and a CLUT's would
    /// average in the palette beside it. <see cref="Mipmaps"/> builds one where the
    /// texture is decoded instead.
    ///
    /// Clamped to 1..16 by <c>Kf2.Anisotropic</c>. GL backend only; the software
    /// rasterizer is always a single sample. Safe to change at run time — it is a
    /// plain uniform the next batch reads, with nothing rebuilt.
    /// </summary>
    public static int Anisotropy = 1;

    /// <summary>True once the GL backend has found <c>uAniso</c> on the prim
    /// program and is uploading it. This is the counter that matters: the port has
    /// twice shipped a picture switch that printed "on" at boot while the mechanism
    /// underneath it was dead — the RAM fast path went round the vertex map's
    /// hooks, and a hook summary counted registrations rather than detours. A
    /// setting that cannot reach the shader should say so rather than be believed.
    /// </summary>
    public static bool AnisotropyLive;

    /// <summary>
    /// 0060. Mipmaps: each texture a polygon is minified on is decoded through its
    /// CLUT into an atlas with a mip chain of its own, and the prim shader samples
    /// that at the footprint's level, with <see cref="Anisotropy"/> taps along the
    /// long axis. A texture cannot be mipmapped where it lives -- VRAM is one sheet
    /// of pages, CLUTs and indices -- so it is mipmapped where it is decoded.
    /// GL core backend only.
    /// </summary>
    public static bool Mipmaps;

    /// <summary>True once the GL backend has built the atlas and found <c>uMipOn</c>.</summary>
    public static bool MipmapsLive;

    /// <summary>0073. How often a replacement texture's filter was set.</summary>
    public static long RepFilterSets;

    /// <summary>Atlas entries live, decoded (first or again), evicted, and polygons
    /// that asked and found no room. Never reset.</summary>
    public static long MipEntries, MipDecodes, MipEvictions, MipFull;

    /// <summary>Decode batches still to read back and print (KF2_ANISO_PROBE=2).</summary>
    public static int MipVerify;

    /// <summary>
    /// 0053. Scrolling textures — water, slime skins, the main-hall fire — that
    /// <c>func_8002DC78</c> re-uploads at the world tick. Each live slot is a VRAM
    /// dest rect plus a leftover V shift in texels, so the fragment shader can
    /// blend the two wrap-rows the integer upload sits between. <see cref="FluidN"/>
    /// of 0 is off and is the console's own stepped picture; the shader takes that
    /// path by construction. GL backend only; the software rasterizer always
    /// samples the last upload. Safe to change at run time — plain uniforms the
    /// next batch reads.
    /// </summary>
    public const int FluidSlots = 8;

    public struct FluidRec
    {
        public float X, Y, W, H, Off;
    }

    public static int FluidN;
    public static readonly FluidRec[] Fluid = new FluidRec[FluidSlots];

    /// <summary>True once the GL backend has found <c>uFluidN</c> on the prim
    /// program and is uploading it. Same job as <see cref="AnisotropyLive"/>.</summary>
    public static bool FluidLive;

    const int Bits = 14;
    const int Size = 1 << Bits;
    const int Mask = Size - 1;

    // Linear probing, bounded. A key that cannot be placed within this many slots
    // displaces the oldest of them rather than searching further, so both Record
    // and Collect are constant time no matter how full the table is. Wider than
    // the four samples kept per key, so a hot pixel and its hash neighbours still
    // fit in the window.
    const int ProbeLen = 16;

    // Several vertices share a screen pixel; last-write-wins is what hands a
    // nearby floor a far wall's W. Four is enough for a quad's corners to collide
    // on the clamp and still all be sitting in the table when the primitive is
    // drawn.
    public const int MaxCand = 4;

    // An entry is stale once a table's worth of vertices has been recorded since.
    // At a few thousand projected vertices a frame that is the last frame or two.
    const long MaxAge = Size;

    // A high outlier: the furthest vertex is more than this times the middle one,
    // *and* that jump is at least four times the step between the nearer two. A
    // corridor floor (100, 500, 2000) is a geometric progression and passes; a
    // collision (100, 120, 8000) is a cliff and does not. Only the far end is
    // tested — a vertex next to the camera among two distant ones is legitimate.
    const float OutlierRatio = 8f;
    const float OutlierCliff = 4f;

    // Fx and Fy are the [0, 1) fraction the GTE truncated, or zero for a vertex
    // that saturated: the packet coordinate is then the whole position, which is
    // what keeps a shared edge closed when one end is on the clamp wall.
    struct Slot { public int Key; public long Seq; public float Z, Fx, Fy; }

    public struct Sample
    {
        public float Z, Fx, Fy;
        public long Seq;
    }

    /// <summary>One vertex of the primitive being bound: the lookup key is the
    /// packet coordinate, and the rest is filled in by <see cref="Apply"/>.</summary>
    public struct Attr
    {
        public int X, Y;
        public float Z, Fx, Fy;
        public bool HasW, HasSub;
    }

    static readonly Slot[] _slots = new Slot[Size];
    static long _seq;

    /// <summary>Vertices recorded, and lookups that found one or did not. The
    /// hit rate is the only real evidence that the two ends agree on the key.</summary>
    public static long Recorded, Hits, Misses;

    /// <summary>Vertices whose screen position was the GTE clamp, not the true
    /// projection. They used to be dropped entirely; they are now the main source
    /// of extra hits on nearby walls and floors.</summary>
    public static long Saturated;

    /// <summary>Vertices that <see cref="Apply"/> rebound from the newest sample
    /// at their key to an older one whose depth fitted the rest of the primitive.</summary>
    public static long Refined;

    /// <summary>Vertices whose recovered W was an obvious high outlier with no
    /// better candidate, so perspective was refused for that corner.</summary>
    public static long Rejected;

    public static void ResetCounters() =>
        Recorded = Hits = Misses = Saturated = Refined = Rejected = 0;

    /// <summary>Triangles that depth-tested, triangles that had no recovered Z
    /// and so kept painter's order, and software-rasterizer pixels that lost the
    /// test. The hardware path cannot count pixels without reading the buffer
    /// back, so <see cref="ZRejects"/> stays at zero there.</summary>
    public static long ZTris, ZSkipped, ZRejects;

    /// <summary>0051. Opaque batches that wrote their true depth before their colour.</summary>
    public static long ZPrepasses;

    public static void ResetZCounters()
    {
        ZTris = ZSkipped = ZRejects = ZClears = 0;
        Array.Clear(ZDrops);
        ZDropMax = 0f;
    }

    /// <summary>A census of the frame's <i>large</i> polygons, in the order the
    /// ordering table submitted them, with the depth each one recovered. It exists
    /// to answer one question a rate cannot: when a depth-tested surface disappears,
    /// which earlier primitive claimed to be in front of it. The table is walked
    /// back to front, so an earlier entry is one the game itself sorted as farther
    /// away — an early entry holding a near depth over a wide area is a recovered
    /// depth that disagrees with the game's own sort, and that is what a hole in
    /// the picture looks like from here.
    ///
    /// Off unless <see cref="TriCensus"/> is set, and capped, so it costs a compare
    /// per polygon while nobody is asking. Small polygons are counted and not kept:
    /// a surface that hides half the view is never a small one.</summary>
    public static bool TriCensus;

    /// <summary>The ordering-table entry currently being emitted, counted from the
    /// head — which is the <i>far</i> end, since <c>DrawOTag</c> walks back to
    /// front. -1 outside a walk.
    ///
    /// This is the game's own opinion of a primitive's depth, and it is the only
    /// thing in the port that can contradict a recovered SZ. The two normally
    /// agree, because the entry is <c>OTZ</c> and <c>OTZ</c> is the average of the
    /// same SZs. Where they disagree, the game overrode depth on purpose — a
    /// skybox is a small box drawn around the camera, so it projects <i>near</i>
    /// and is linked at the far end of the table to keep it behind everything.
    /// Believing its SZ puts the sky in front of the world.</summary>
    public static int OtEntry = -1;

    /// <summary>The ordering-table slot the walk is in, counted from the far end; -1 outside a walk.
    /// Slot 0 holds the skybox, which projects near but must read as no surface.</summary>
    public static int OtSlot = -1;

    /// <summary>How many entries the last completed walk had, so an entry can be
    /// read back as the OTZ the game linked at: <c>otz = OtLength - 1 - OtEntry</c>.
    /// Published one walk late, which is what makes it free.</summary>
    public static int OtLength;

    /// <summary>One large polygon: where it landed, what depth it recovered, and
    /// whether it depth-tested at all. <see cref="MinZ"/> is the nearest of its
    /// corners, so "entirely in front of" is <c>MaxZ &lt; other.MinZ</c>.</summary>
    public struct BigTri
    {
        public int Order;
        /// <summary>The ordering-table entry it was linked at. See <see cref="OtEntry"/>.</summary>
        public int Ot;
        public float MinZ, MaxZ;
        public int X0, Y0, X1, Y1;
        public bool Tested, Tex, Semi;
        /// <summary>A corner the GTE saturated at ±1024. The packet then carries a
        /// screen position that is not the projection of the vertex, so the depth
        /// ramp across the polygon is compressed — the visible part of it reads
        /// nearer than the surface is. That is a depth the console never had, and
        /// the reason this flag is worth a column.</summary>
        public bool Clamped;
        public int R, G, B;

        public long Area => (long)(X1 - X0 + 1) * (Y1 - Y0 + 1);
        public bool Overlaps(in BigTri o) => X0 <= o.X1 && o.X0 <= X1 && Y0 <= o.Y1 && o.Y0 <= Y1;
    }

    /// <summary>Big enough to be worth a line of its own, in pixels of the 320x240
    /// the game draws — a fiftieth of the picture. Only the *printed* lines are
    /// filtered by it; every polygon is kept, because the surface that vanishes is
    /// usually a crowd of small ones and the thing standing in front of them is the
    /// only large one in the pair.</summary>
    public const long CensusBigArea = 1500;

    const int CensusMax = 2048;

    static readonly BigTri[] _census = new BigTri[CensusMax];
    static int _censusCount, _censusOrder, _censusDropped;

    /// <summary>The polygons kept this frame, in submission order.</summary>
    public static ReadOnlySpan<BigTri> Census => _census.AsSpan(0, _censusCount);

    /// <summary>Polygons that were large enough but arrived after the table filled.</summary>
    public static int CensusDropped => _censusDropped;

    /// <summary>Every polygon submitted this frame, large or not — the denominator
    /// the kept ones are a fraction of.</summary>
    public static int CensusSubmitted => _censusOrder;

    /// <summary>Called once per polygon from the rasterizer's common path, with the
    /// vertex positions already relative to the draw area. Cheap and unconditional
    /// on the caller's side: the size test is here.</summary>
    public static void NoteTri(int x0, int y0, int x1, int y1, float minZ, float maxZ,
                               bool tested, bool tex, bool semi, bool clamped, int r, int g, int b)
    {
        int order = _censusOrder++;
        if (_censusCount >= CensusMax) { _censusDropped++; return; }

        _census[_censusCount++] = new BigTri
        {
            Order = order, Ot = OtEntry, MinZ = minZ, MaxZ = maxZ,
            X0 = x0, Y0 = y0, X1 = x1, Y1 = y1,
            Tested = tested, Tex = tex, Semi = semi, Clamped = clamped, R = r, G = g, B = b,
        };
    }

    /// <summary>Ask the hardware backend to read its depth attachment back at the
    /// next <c>Present</c> and reduce it to <see cref="DepthMap"/>. One frame, on
    /// request: a full-resolution readback is a pipeline stall, and this exists to
    /// be looked at every couple of seconds, not every frame.</summary>
    public static bool WantDepthMap;

    public const int DepthMapCols = 32, DepthMapRows = 16;

    /// <summary>The nearest depth in each cell of the finished frame, as SZ rather
    /// than as a [0,1) window value — the same units the census prints — with
    /// <see cref="DepthMapEmpty"/> for a cell nothing wrote. This is the occlusion
    /// the frame actually ended up with, which no amount of reasoning about
    /// submission order can substitute for.</summary>
    public static float[]? DepthMap;

    public const float DepthMapEmpty = 65536f;

    /// <summary>Depth-testing batches by where they were drawn: a display render
    /// target, which has a depth attachment, or the VRAM framebuffer, which has
    /// none — and where OpenGL therefore passes every depth test silently. If the
    /// second number is the large one, the hardware Z-buffer is not running at all
    /// and whatever changed in the picture changed for some other reason.</summary>
    public static long ZBatchRt, ZBatchVram;

    /// <summary>Display targets thrown away and rebuilt while the Z-buffer was on.
    /// Each one drops a depth attachment, so anything already tested into it stops
    /// occluding. A steady count per frame means the depth buffer is being reset
    /// underneath the frame that is using it.</summary>
    public static long ZRtRecreated;

    /// <summary>Called by the backend with the raw depth attachment. Reduces to the
    /// grid by taking the nearest sample in each cell, since occlusion is decided
    /// by the nearest thing there.</summary>
    public static void SetDepthMap(ReadOnlySpan<float> depth, int w, int h)
    {
        var map = DepthMap ??= new float[DepthMapCols * DepthMapRows];
        Array.Fill(map, DepthMapEmpty);
        if (w <= 0 || h <= 0) return;

        for (int y = 0; y < h; y++)
        {
            // The readback is bottom-up, the picture is top-down.
            int row = (h - 1 - y) * DepthMapRows / h;
            int rowBase = y * w;
            for (int x = 0; x < w; x++)
            {
                float d = depth[rowBase + x];
                if (d >= 1f) continue;
                int cell = row * DepthMapCols + x * DepthMapCols / w;
                float sz = d * 65536f;
                if (sz < map[cell]) map[cell] = sz;
            }
        }
        WantDepthMap = false;
    }

    /// <summary>Start a new frame's census. The reporter calls this after it has
    /// read the frame it wanted, so a window that reports every two seconds still
    /// prints one whole frame rather than a smear of several.</summary>
    public static void ResetCensus() { _censusCount = 0; _censusOrder = 0; _censusDropped = 0; }

    /// <summary>While false <see cref="Offset"/> and <see cref="OffsetMax"/> are not
    /// accumulated, so measuring the wobble costs nothing when nobody is asking.
    /// It is a separate switch from the counters above because the two probes
    /// report over their own windows and must not reset each other's numbers.</summary>
    public static bool Probe;

    /// <summary>How far the recorded vertices sat from the pixel they snapped to,
    /// summed and at worst, in pixels. Divided by <see cref="OffsetCount"/> the sum
    /// is the mean, and for positions spread evenly inside a pixel that mean is the
    /// mean distance from a corner of a unit square, 0.7652 — the number that says
    /// the recovered fraction is a real fraction and not a table full of zeroes.
    /// Clamped vertices are kept out of this sum, so the measurement stays about
    /// the 16.16 fraction and is not blown up by the unclamped remainder.</summary>
    public static double Offset;
    public static float OffsetMax;
    public static long OffsetCount;

    public static void ResetOffsets() { Offset = 0; OffsetMax = 0f; OffsetCount = 0; }

    /// <summary>0052, under <see cref="Probe"/>: polygons with a fraction on every corner,
    /// those under one square pixel, those with some corners lacking one, and those whose
    /// fractional corners wind opposite to their whole-pixel ones.</summary>
    public static long CensusPolys, CensusTiny, CensusMixed, CensusFlipped;

    // +1 so that 0 can mean "never written", which is what lets Collect stop early.
    static int KeyOf(int x, int y) => (((y & 0x7FF) << 11) | (x & 0x7FF)) + 1;

    static int Bucket(int key) => (int)(((uint)key * 2654435761u) >> (32 - Bits));

    /// <summary>Called from <c>Gte.Rtp</c> with the (possibly clamped) screen
    /// position the packet will carry, the view depth that produced it, and the
    /// GTE's true 16.16 position. <paramref name="z"/> is SZ3, the same quantity
    /// the divider used, so it is proportional to view-space W.
    /// <paramref name="clipped"/> is a vertex that saturated at ±1024.</summary>
    /// <summary>Called from <c>Gte.Rtp</c> for every vertex it projects, whether the
    /// fallback table is filling or not: this is the accounting the two probes read,
    /// and it has to describe what the GTE did rather than what one of the two
    /// mechanisms happened to keep.</summary>
    public static void NoteProjected(float fx, float fy, bool clipped, bool valid)
    {
        if (!valid) return;

        Recorded++;
        if (clipped) { Saturated++; return; }

        if (Probe)
        {
            float d = MathF.Sqrt(fx * fx + fy * fy);
            Offset += d;
            if (d > OffsetMax) OffsetMax = d;
            OffsetCount++;
        }
    }

    public static void Record(int x, int y, int z, float trueX, float trueY, bool clipped)
    {
        if (!Active || z <= 0) return;

        // On-screen: the 16.16 remainder in [0, 1). Clamped: leave the vertex on
        // the packet coordinate. Serving the true position past ±1024 opened a
        // hole along every shared edge whose other end was still on the wall.
        float fx = 0f, fy = 0f;
        if (!clipped)
        {
            fx = trueX - x;
            fy = trueY - y;
        }

        int key = KeyOf(x, y);
        int bucket = Bucket(key);
        long now = ++_seq;

        int copies = 0;
        int oldestSame = -1;
        long oldestSameSeq = long.MaxValue;
        int firstEmpty = -1;
        int oldestAny = bucket;
        long oldestAnySeq = long.MaxValue;

        for (int i = 0; i < ProbeLen; i++)
        {
            int s = (bucket + i) & Mask;
            ref var slot = ref _slots[s];
            bool stale = slot.Key == 0 || now - slot.Seq > MaxAge;
            if (stale)
            {
                if (firstEmpty < 0) firstEmpty = s;
            }
            else if (slot.Key == key)
            {
                copies++;
                if (slot.Seq < oldestSameSeq) { oldestSameSeq = slot.Seq; oldestSame = s; }
            }
            if (!stale && slot.Seq < oldestAnySeq) { oldestAnySeq = slot.Seq; oldestAny = s; }
        }

        int dest;
        if (copies < MaxCand && firstEmpty >= 0) dest = firstEmpty;
        else if (copies >= MaxCand && oldestSame >= 0) dest = oldestSame;
        else if (firstEmpty >= 0) dest = firstEmpty;
        else dest = oldestAny;

        _slots[dest] = new Slot { Key = key, Seq = now, Z = z, Fx = fx, Fy = fy };
    }

    /// <summary>The depth and sub-pixel position last projected to this screen
    /// position, if they are recent enough to belong to the geometry being drawn
    /// now. One probe of the table serves both halves; which of them the caller is
    /// allowed to use is <see cref="Enabled"/> and <see cref="Subpixel"/>.
    /// Prefer <see cref="Apply"/> at primitive decode — this is the single-vertex
    /// last-write path, kept for anything that is not a polygon.</summary>
    public static bool TryGet(int x, int y, out float z, out float fx, out float fy)
    {
        z = 0f; fx = 0f; fy = 0f;
        if (!Active) return false;

        Span<Sample> one = stackalloc Sample[1];
        if (Collect(x, y, one) == 0)
        {
            Misses++;
            return false;
        }

        z = one[0].Z;
        fx = one[0].Fx;
        fy = one[0].Fy;
        Hits++;
        return true;
    }

    /// <summary>Bind every vertex of a triangle or quad at once. Newest-at-the-key
    /// is the first guess; a key that has several samples is then rebound to the
    /// depth that sits with the rest of the primitive, and a leftover high outlier
    /// is dropped so the triangle stays affine rather than tearing.</summary>
    public static void Apply(Span<Attr> verts, bool wantW, bool wantSub)
    {
        int n = verts.Length;
        Span<Sample> pool = stackalloc Sample[MaxCand * 4];
        Span<int> count = stackalloc int[4];

        for (int i = 0; i < n; i++)
        {
            int c = Collect(verts[i].X, verts[i].Y, pool.Slice(i * MaxCand, MaxCand));
            count[i] = c;
            if (c > 0)
            {
                Hits++;
                ref readonly var s = ref pool[i * MaxCand];
                verts[i].Z = s.Z;
                verts[i].Fx = wantSub ? s.Fx : 0f;
                verts[i].Fy = wantSub ? s.Fy : 0f;
                verts[i].HasW = wantW && s.Z > 0f;
                verts[i].HasSub = wantSub;
            }
            else
            {
                Misses++;
                verts[i].Z = 1f;
                verts[i].Fx = 0f;
                verts[i].Fy = 0f;
                verts[i].HasW = false;
                verts[i].HasSub = false;
            }
        }

        if (n < 3) return;

        int hitN = 0;
        float logSum = 0f;
        for (int i = 0; i < n; i++)
        {
            if (count[i] == 0) continue;
            float z = pool[i * MaxCand].Z;
            if (z <= 0f) continue;
            logSum += MathF.Log(z);
            hitN++;
        }

        if (hitN >= 2)
        {
            float target = MathF.Exp(logSum / hitN);
            for (int i = 0; i < n; i++)
            {
                if (count[i] < 2) continue;
                int best = 0;
                float bestD = Rel(pool[i * MaxCand].Z, target);
                for (int k = 1; k < count[i]; k++)
                {
                    float d = Rel(pool[i * MaxCand + k].Z, target);
                    if (d < bestD) { bestD = d; best = k; }
                }
                if (best == 0) continue;
                Refined++;
                (pool[i * MaxCand], pool[i * MaxCand + best]) =
                    (pool[i * MaxCand + best], pool[i * MaxCand]);
                verts[i].Z = pool[i * MaxCand].Z;
                verts[i].HasW = wantW && verts[i].Z > 0f;
                // Position stays the newest-at-the-key fraction: a shared edge
                // must not move just because this triangle picked a different Z.
            }
        }

        if (!wantW) return;

        CheckTri(verts, count, pool, 0, 1, 2);
        if (n == 4) CheckTri(verts, count, pool, 1, 2, 3);
    }

    static void CheckTri(Span<Attr> v, Span<int> count, Span<Sample> pool,
        int ia, int ib, int ic)
    {
        if (!v[ia].HasW || !v[ib].HasW || !v[ic].HasW) return;
        int o = HighOutlier(v[ia].Z, v[ib].Z, v[ic].Z);
        if (o < 0) return;

        int idx = o == 0 ? ia : o == 1 ? ib : ic;
        float za = v[ia].Z, zb = v[ib].Z, zc = v[ic].Z;
        for (int k = 1; k < count[idx]; k++)
        {
            float z = pool[idx * MaxCand + k].Z;
            float na = idx == ia ? z : za, nb = idx == ib ? z : zb, nc = idx == ic ? z : zc;
            if (HighOutlier(na, nb, nc) >= 0) continue;

            Refined++;
            v[idx].Z = pool[idx * MaxCand + k].Z;
            return;
        }

        Rejected++;
        v[idx].HasW = false;
    }

    static int Collect(int x, int y, Span<Sample> dst)
    {
        int key = KeyOf(x, y);
        int bucket = Bucket(key);
        long now = _seq;
        Span<Sample> tmp = stackalloc Sample[ProbeLen];
        int n = 0;

        for (int i = 0; i < ProbeLen; i++)
        {
            ref var slot = ref _slots[(bucket + i) & Mask];
            if (slot.Key == 0) break;
            if (slot.Key != key) continue;
            if (now - slot.Seq > MaxAge) continue;
            tmp[n++] = new Sample { Z = slot.Z, Fx = slot.Fx, Fy = slot.Fy, Seq = slot.Seq };
        }

        for (int i = 1; i < n; i++)
        {
            var t = tmp[i];
            int j = i - 1;
            while (j >= 0 && tmp[j].Seq < t.Seq) { tmp[j + 1] = tmp[j]; j--; }
            tmp[j + 1] = t;
        }

        int outN = n < dst.Length ? n : dst.Length;
        for (int i = 0; i < outN; i++) dst[i] = tmp[i];
        return outN;
    }

    static float Rel(float z, float target)
    {
        if (z <= 0f || target <= 0f) return float.MaxValue;
        return z > target ? z / target : target / z;
    }

    // Returns 0/1/2 for the far vertex of (a,b,c), or -1 if the three depths
    // could belong on one surface.
    static int HighOutlier(float a, float b, float c)
    {
        int i0 = 0, i1 = 1, i2 = 2;
        float z0 = a, z1 = b, z2 = c;
        if (z0 > z1) { (z0, z1) = (z1, z0); (i0, i1) = (i1, i0); }
        if (z1 > z2) { (z1, z2) = (z2, z1); (i1, i2) = (i2, i1); }
        if (z0 > z1) { (z0, z1) = (z1, z0); (i0, i1) = (i1, i0); }
        if (z0 <= 0f || z1 <= 0f) return -1;
        float rHigh = z2 / z1;
        float rLow = z1 / z0;
        if (rHigh > OutlierRatio && rHigh > rLow * OutlierCliff) return i2;
        return -1;
    }
}
