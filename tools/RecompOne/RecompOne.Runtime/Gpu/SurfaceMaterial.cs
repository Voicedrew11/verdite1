namespace RecompOne.Runtime;

/// <summary>
/// 0067. What a surface is made of, as far as a pass after the frame needs to know.
///
/// <para>The console had no such idea: a polygon is a colour, a texture and a blend
/// mode. A pass that runs on the finished frame — reflections now, lighting later —
/// needs one more fact per pixel, and it is kept in the alpha of the surface buffer
/// beside the normal and the depth (<c>GlCore.RenderSurfaces</c>). The id is small
/// on purpose; what an id *means* is the table below, which a port fills.</para>
///
/// <para>Two sources, in order. A packet the port built may carry its material in
/// its depth record (<see cref="GtePacketDepth.Rec.Material"/>) — that is where an
/// authored material belongs, since only the port knows which routine drew which
/// model or tile. Failing that, a textured polygon whose texels come from a VRAM
/// rectangle the port has published (<see cref="Rects"/>) takes that rectangle's
/// material; that is how water is found, because the game re-uploads its water into
/// fixed dest rects every tick (the same slots 0053 smooths).</para>
/// </summary>
public static class SurfaceMaterial
{
    /// <summary>Nothing drew a surface here.</summary>
    public const byte None = 0;
    /// <summary>Any opaque, depth-carrying polygon without a material of its own.</summary>
    public const byte Opaque = 1;
    /// <summary>A still or scrolling water surface.</summary>
    public const byte Water = 2;
    /// <summary>Drawn in 2D over the scene -- the HUD, text, a menu. Nothing
    /// reflects it, and a ray that lands under it has found nothing it can use.</summary>
    public const byte Overlay = 3;

    /// <summary>The table's size, and one past the largest id a pass will read: the
    /// surface buffer's half-float alpha holds every integer to 2048, and a packet's
    /// record holds a byte.</summary>
    public const int Count = 256;

    /// <summary>Ids a port may author; the ones below are the runtime's.</summary>
    public const byte FirstAuthored = 4;

    /// <summary>Added to a blended triangle's material in the surface list, so the
    /// normal pass takes opacity from the draw rather than from the id.</summary>
    public const float BlendedFlag = 256f;

    /// <summary>A see-through 2D primitive (a message box) leaves the surface under
    /// it and adds this to its id: <see cref="VeilHalf"/> for blend mode 0, which
    /// shows half of what is behind, <see cref="VeilFull"/> for the others. The
    /// water under it is murked and reflected at that share, not left bare.</summary>
    public const float VeilHalf = 512f, VeilFull = 1024f;

    /// <summary>How much of the scene a material reflects at most, 0..1; 0 is not
    /// reflective, and the pass skips the pixel after one texture read.</summary>
    public static readonly float[] Reflectivity = new float[Count];

    /// <summary>Schlick's F0: the share reflected looking straight down at it. The
    /// share rises to <see cref="Reflectivity"/> at a grazing angle.</summary>
    public static readonly float[] F0 = new float[Count];

    /// <summary>How far a reflection is blurred, 0..1: the spread of the cone the
    /// reflected ray stands for, as a share of the distance it travelled. The
    /// reflection pass averages the hit over that footprint.</summary>
    public static readonly float[] Roughness = new float[Count];

    /// <summary>Light a surface gives off, per id and channel, in the game's light
    /// units (1.0 adds the packet's own RGBC once, as an authored light does). Only a
    /// packet with a <see cref="GteLightMap"/> record glows.</summary>
    public static readonly float[] Emissive = new float[Count * 3];

    /// <summary>How an id's glow is added: true after the texture is modulated, so
    /// every texel lights, as a light source does; false to the lit colour before
    /// it, so the glow is the texture shown brighter. Fogged either way.</summary>
    public static readonly bool[] EmissiveAdditive = new bool[Count];

    /// <summary>An additive glow the depth cue does not darken: a lamp seen from
    /// across the dark.</summary>
    public static readonly bool[] EmissiveUnfogged = new bool[Count];

    /// <summary>How much a material's reflection and highlight take its own colour,
    /// 0..1: 0 is stone or water, 1 a metal.</summary>
    public static readonly float[] Metalness = new float[Count];

    /// <summary>The highlight an authored light leaves, 0..1; its size is the
    /// material's roughness.</summary>
    public static readonly float[] Specular = new float[Count];

    /// <summary>How much the occlusion pass darkens a material, 0..1; 1 is every
    /// id's until a port says otherwise.</summary>
    public static readonly float[] Occlusion = Filled(1f);

    static float[] Filled(float v)
    {
        var a = new float[Count];
        Array.Fill(a, v);
        return a;
    }

    /// <summary>Bumped by <see cref="Changed"/>; the table is uploaded to the GPU when
    /// it moves. A port that writes the arrays calls it once it is done.</summary>
    public static int Generation { get; private set; }

    /// <summary>Whether any id glows, so the prim shader can skip the lookup.</summary>
    public static bool AnyEmissive { get; private set; }

    /// <summary>Whether any id has a highlight, or is occluded other than fully.</summary>
    public static bool AnySpecular { get; private set; }
    public static bool AnyOcclusion { get; private set; }

    /// <summary>Whether any id's reflection is blurred, so the pass builds the mip
    /// chains its blur reads.</summary>
    public static bool AnyRoughness { get; private set; }

    /// <summary>Uploads of the table to the GPU; never reset.</summary>
    public static long Uploads;

    public static void Changed()
    {
        bool any = false;
        foreach (float e in Emissive) if (e > 0f) { any = true; break; }
        AnyEmissive = any;
        AnySpecular = Array.Exists(Specular, v => v > 0f);
        AnyOcclusion = Array.Exists(Occlusion, v => v != 1f);
        AnyRoughness = Array.Exists(Roughness, v => v > 0f);
        Generation++;
    }

    /// <summary>A VRAM rectangle whose texels are one material.</summary>
    public struct TexRect
    {
        public int X, Y, W, H;
        public byte Material;
        /// <summary>Only a semi-transparent polygon, in an averaging blend, takes
        /// it: the same slots also hold the main-hall fire (additive) and the
        /// creatures' skins.</summary>
        public bool TranslucentOnly;
    }

    public const int RectSlots = 16;
    public static readonly TexRect[] Rects = new TexRect[RectSlots];
    public static int RectN;

    /// <summary>Triangles given a material other than Opaque, by where it came
    /// from. Reset by the probe.</summary>
    public static long FromPacket, FromRect;
    public static readonly long[] ByMaterial = new long[Count];

    /// <summary>Blended, depth-carrying triangles looked at; those whose texture
    /// was in a translucent-only rect and whose blend refused it (the fire), by
    /// blend mode.</summary>
    public static long Blended;

    /// <summary>2D triangles kept as <see cref="Overlay"/>.</summary>
    public static long Overlays;
    /// <summary>2D triangles kept as a veil, and see-through textured ones kept as
    /// <see cref="Overlay"/> (a texel may be opaque, which the normal pass cannot see).</summary>
    public static long Veils, TexturedVeils;
    public static readonly long[] RefusedByBlend = new long[4];

    public static void ResetCounters()
    {
        FromPacket = FromRect = Blended = Overlays = Veils = TexturedVeils = 0;
        Array.Clear(ByMaterial);
        Array.Clear(RefusedByBlend);
    }

    /// <summary>The material of a triangle: the packet's, then a published
    /// rectangle's, then <see cref="Opaque"/> for an opaque one and
    /// <see cref="None"/> for a blended one.</summary>
    public static byte Classify(byte packet, bool textured, bool semi, int blend, int tpage,
                                int u0, int v0, int u1, int v1)
    {
        if (packet != None) { FromPacket++; ByMaterial[packet]++; return packet; }
        if (semi) Blended++;
        if (textured && RectN > 0)
        {
            // The texel columns the UVs reach, in VRAM halfwords: a 4-bit texel is a
            // quarter of one and an 8-bit texel a half.
            int mode = (tpage >> 7) & 3;
            int div = mode == 0 ? 4 : mode == 1 ? 2 : 1;
            int x0 = (tpage & 0xF) * 64 + u0 / div, x1 = (tpage & 0xF) * 64 + u1 / div + 1;
            int y0 = ((tpage >> 4) & 1) * 256 + v0, y1 = ((tpage >> 4) & 1) * 256 + v1 + 1;
            bool averaging = semi && (blend == 0 || blend == 3);
            for (int i = 0; i < RectN; i++)
            {
                ref var r = ref Rects[i];
                if (x0 >= r.X + r.W || x1 <= r.X || y0 >= r.Y + r.H || y1 <= r.Y) continue;
                if (r.TranslucentOnly && !averaging)
                {
                    if (semi) RefusedByBlend[blend & 3]++;
                    continue;
                }
                FromRect++;
                ByMaterial[r.Material]++;
                return r.Material;
            }
        }
        return semi ? None : Opaque;
    }
}
