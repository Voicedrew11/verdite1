namespace RecompOne.Runtime;

/// <summary>
/// 0089. The main view's map and models faded out by **horizontal** distance from the
/// camera, per pixel, through the world program's ordered dither (0072's
/// <c>vFade</c>): no blending, and a pixel that is drawn keeps its depth. Turning on
/// the spot fades nothing, and a model and the ground under it fade together, since
/// both are weighed by their world X and Z.
///
/// <para>The weight is <c>clamp((edge - d) / band, 0, 1)</c>, d the distance from the
/// camera's X and Z; band 0 is no distance fade. Past <c>depth</c> - 2048 of view depth
/// the weight falls to 0 by <c>depth</c> as well, so nothing reaches the depth the
/// world program's 16 bits stop at (65,536, where every surface is the far plane).
/// Depth 0 is no limit.</para>
///
/// <para>The port sets a frame's fade with <see cref="RetainedScene.SetFade"/>; the
/// main view and its normal pass take it, the reflections and the arm never do. A frame
/// with no fade draws what it drew before. The sky's models (<c>uModelSky</c>) and a
/// model placed in view space (<c>uModelView</c>, the arm) stand at the camera for it.
/// </para>
/// </summary>
public static class DistanceFade
{
    /// <summary>The view depth over which the depth limit fades a pixel out.</summary>
    public const float DepthBand = 2048f;

    /// <summary>A depth limit that fades out before the world program's 65,536.</summary>
    public const float DepthLimit = 65024f;

    /// <summary>The CPU reference: <see cref="Glsl"/>'s <c>distanceFade</c>.</summary>
    public static float Weight(float dx, float dz, float z, float edge, float band, float depth)
    {
        float k = 1f;
        if (band > 0f) k = Math.Clamp((edge - MathF.Sqrt(dx * dx + dz * dz)) / band, 0f, 1f);
        if (depth > 0f) k = Math.Min(k, Math.Clamp((depth - z) / DepthBand, 0f, 1f));
        return k;
    }

    /// <summary>The CPU reference: whether <see cref="Glsl"/>'s <c>fadeDropped</c> drops
    /// a pixel at <paramref name="x"/>, <paramref name="y"/> at weight
    /// <paramref name="fade"/>.</summary>
    public static bool Dropped(float fade, int x, int y) =>
        fade < 1f && (Order[(y & 3) * 4 + (x & 3)] + 4 + 0.5f) / 8f > fade;

    static readonly int[] Order = [-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2];

    /// <summary>For PrimFs and NormalFs: the camera's X and Z, the edge and the band in
    /// <c>uFade</c>, the depth limit in <c>uFadeZ</c>; all 0 is off. <c>fadeDropped</c> is
    /// PrimFs's ordered dither, the crosshatch's table.</summary>
    public const string Glsl = """
        uniform vec4  uFade;
        uniform float uFadeZ;
        float distanceFade(vec2 xz, float z) {
            float k = 1.0;
            if (uFade.w > 0.0) k = clamp((uFade.z - length(xz - uFade.xy)) / uFade.w, 0.0, 1.0);
            if (uFadeZ > 0.0) k = min(k, clamp((uFadeZ - z) / 2048.0, 0.0, 1.0));
            return k;
        }
        const int fadeOrder[16] = int[16](
            -4,  0, -3,  1,
             2, -2,  3, -1,
            -3,  1, -4,  0,
             3, -1,  2, -2 );
        bool fadeDropped(float fade) {
            if (fade >= 1.0) return false;
            ivec2 fp = ivec2(gl_FragCoord.xy) & 3;
            return (float(fadeOrder[fp.y * 4 + fp.x] + 4) + 0.5) / 8.0 > fade;
        }
        """;
}
