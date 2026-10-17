using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;

namespace Kf1;

/// <summary>
/// The back-face test at the corners the GPU draws, Verdite2's <c>0052</c> and
/// <c>QuadFaces</c>. Every writer calls NCLIP (<c>func_8004DE4C</c>) on three cached
/// whole-pixel screen words and keeps a face when it is positive. With sub-pixel on
/// that is no longer what is drawn: a face a pixel wide collapses to a line on whole
/// pixels, is dropped, and leaves a hole between neighbours drawn at their fractional
/// corners (the compass needle's slivers). So the decision is taken on the signed
/// area at the fractional corners, read from the vertex map at the cache words, both
/// ways; a corner with no fraction or clamped leaves whole pixels. NCLIP still runs in
/// the writers, so the GTE is left as the game leaves it.
///
/// A quad is decided by the game on its first triangle alone, so one whose first
/// triangle is a sliver is dropped while it plainly faces. It is decided here on the
/// area of the loop 0, 1, 3, 2 the GPU draws, at the fractional corners when all four
/// have one and on whole pixels otherwise, so that holds with sub-pixel off too.
///
///     KF1_SUBPIXEL_CULL=0    whole pixels even with sub-pixel on
///     KF1_QUAD_FACING=0      a quad on its first triangle, as the game does
///
/// The C# writers (<see cref="Nclip"/>, <see cref="Quad"/>, not under their verify)
/// and the retained arm and blended faces (<see cref="Faces"/>) take the same answer.
/// The retained map and models are culled by the GPU on their real geometry. See "A
/// thin face was culled on whole pixels" in docs/PICTURE.md.
/// </summary>
public static class Facing
{
    public static bool Enabled { get; set; } =
        Environment.GetEnvironmentVariable("KF1_SUBPIXEL_CULL")?.Trim() is not ("0" or "off");

    public static bool QuadEnabled { get; set; } =
        Environment.GetEnvironmentVariable("KF1_QUAD_FACING")?.Trim() is not ("0" or "off");

    /// <summary>Faces kept that the game's test would have dropped, and dropped that
    /// it would have kept; the sub-pixel probe reads and clears them.</summary>
    public static long Kept, Dropped;

    /// <summary>A triangle's NCLIP in a writer: the words at <paramref name="a"/>,
    /// <paramref name="b"/>, <paramref name="d"/>, already loaded into a0-a2; v0
    /// positive keeps the face.</summary>
    public static void Nclip(CpuContext c, PSMemory mem, Literal owner, uint a, uint b, uint d)
    {
        KingsField1_game.func_8004DE4C(c, mem);
        if (owner.InCSharp) Decide(c, Faces(mem, a, b, d));
    }

    /// <summary>A quad's NCLIP in a writer: its first triangle, then the loop with the
    /// fourth corner's cache word at <paramref name="e"/>.</summary>
    public static void Quad(CpuContext c, PSMemory mem, Literal owner, uint a, uint b, uint d, uint e)
    {
        KingsField1_game.func_8004DE4C(c, mem);
        if (owner.InCSharp) Decide(c, Faces(mem, a, b, d, e));
    }

    static void Decide(CpuContext c, bool keep)
    {
        if (keep == (int)c.V0 > 0) return;
        if (keep) Kept++; else Dropped++;
        c.V0 = keep ? 1u : 0u;
    }

    /// <summary>Whether the face with its corners' cache words at <paramref name="a"/>,
    /// <paramref name="b"/>, <paramref name="d"/> (and a quad's fourth at
    /// <paramref name="e"/>) faces the view.</summary>
    public static bool Faces(PSMemory mem, uint a, uint b, uint d, uint e = 0)
    {
        bool quad = e != 0 && QuadEnabled;
        Span<uint> at = quad ? [a, b, e, d] : [a, b, d];
        int n = at.Length;
        Span<float> x = stackalloc float[4], y = stackalloc float[4];
        bool frac = Enabled && GteDepth.Subpixel && GteVertexMap.Active;
        for (int i = 0; i < n; i++)
        {
            uint w = Word(mem, at[i]);
            x[i] = (short)w;
            y[i] = (short)(w >> 16);
            if (frac && GteVertexMap.Peek(at[i] & 0x1FFFFFFFu, w, out var v) && !v.Clipped)
            {
                x[i] += v.Fx;
                y[i] += v.Fy;
            }
            else frac = false;
        }
        if (!frac)
            for (int i = 0; i < n; i++) { uint w = Word(mem, at[i]); x[i] = (short)w; y[i] = (short)(w >> 16); }

        float area = 0f;
        for (int i = 0; i < n; i++)
        {
            int j = i + 1 == n ? 0 : i + 1;
            area += x[i] * y[j] - x[j] * y[i];
        }
        return area > 0f;
    }

    // Two halfword loads: a word load of a mapped address would offer it to the map.
    static uint Word(PSMemory mem, uint at) => mem.ReadU16(at) | (uint)mem.ReadU16(at + 2u) << 16;
}
