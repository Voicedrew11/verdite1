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

    /// <summary>Water with no floor drawn under it takes its depth from the map: per
    /// tile, the depth of the floor below the water's surface times a weight, and the
    /// weight (1 where the tile has a floor or borders one, 0 where the water is open
    /// and deep). <see cref="GridSpan"/> squared pairs, row by tile Z. The port fills
    /// it and bumps <see cref="GridGen"/>.</summary>
    public const int GridSpan = 80, TileUnits = 2048;
    public static float[]? Grid;
    public static int GridGen;

    /// <summary>The camera the frame now being drawn was walked with, as
    /// <see cref="WaterWaves"/> holds it (view = R (world - cam) + T): the rotation
    /// row by row, the camera's world position, the translation.</summary>
    public static readonly float[] View = new float[15];
    public static bool ViewSet;
}
