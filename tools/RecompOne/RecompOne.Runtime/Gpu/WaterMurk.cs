namespace RecompOne.Runtime;

/// <summary>
/// Water murk: the reflection pass lays a dark colour over water in proportion to
/// how much of it the view ray crosses. A translucent surface writes no depth, so
/// the depth buffer holds the floor under the water and the surface buffer the
/// water itself; the run between the two is the water crossed. Its own switch: it
/// needs the pass, not the screen march or either planar reflection.
/// </summary>
public static class WaterMurk
{
    static bool _on;

    public static bool Enabled
    {
        get => _on;
        set { _on = value; ScreenReflections.Refresh(); }
    }

    /// <summary>The distance through water, in world units, over which the floor
    /// under it fades to 63% of <see cref="R"/>/G/B.</summary>
    public static float Distance = 2654f;
    public static float R = 0.03f, G = 0.05f, B = 0.06f;
}
