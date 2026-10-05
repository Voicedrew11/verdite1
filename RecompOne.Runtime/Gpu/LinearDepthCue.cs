namespace RecompOne.Runtime;

/// <summary>A depth-linear cue using quantised quarter-depth and a near/far pair.
/// Both retained shaders compose this same function; no game addresses or policy.</summary>
public static class LinearDepthCue
{
    public const uint Curve = 5;
    public static float Weight(float depth, float near, float far)
    {
        if (near >= 32000 || near == far) return 0;
        if (near == MathF.Truncate(near) && far == MathF.Truncate(far)
            && Math.Abs(near) <= 65535 && Math.Abs(far) <= 65535 && Math.Abs(depth) <= 65535)
            return Math.Clamp(((int)MathF.Floor(depth / 4) - (int)MathF.Floor(near / 4)) * 16384
                / ((int)far - (int)near), 0, 7951);
        return Math.Clamp(MathF.Truncate((MathF.Floor(depth / 4) - MathF.Floor(near / 4)) * 16384 / (far - near)), 0, 7951);
    }
    public const string Glsl = """
        float linearDepthCue(float depth, vec2 pair) {
            if (pair.x >= 32000.0 || pair.x == pair.y) return 0.0;
            // Exact integer records must not use the driver's approximate float
            // reciprocal: an exact boundary can otherwise become one unit short.
            if (all(equal(pair, trunc(pair))) && all(lessThanEqual(abs(pair), vec2(65535.0))) && abs(depth) <= 65535.0) {
                int numerator = (int(floor(depth * 0.25)) - int(floor(pair.x * 0.25))) * 16384;
                return float(clamp(numerator / (int(pair.y) - int(pair.x)), 0, 7951));
            }
            return clamp(trunc((floor(depth * 0.25) - floor(pair.x * 0.25)) * 16384.0 / (pair.y - pair.x)), 0.0, 7951.0);
        }
        """;
}
