namespace RecompOne.Runtime;

/// <summary>
/// 0071. What the port's remaster hands the prim shader beyond the game's own state:
/// for now, authored lights.
///
/// A light is one more term in <c>shade8</c>'s lit colour, added before the depth cue
/// and before the texture is modulated, so the game's own fog, texture and saturation
/// apply to it exactly as to the game's light. The port publishes each light already
/// in the GTE's view space (X right, Y down, Z into the screen, world units), since it
/// has the camera and the shader has only the fragment: the shader rebuilds the
/// fragment's view position from its recovered depth, the H and the centre, as
/// <c>NormalFs</c> does, and its normal from that position's screen derivatives.
///
/// A light reaches only a packet with a <see cref="GteLightMap"/> record, whose low
/// bytes are the colour the game lit it with (RGBC); the HUD and anything unrecorded
/// keep their vertex colour. None is drawn into a planar reflection's texture. GL
/// core only. Nothing here writes guest memory or the GTE.
/// </summary>
public static class RemasterUniforms
{
    public const int MaxLights = 16;

    /// <summary>The port's switch.</summary>
    public static bool Enabled;

    /// <summary>The backend can draw it: the core-profile prim shader has the uniforms.</summary>
    public static bool Supported;

    public static bool Active => Enabled && Supported && LightCount > 0;

    /// <summary>Per light, four floats each. Pos: view position and radius. Col: colour
    /// times intensity, in the game's light units (1.0 adds the packet's own RGBC once),
    /// and the cosine of the spot's inner cone. Dir: the spot's view direction and the
    /// cosine of its outer cone; a point light's outer cosine is -2, or -2 - id for a
    /// light a material gives off, which leaves that material unlit by it.</summary>
    public static readonly float[] LightPos = new float[MaxLights * 4];
    public static readonly float[] LightCol = new float[MaxLights * 4];
    public static readonly float[] LightDir = new float[MaxLights * 4];

    public static int LightCount { get; private set; }

    /// <summary>Bumped by <see cref="Publish"/>; a batch drawn under one generation is
    /// flushed before a primitive of the next is added.</summary>
    public static int Generation { get; private set; }

    /// <summary>The arrays hold <paramref name="count"/> lights for the frame about to be drawn.</summary>
    public static void Publish(int count)
    {
        LightCount = Math.Clamp(count, 0, MaxLights);
        Generation++;
    }

    /// <summary>Batches drawn with lights, and light-list uploads; never reset.</summary>
    public static long LitBatches, Uploads;

    // ---- 0077. Shadows ---------------------------------------------------------

    /// <summary>0077. Lights that cast shadows, each a depth cubemap the backend draws
    /// from the retained map (<see cref="RetainedScene"/>) with the light at its centre,
    /// again only when the light or the map changes.</summary>
    public const int MaxShadows = 4;

    /// <summary>Per light in the list, the shadow slot it samples, or -1 for none.</summary>
    public static readonly int[] LightShadow = Enumerable.Repeat(-1, MaxLights).ToArray();

    /// <summary>Per slot, the light's world position and radius; a slot whose radius
    /// is 0 is unused.</summary>
    public static readonly float[] ShadowLight = new float[MaxShadows * 4];

    /// <summary>The frame's world-to-view rotation, row-major, published with the
    /// lights: the shader turns a view-space offset from a light back into world axes
    /// with its transpose to look the cubemap up.</summary>
    public static readonly float[] ToWorld = new float[9];

    /// <summary>A cubemap face's size in texels; how far the receiver is moved off its
    /// surface along its normal, in texels at its distance; the constant bias on the
    /// compare, in world units; and the spread of the fixed filter taps, in texels.</summary>
    public static int ShadowSize = 1024;
    public static float ShadowOffset = 1.5f, ShadowBias = 6f, ShadowSoft = 1.25f;

    /// <summary>Cubemaps drawn, and triangles drawn into them; never reset.</summary>
    public static long ShadowRenders, ShadowTriangles;

    /// <summary>The retained frame (<see cref="RetainedScene.Serial"/>) whose models
    /// cast, published with the lights, once the frame's walk has submitted them all.</summary>
    public static int ShadowFrame;

    /// <summary>Cubemaps drawn again for their models, the model triangles drawn into
    /// them, and the triangles in some light's reach on the last frame; never reset but
    /// the last.</summary>
    public static long ShadowModelRenders, ShadowModelTriangles;
    public static int ShadowCasters;

    /// <summary>Slots whose cubemap is drawn for the light it holds now.</summary>
    public static int ShadowsReady;
}
