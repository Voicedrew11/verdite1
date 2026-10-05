namespace RecompOne.Runtime.Hle;

public static class GlVram
{
    public static int Scale { get; set; } = 4;
    public static int Width => VramShadow.Width * Scale;
    public static int Height => VramShadow.Height * Scale;

    /// <summary>Keep a scaled copy of every framebuffer readback, so an upload of
    /// the same pixels back into VRAM is served by that copy instead of by the 1x
    /// data the game read. See GlCore's snapshot block. On by default;
    /// KF2_VRAMSNAP=0 is the comparison.</summary>
    public static bool Snapshots { get; set; } = true;

    /// <summary>KF2_VRAMSNAP_PROBE=1: restores served against uploads that missed,
    /// a line every two seconds.</summary>
    public static bool SnapshotProbe { get; set; }
}