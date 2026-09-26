namespace RecompOne.Runtime;

/// <summary>
/// 0067. Screen-space reflections: the settings the pass reads, and its counters.
///
/// <para>The pass runs at present beside the occlusion pass and reads what that one
/// reads — the finished frame's depth, recovered from the GTE — plus the surface
/// buffer (normal, depth and material per pixel, <c>GlCore.RenderSurfaces</c>) and
/// the finished colour. For every pixel whose material reflects, it reflects the view
/// ray about the surface's own plane and marches it through the depth buffer; the
/// colour where it lands is blended over the surface by a Fresnel term. Nothing is
/// written back: like the occlusion, the reflection lives in its own texture and the
/// present composites it, so VRAM, the display buffers and a menu's restored frame
/// never carry it.</para>
///
/// <para>Water is the reason it has to be a surface buffer and not the depth buffer.
/// The game draws water semi-transparent, so it writes no depth and the depth under a
/// pool is the pool's floor; the surface buffer holds the water's own plane, drawn in
/// the same painter's order as everything else.</para>
/// </summary>
public static class ScreenReflections
{
    static bool _march;

    /// <summary>The screen-space march. The pass itself also runs for the murk and
    /// the planar and retained reflections, each on its own switch.</summary>
    public static bool Enabled
    {
        get => _march;
        set { _march = value; Refresh(); }
    }

    /// <summary>The pass runs, and the frame keeps its surfaces, while any of its
    /// terms is on.</summary>
    public static void Refresh() =>
        GteDepth.Reflections = _march || WaterMurk.Enabled || PlanarReflections.Enabled || RetainedScene.Enabled;

    /// <summary>How far a ray is marched, in the game's world units (a floor tile is
    /// 2048); 0 marches to where the game's fog turns a colour black, so whatever
    /// lies past the march's end would have reflected black anyway.</summary>
    public static float MaxDistance;

    /// <summary>The march used for this frame: <see cref="MaxDistance"/>, or the
    /// depth the fog is black at on <see cref="FogCurve"/>, capped at 16 tiles.</summary>
    public static float March()
    {
        if (MaxDistance > 0f) return MaxDistance;
        float z = FogBlackDepth();
        return float.IsFinite(z) ? Math.Clamp(z, 4096f, 32768f) : 16384f;
    }

    /// <summary>The view depth at which the depth cue leaves nothing of a colour, or
    /// infinity when this curve never gets there.</summary>
    public static float FogBlackDepth()
    {
        float ir0 = FogCurve switch { 1 => 2848f, 2 => 3232f, 4 => 4096f, _ => float.NaN };
        if (float.IsNaN(ir0) || GteDepth.ProjDqa >= 0) return float.PositiveInfinity;
        // IR0 = (DQA * q + DQB) / 4096 with q = H * 65536 / z.
        double q = (ir0 * 4096.0 - GteDepth.ProjDqb) / GteDepth.ProjDqa;
        return q <= 0 ? float.PositiveInfinity : (float)(GteDepth.ProjH * 65536.0 / q);
    }

    /// <summary>Steps along the ray; the spacing grows with distance, so the near
    /// field is sampled finely.</summary>
    public static int Steps = 32;

    /// <summary>How far behind the depth buffer a ray may be and still have hit it,
    /// in world units, on top of the step's own length.</summary>
    public static float Thickness = 256f;

    /// <summary>A ray that finds nothing takes the colour of the last background
    /// pixel it crossed (the skybox, which writes no depth) at this weight, 0..1.</summary>
    public static float Sky = 1f;

    /// <summary>Which of the game's depth-cue curves fogs a reflection's longer
    /// path (GteLightMap's numbering: 0 none, 1 offset, 2 knee, 3 half, 4 the word
    /// itself). The map tiles near the camera are on the knee; the curve is per
    /// record in the game, and one curve is the approximation.</summary>
    public static int FogCurve = 2;

    /// <summary>The pass's resolution: a multiple of the game's pixels, capped at the
    /// render scale; 0 is the render scale.</summary>
    public static int Resolution = 2;

    /// <summary>The probe's switch; <see cref="WantMap"/> asks for one readback.</summary>
    public static bool Probe;
    public static bool WantMap;

    /// <summary>Passes run, presents that could not run one, and surface passes drawn.</summary>
    public static long Passes, NoTarget;

    /// <summary>Mip chains built for a rough reflection; never reset.</summary>
    public static long MipBuilds;

    /// <summary>The last readback: the share of the picture that reflects, the share
    /// of those that found a surface, the share that took the sky, and the mean
    /// weight blended over a reflective pixel.</summary>
    public static float ReflectivePct, HitPct, SkyPct, OverlayPct, PassedPct, MeanWeight;

    public static void ResetCounters() => Passes = NoTarget = 0;

    /// <summary>The readback's census, from RGBA8 texels: alpha is the weight, and
    /// the probe build writes 1/255 into a reflective pixel's alpha whatever it
    /// found, so a miss is still counted.</summary>
    public static void SetMap(byte[] rgba, byte[] info, int w, int h)
    {
        LastInfo = info; LastW = w; LastH = h; MapSerial++;
        long n = (long)w * h, refl = 0, hit = 0, sky = 0, under = 0, passed = 0, planar = 0;
        double wsum = 0, skyR = 0, skyG = 0, skyB = 0, hitL = 0, keepSum = 0;
        long fogged = 0, compared = 0;
        double mirrorDiff = 0, controlDiff = 0;
        for (long i = 0; i < n; i++)
        {
            int k = info[i * 4 + 3];
            if (k == 0) continue;
            refl++;
            if ((k & 8) != 0) passed++;
            if ((k & 16) != 0)
            {
                compared++;
                mirrorDiff += info[i * 4 + 2] / 255.0;
                controlDiff += info[i * 4 + 1] / 255.0;
            }
            k &= 7;
            if (k == 2)
            {
                hit++;
                float a = Math.Max(rgba[i * 4 + 3], (byte)1);
                hitL += (rgba[i * 4] + rgba[i * 4 + 1] + rgba[i * 4 + 2]) / (3f * a);
                keepSum += info[i * 4 + 2] / 255.0;
                if (info[i * 4 + 2] < 128) fogged++;
            }
            else if (k == 3)
            {
                sky++;
                // Premultiplied: divide the weight back out for the colour taken.
                float a = Math.Max(rgba[i * 4 + 3], (byte)1);
                skyR += rgba[i * 4] / a; skyG += rgba[i * 4 + 1] / a; skyB += rgba[i * 4 + 2] / a;
            }
            else if (k == 4) under++;
            else if (k == 5) planar++;
            wsum += rgba[i * 4 + 3] / 255.0;
        }
        ReflectivePct = n == 0 ? 0f : 100f * refl / n;
        HitPct = refl == 0 ? 0f : 100f * hit / refl;
        SkyPct = refl == 0 ? 0f : 100f * sky / refl;
        OverlayPct = refl == 0 ? 0f : 100f * under / refl;
        PassedPct = refl == 0 ? 0f : 100f * passed / refl;
        PlanarReflections.PlanarPct = refl == 0 ? 0f : 100f * planar / refl;
        PlanarReflections.ComparedPct = planar == 0 ? 0f : 100f * compared / planar;
        PlanarReflections.MirrorDiff = compared == 0 ? 0f : (float)(mirrorDiff / compared * 255);
        PlanarReflections.ControlDiff = compared == 0 ? 0f : (float)(controlDiff / compared * 255);
        SkyColour = sky == 0 ? "none" : $"{skyR / sky * 255:F0},{skyG / sky * 255:F0},{skyB / sky * 255:F0}";
        HitLuma = hit == 0 ? 0f : (float)(hitL / hit * 255);
        HitKeep = hit == 0 ? 1f : (float)(keepSum / hit);
        HitFoggedPct = hit == 0 ? 0f : 100f * fogged / hit;
        MeanWeight = refl == 0 ? 0f : (float)(wsum / refl);
        // A 48x16 map: the material in each cell (the most common one), and
        // whether the depth there is the far plane.
        var sb = new System.Text.StringBuilder();
        const int cols = 48, rows = 16;
        // Row 0 of the texture is the top of the picture.
        for (int r = 0; r < rows; r++)
        {
            sb.Append("[KF2] reflections:  ");
            for (int c = 0; c < cols; c++)
            {
                Span<int> votes = stackalloc int[9];
                for (int y = r * h / rows; y < (r + 1) * h / rows; y += 2)
                    for (int x = c * w / cols; x < (c + 1) * w / cols; x += 2)
                    {
                        long i = ((long)y * w + x) * 4;
                        int m = Math.Min((int)info[i], 7);
                        votes[m == 0 && info[i + 1] != 0 ? 8 : m]++;
                    }
                int best = 0;
                for (int k = 1; k < 9; k++) if (votes[k] > votes[best]) best = k;
                sb.Append(best switch { 0 => ' ', 1 => '.', 2 => '~', 3 => 'H', 8 => '*', _ => '?' });
            }
            sb.Append('\n');
        }
        Map = sb.ToString();
        WantMap = false;
    }

    /// <summary>The last readback's map: '.' opaque, '~' water, 'H' 2D overlay,
    /// '*' far plane with no surface (the sky), ' ' nothing.</summary>
    public static string? Map;

    /// <summary>The last readback's info texels (red is the material), top row first.</summary>
    public static byte[]? LastInfo;
    public static int LastW, LastH;
    public static long MapSerial;

    /// <summary>The mean colour the sky fallback took, and the mean brightness of
    /// what a hit took, both 0..255.</summary>
    public static string SkyColour = "none";
    public static float HitLuma;

    /// <summary>What the path fog left of a hit's colour, on average, and the share
    /// of hits it more than halved.</summary>
    public static float HitKeep = 1f, HitFoggedPct;
}
