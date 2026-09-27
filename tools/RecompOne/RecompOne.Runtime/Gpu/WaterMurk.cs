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
    public static float Distance = 200f;
    public static float R = 0.03f, G = 0.05f, B = 0.06f;

    /// <summary>The haze on the water itself: the reflection pass lays the colour
    /// over it as if the view crossed <see cref="HazeThickness"/> of water at the
    /// angle it meets the surface, 63% at this distance; 0 is none. A fixed
    /// thickness, not the floor's, so deep and bottomless water haze alike.</summary>
    public static float Haze = 2654f;
    public const float HazeThickness = 640f;

    /// <summary>The water's level by tile, from the map: world Y (down) and 1 where
    /// the tile has water, <see cref="GridSpan"/> squared pairs, row by tile Z. The
    /// port fills it and bumps <see cref="LevelGen"/>. The prim shader murks what it
    /// draws below it.</summary>
    public const int GridSpan = 80, TileUnits = 2048;
    public static float[]? Level;
    public static int LevelGen;

    /// <summary>The camera the frame now being drawn was walked with, as
    /// <see cref="WaterWaves"/> holds it (view = R (world - cam) + T): the rotation
    /// row by row, the camera's world position, the translation. The port bumps
    /// <see cref="ViewGen"/> with it, and with a slider.</summary>
    public static readonly float[] View = new float[15];
    public static bool ViewSet;
    public static int ViewGen;
}
