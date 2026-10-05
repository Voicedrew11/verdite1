namespace RecompOne.Runtime.Pgxp;

/// <summary>
/// Not upstream's. PGXP answers or it does not, and the difference between the two
/// mechanisms this port carries is entirely a difference in <i>how often</i> they
/// answer — so the coverage has to be countable or there is no way to say whether
/// the backport was worth taking.
///
/// Counted where the answer is used (<c>Gpu.ApplyPgxp</c>) rather than inside PGXP,
/// so a vertex refused by the tolerance guard is a miss here even though PGXP found
/// something for it. Free when nothing reads them: they are plain longs on a path
/// that already exists, and the port's probe resets them each window.
/// </summary>
public static class PgxpStats
{
    /// <summary>Vertices asked about.</summary>
    public static long Asked;

    /// <summary>Answered out of the RAM shadow: this word, at this address, put
    /// there by a tracked store. The exact answer.</summary>
    public static long FromMemory;

    /// <summary>Answered out of the screen-position vertex cache, which is keyed on
    /// where the vertex landed rather than on which vertex it is.</summary>
    public static long FromCache;

    /// <summary>Recovered by the per-primitive ambiguity pass, using a sibling
    /// vertex's sequence number to pick between a cell's candidates.</summary>
    public static long Resolved;

    /// <summary>Found, then thrown away for disagreeing with the packet coordinate
    /// by more than the tolerance. A rising number here means PGXP is answering with
    /// the wrong vertex, which is worth knowing before the picture is blamed.</summary>
    public static long Refused;

    /// <summary>Answered with a position but no usable view depth, so the polygon
    /// stays affine and untested even though its position improved.</summary>
    public static long NoDepth;

    /// <summary>
    /// How far each recovered position sat from the coordinate in the packet,
    /// bucketed, and taken **before** the tolerance test rather than after — the
    /// point is to see both populations at once.
    ///
    /// PGXP recovering *this* vertex more precisely can only disagree by the
    /// fraction the GTE truncated, so it lands under one pixel. PGXP recovering
    /// *a different* vertex can disagree by anything. The right tolerance is the
    /// gap between the two, and if there is no gap there is no safe tolerance and
    /// the answer is that the guard is doing something other than what it claims.
    ///
    /// Buckets are &lt;0.5, &lt;1, &lt;2, &lt;4, &lt;8, &gt;=8 pixels, on the larger of the two axes.
    /// </summary>
    public static readonly long[] Disagree = new long[6];

    /// <summary>The largest disagreement seen in the window, in pixels.</summary>
    public static float DisagreeMax;

    public static void NoteDisagreement(float dx, float dy)
    {
        float d = Math.Max(Math.Abs(dx), Math.Abs(dy));
        if (d > DisagreeMax) DisagreeMax = d;

        int bucket = d < 0.5f ? 0 : d < 1f ? 1 : d < 2f ? 2 : d < 4f ? 3 : d < 8f ? 4 : 5;
        Disagree[bucket]++;
    }

    public static long Hits => FromMemory + FromCache + Resolved;

    public static void Reset()
    {
        Asked = FromMemory = FromCache = Resolved = Refused = NoDepth = 0;
        Array.Clear(Disagree);
        DisagreeMax = 0f;
    }
}
