namespace Verdite.Core;

/// <summary>
/// A window icon made of the game's own memory-card icon, read off the player's
/// disc by the game (where it is on a disc is the game's finding; what to do with
/// it is this).
///
///     {Tag}_ICON=orb        the shipped mark instead (png says the same)
///     {Tag}_ICON=off        no icon at all (none says the same)
///     {Tag}_ICON=1          another frame of the icon's animation
///     {Tag}_ICON_INSTALL=0  do not write the icon into the desktop's icon theme
///
/// A card icon is 16x16 at 4bpp with a 16-entry BGR555 palette, so it is scaled
/// by whole multiples with no filter and every size a desktop asks for is handed
/// over at once (RecompOne's 0061): pixel art a window manager never resamples.
/// Entry 0 is the background, which a card header zeroes, so it is transparent.
/// On Linux the same sizes go into the icon theme (<see cref="DesktopEntry"/>),
/// which is the only icon a Wayland compositor reads.
/// </summary>
public static class WindowIcon
{
    public const int Side = 16;

    static readonly int[] Sizes = [16, 32, 48, 64, 128, 256];

    /// <summary>
    /// Which frame the game's <c>{Tag}_ICON</c> asks for, <paramref name="byDefault"/>
    /// when it names none; null when the card icon is not wanted, in which case
    /// <c>off</c> has already cleared the icon and <c>orb</c> left the shipped one.
    /// </summary>
    public static int? Frame(int frames, int byDefault)
    {
        var mode = (Game.Env("ICON") ?? "").Trim().ToLowerInvariant();
        if (mode is "orb" or "png") return null;
        if (mode is "off" or "none")
        {
            RecompOne.Runtime.Runtime.ClearIcon();
            return null;
        }

        int frame = mode.Length == 1 && char.IsDigit(mode[0]) ? mode[0] - '0' : byDefault;
        return frame < frames ? frame : byDefault;
    }

    /// <summary>
    /// One frame as RGBA. <paramref name="pixels"/> holds row 0 of the frame at
    /// <c>frame * 8</c>, and each row after it <paramref name="rowStride"/> bytes
    /// further on: a card's own three frames stored a row at a time are stride 24,
    /// and a wider table of icons is its own width.
    /// </summary>
    public static byte[] Decode(ReadOnlySpan<byte> clut, ReadOnlySpan<byte> pixels, int rowStride, int frame)
    {
        var rgba = new byte[Side * Side * 4];
        for (int y = 0; y < Side; y++)
        {
            int row = y * rowStride + frame * (Side / 2);
            for (int x = 0; x < Side; x++)
            {
                int b = pixels[row + x / 2];
                int index = (x & 1) == 0 ? b & 0xF : b >> 4;
                int c = clut[index * 2] | (clut[index * 2 + 1] << 8);
                int o = (y * Side + x) * 4;
                rgba[o + 0] = (byte)((c & 31) * 255 / 31);
                rgba[o + 1] = (byte)((c >> 5 & 31) * 255 / 31);
                rgba[o + 2] = (byte)((c >> 10 & 31) * 255 / 31);
                rgba[o + 3] = index == 0 ? (byte)0 : (byte)255;
            }
        }

        return rgba;
    }

    /// <summary>Set a decoded 16x16 frame as the window's icon at every size, and publish it to the icon theme.</summary>
    public static void Apply(byte[] rgba)
    {
        var images = new List<(byte[] Rgba, int W, int H)>(Sizes.Length);
        foreach (int n in Sizes) images.Add((Scale(rgba, n / Side), n, n));
        RecompOne.Runtime.Runtime.SetIcons(images);
        Console.WriteLine($"[{Game.Tag}] icon: the game's memory-card icon, off the disc");
        DesktopEntry.Publish(images);
    }

    static byte[] Scale(byte[] src, int n)
    {
        if (n <= 1) return src;
        int side = Side * n;
        var dst = new byte[side * side * 4];
        for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++)
        {
            int s = ((y / n) * Side + x / n) * 4, d = (y * side + x) * 4;
            dst[d] = src[s];
            dst[d + 1] = src[s + 1];
            dst[d + 2] = src[s + 2];
            dst[d + 3] = src[s + 3];
        }

        return dst;
    }
}
