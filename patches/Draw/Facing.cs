using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using Recompiled;

namespace Kf1;

/// <summary>
/// The polygon writers' back-face test at the corners the GPU draws, Verdite2's
/// <c>0052</c>. Every writer calls NCLIP (<c>func_8004DE4C</c>) on three cached
/// whole-pixel screen words and keeps a face when it is positive. With sub-pixel on
/// that is no longer what is drawn: a face a pixel wide collapses to a line on whole
/// pixels, is dropped, and leaves a hole between neighbours drawn at their fractional
/// corners (the compass needle's slivers). So the decision is taken on the signed
/// area at the fractional corners, read from the vertex map at the three cache words,
/// both ways; a corner with no fraction or clamped leaves the game's answer. NCLIP
/// still runs, so the GTE is left as the game leaves it. A quad is decided on its
/// first triangle, as the game does.
///
///     KF1_SUBPIXEL_CULL=0    the game's whole-pixel answer
///
/// Not under the writer's verify, which compares against the recompiled cull. See
/// "A thin face was culled on whole pixels" in docs/PICTURE.md.
/// </summary>
public static class Facing
{
    public static bool Enabled { get; set; } =
        Environment.GetEnvironmentVariable("KF1_SUBPIXEL_CULL")?.Trim() is not ("0" or "off");

    /// <summary>Faces kept that whole pixels would have dropped, and dropped that they
    /// would have kept; the sub-pixel probe reads and clears them.</summary>
    public static long Kept, Dropped;

    /// <summary>NCLIP of the words at <paramref name="a"/>, <paramref name="b"/>,
    /// <paramref name="d"/>, already loaded into a0-a2; v0 positive keeps the face.</summary>
    public static void Nclip(CpuContext c, PSMemory mem, Literal owner, uint a, uint b, uint d)
    {
        KingsField1_game.func_8004DE4C(c, mem);
        if (!Enabled || !GteDepth.Subpixel || !GteVertexMap.Active || !owner.InCSharp) return;
        if (!Corner(a, c.A0, out float x0, out float y0) || !Corner(b, c.A1, out float x1, out float y1)
            || !Corner(d, c.A2, out float x2, out float y2)) return;

        bool keep = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0) > 0f;
        if (keep == (int)c.V0 > 0) return;
        if (keep) Kept++; else Dropped++;
        c.V0 = keep ? 1u : 0u;
    }

    static bool Corner(uint at, uint word, out float x, out float y)
    {
        x = (short)word;
        y = (short)(word >> 16);
        if (!GteVertexMap.Peek(at & 0x1FFFFFFFu, word, out var v) || v.Clipped) return false;
        x += v.Fx;
        y += v.Fy;
        return true;
    }
}
