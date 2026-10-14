namespace RecompOne.Runtime.Hle;

public static class GlVram
{
    public static int Scale { get; set; } = 4;
    public static int Width => VramShadow.Width * Scale;
    public static int Height => VramShadow.Height * Scale;

    /// <summary>0097. A render scale asked for while running; 0 is none. The
    /// backend takes it at the next present, between two frames, and sets
    /// <see cref="Scale"/> itself.</summary>
    public static int Requested { get; set; }

    /// <summary>0097. The largest scale whose VRAM texture fits the context's
    /// texture size limit; a request above it is taken at this.</summary>
    public static int MaxScale { get; set; } = 8;

    /// <summary>Keep a scaled copy of every framebuffer readback, so an upload of
    /// the same pixels back into VRAM is served by that copy instead of by the 1x
    /// data the game read. See GlCore's snapshot block. On by default;
    /// KF2_VRAMSNAP=0 is the comparison.</summary>
    public static bool Snapshots { get; set; } = true;

    /// <summary>KF2_VRAMSNAP_PROBE=1: restores served against uploads that missed,
    /// a line every two seconds.</summary>
    public static bool SnapshotProbe { get; set; }
}