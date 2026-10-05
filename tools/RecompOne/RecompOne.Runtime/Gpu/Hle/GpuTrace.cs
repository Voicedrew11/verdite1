namespace RecompOne.Runtime.Hle;

/// <summary>Why the GL backend submitted a batch. The state reasons name the first
/// field <c>GlCore.DesiredMatches</c> found different.</summary>
public enum FlushReason : byte
{
    Other,
    Target,
    Full,
    TextureFeedback,
    Fill,
    Copy,
    Upload,
    Readback,
    Present,
    StateReplacement,
    StateSemi,
    StateBlend,
    StateImage,
    StateDepthMode,
    StateMask,
    StateTexWindow,
    StateClip,
    StateLight,
}

/// <summary>0046. Receives the live GPU's command stream and the backend's batch
/// submits, for a frame capture. Called on the game thread; null when nothing is
/// capturing, which is the only cost when off.</summary>
public interface IGpuTrace
{
    /// <summary>A word written to GP0, and the guest address it was read from (0 if none).</summary>
    void Word(uint word, uint src);
    void Gp1(uint word);
    /// <summary>The words since the last call made one complete command, and it has run.</summary>
    void Executed();
    void Flush(FlushReason why, int verts);
    void Flushed();
    /// <summary>A polygon's vertices were looked up for perspective, sub-pixel or
    /// depth: how many were asked for, and how many recovered their attributes.</summary>
    void Vertices(int asked, int hits);
    /// <summary>Backend work between two timestamps, with a GL_TIME_ELAPSED query
    /// around it (0 if timer queries are unavailable). A <see cref="GpuWork.Batch"/>
    /// belongs to the flush in progress and carries no times of its own.</summary>
    void Work(GpuWork what, long start, long end, uint query);
}

/// <summary>Backend work a capture times on the GPU as well as the CPU.</summary>
public enum GpuWork : byte
{
    Batch,
    AmbientOcclusion,
    /// <summary>The present blit and any post-fx.</summary>
    Composite,
    /// <summary>The whole of <c>GlCore.PresentDisplay</c>; CPU only, and the last
    /// call a present makes.</summary>
    Present,
    /// <summary>0067. The surface buffer and the reflection pass.</summary>
    Reflections,
}

public static class GpuTrace
{
    public static IGpuTrace? Sink;
}
