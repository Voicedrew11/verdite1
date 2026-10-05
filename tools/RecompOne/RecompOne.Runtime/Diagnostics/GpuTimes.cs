namespace RecompOne.Runtime.Diagnostics;

/// <summary>
/// 0084. GPU time per present, by pass. While <see cref="Enabled"/>, the GL backend
/// puts a GL_TIME_ELAPSED query around each batch submit and each present pass, and
/// reads them back without waiting once the GPU has finished them, a few presents
/// late. A frame capture's own queries (0046) take precedence: nothing is counted
/// while a trace sink is set. What no query covers -- VRAM uploads, writebacks, the
/// interface, the swap -- is not counted.
/// </summary>
public static class GpuTimes
{
    public enum Pass : byte
    {
        /// <summary>Batches drawn into a display target or VRAM: the frame.</summary>
        Scene,
        /// <summary>Batches drawn into a planar texture (0068).</summary>
        Capture,
        /// <summary>The surface buffer and the occlusion pass.</summary>
        Ao,
        /// <summary>The retained scene's draws and the reflection pass.</summary>
        Reflections,
        /// <summary>The present blit and any post-fx.</summary>
        Composite,
        /// <summary>0085. The retained map drawn into the frame.</summary>
        World,
    }

    public const int Passes = 6;
    public static readonly string[] Names = ["scene", "capture", "ao", "reflections", "composite", "world"];

    public static bool Enabled;
    /// <summary>Set by a backend that has timer queries.</summary>
    public static bool Supported;

    /// <summary>Nanoseconds resolved per pass, presents whose queries have resolved,
    /// and queries thrown away unread; never reset, so a reader takes deltas.</summary>
    public static readonly long[] Ns = new long[Passes];
    public static long Presents, Dropped;

    /// <summary>The present being issued now (a serial from 0), and the last one whose
    /// queries have all resolved (-1 for none), so a reader can tell a frame whose GPU
    /// time is still arriving from one that is done.</summary>
    public static long Issued, Complete = -1;

    /// <summary>Each query as it resolves: the present it was issued in, its pass and
    /// its nanoseconds. Raised on the thread that presents.</summary>
    public static event Action<long, Pass, long>? Resolved;

    public static void Resolve(long present, Pass pass, long ns)
    {
        Ns[(int)pass] += ns;
        Resolved?.Invoke(present, pass, ns);
    }
}
