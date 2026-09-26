namespace RecompOne.Runtime;

/// <summary>
/// 0078. Ripples on water, drawn per pixel by the prim shader: the texture coordinate
/// is pushed by the slope of a moving wave field, and the texel lightened or darkened
/// by it, so the one 64x64 image every water tile repeats no longer reads as a grid.
///
/// The field is anchored in the world, not the texture: the shader rebuilds the
/// fragment's view position from its recovered depth, the H and the centre, as
/// <c>NormalFs</c> does, and takes it to world space with the camera the port
/// publishes. Water is a fragment whose texel lies in one of the port's rectangles
/// (the scrolling textures' dest rects), drawn in a batch the port says is water's
/// blend; the offset texel wraps inside that rectangle, as the upload itself does.
///
/// Nothing is drawn differently with <see cref="Enabled"/> off, into a planar
/// reflection, or for a fragment with no recovered depth. GL core only. Nothing here
/// writes guest memory or the GTE.
/// </summary>
public static class WaterWaves
{
    public const int MaxRects = 8;

    /// <summary>The port's switch.</summary>
    public static bool Enabled;

    /// <summary>The backend can draw it: the core-profile prim shader has the uniforms.</summary>
    public static bool Supported;

    public static bool Active => Enabled && Supported && RectN > 0;

    /// <summary>The water's VRAM rectangles, in halfwords: x, y, w, h.</summary>
    public static readonly float[] Rects = new float[MaxRects * 4];
    public static int RectN;

    /// <summary>The camera the frame's geometry was drawn with: world to view,
    /// row-major, and the view translation and world position it was taken about,
    /// so view = R (world - Cam) + T.</summary>
    public static readonly float[] R = new float[9];
    public static float CamX, CamY, CamZ, Tx, Ty, Tz;

    /// <summary>The field's clock, in seconds at its authored speed: the port runs it
    /// with the world and scales it, so a change of speed does not jump the phase.</summary>
    public static float Time;

    /// <summary>How far the texture is pushed at the steepest slope, in world units (a
    /// water texel is 32); the wavelength of the longest ripple, in world units (a tile
    /// is 2048); and how much the slope lightens and darkens, 0-1.</summary>
    public static float Distort = 48f, Scale = 700f, Shade = 0.25f;

    /// <summary>Bumped by the port when anything above changes; the backend sends the
    /// uniforms again only then.</summary>
    public static int Generation;

    /// <summary>Batches drawn with the ripples on; never reset.</summary>
    public static long Batches;
}
