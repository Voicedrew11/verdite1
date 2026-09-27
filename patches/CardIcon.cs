using RecompOne.Runtime.Cdrom;

namespace Kf2;

/// <summary>
/// The window icon is the game's own memory-card icon, read off the disc.
///
///     KF2_ICON=orb      the shipped verdite mark instead
///     KF2_ICON=off      no icon at all
///     KF2_ICON=1        a frame of the card icon's three (0 by default)
///     KF2_ICON_INSTALL=0   do not write the icon into the desktop's icon theme
///
/// 16x16 at 4bpp, scaled by whole multiples with no filter, so the desktop is
/// handed a size it can use without resampling pixel art. Nothing is shipped:
/// the bytes come from the player's own image at boot, and a disc that does not
/// answer leaves whatever Program.cs already set.
///
/// A Wayland compositor does not take an icon from a window -- GLFW says so in
/// as many words -- so on Linux the same pixels are also written into the icon
/// theme under the app id, which is what a compositor looks up. See "The icon
/// comes off the disc" and "Wayland takes the icon from the desktop entry" in
/// docs/PACKAGING.md.
/// </summary>
public static class CardIcon
{
    /// <summary>The CLUT in CD/COM/FDAT.T; the three frames are 0x30 past it.</summary>
    const int IconOffset = 0x14D210;

    /// <summary>The same sixteen colours in GAME.EXE, which is what names the icon.</summary>
    const int PaletteOffset = 0x56DB4;

    const int Frames = 3, Side = 16, FrameBytes = Side * Side / 2;

    static readonly int[] Sizes = [16, 32, 48, 64, 128, 256];

    public static void Install(string? discPath)
    {
        var mode = (Environment.GetEnvironmentVariable("KF2_ICON") ?? "").Trim().ToLowerInvariant();
        if (mode is "orb" or "png") return;
        if (mode is "off" or "none")
        {
            RecompOne.Runtime.Runtime.ClearIcon();
            return;
        }

        int frame = mode.Length == 1 && char.IsDigit(mode[0]) ? mode[0] - '0' : 0;
        if (frame >= Frames) frame = 0;

        var path = string.IsNullOrWhiteSpace(discPath) ? RecompOne.Runtime.Runtime.CdPath : discPath;
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path)) return;

        try
        {
            using var disc = DiscFs.Open(path);
            byte[] argb;
            if (ReadKf1(disc, frame) is { } kf1)
            {
                argb = kf1;
            }
            else
            {
                if (Read(disc, out byte[] clut, out byte[] pixels) is { } why)
                {
                    Console.Error.WriteLine($"[KF2] icon: {why}; keeping the shipped mark");
                    return;
                }
                argb = Decode(clut, pixels, frame);
            }

            var images = new List<(byte[] Rgba, int W, int H)>(Sizes.Length);
            foreach (int n in Sizes) images.Add((Scale(argb, n / Side), n, n));
            RecompOne.Runtime.Runtime.SetIcons(images);
            Console.WriteLine("[KF2] icon: the game's memory-card icon, off the disc");
            DesktopEntry.Publish(images);
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[KF2] icon: {e.Message}; keeping the shipped mark");
        }
    }

    /// <summary>
    /// King's Field (JP) keeps its card icon as three plain TIMs, one a frame, at
    /// KF/TIM/ICO1-3.TIM: 192 bytes each, a 16-colour CLUT and a 16x16 4-bit image
    /// (a knight swinging a sword). Null when the disc has none, which is the KF2
    /// disc, so the path below still serves it.
    /// </summary>
    static byte[]? ReadKf1(DiscFs disc, int frame)
    {
        if (!disc.Locate($"KF/TIM/ICO{frame + 1}.TIM", out int lba, out uint size) || size < 192) return null;
        var d = disc.ReadSector(lba);
        if (BitConverter.ToUInt32(d, 0) != 0x10 || (BitConverter.ToUInt32(d, 4) & 0xF) != 0x8) return null;
        int clutLen = (int)BitConverter.ToUInt32(d, 8);
        byte[] clut = d[20..52];
        int img = 8 + clutLen + 12;
        var rgba = new byte[Side * Side * 4];
        for (int y = 0; y < Side; y++)
        for (int x = 0; x < Side; x++)
        {
            int b = d[img + y * (Side / 2) + x / 2];
            int index = (x & 1) == 0 ? b & 0xF : b >> 4;
            int c = clut[index * 2] | (clut[index * 2 + 1] << 8);
            int o = (y * Side + x) * 4;
            rgba[o + 0] = (byte)((c & 31) * 255 / 31);
            rgba[o + 1] = (byte)((c >> 5 & 31) * 255 / 31);
            rgba[o + 2] = (byte)((c >> 10 & 31) * 255 / 31);
            rgba[o + 3] = index == 0 ? (byte)0 : (byte)255;
        }
        return rgba;
    }

    /// <summary>
    /// The palette is in GAME.EXE as well as in FDAT.T, so it is the signature the
    /// pixels are found by: the fixed offset is checked against it, and a scan is
    /// what answers if some other mastering moved the archive's contents.
    /// </summary>
    static string? Read(DiscFs disc, out byte[] clut, out byte[] pixels)
    {
        clut = [];
        pixels = [];

        if (!disc.Locate("GAME.EXE", out int exeLba, out _)) return "no GAME.EXE";
        if (!disc.Locate("CD/COM/FDAT.T", out int fdatLba, out uint fdatSize)) return "no CD/COM/FDAT.T";

        var exe = disc.ReadSector(exeLba + PaletteOffset / 2048);
        // Entry 0 is transparent in the card's header and opaque in the archive's
        // copy, so the fifteen that are the icon's colours are what is compared.
        var signature = exe[(PaletteOffset % 2048 + 2)..(PaletteOffset % 2048 + 32)];

        int offset = IconOffset;
        var sector = disc.ReadSector(fdatLba + offset / 2048);
        if (!Same(sector, offset % 2048 + 2, signature))
        {
            var whole = disc.ReadSectors(fdatLba, (int)fdatSize);
            offset = IndexOf(whole, signature) - 2;
            if (offset < 0 || offset + 0x30 + Frames * FrameBytes > whole.Length)
                return "the icon is not in this image";
            clut = whole[offset..(offset + 32)];
            pixels = whole[(offset + 0x30)..(offset + 0x30 + Frames * FrameBytes)];
            return null;
        }

        int at = offset % 2048;
        clut = sector[at..(at + 32)];
        pixels = sector[(at + 0x30)..(at + 0x30 + Frames * FrameBytes)];
        return null;
    }

    /// <summary>
    /// One frame as RGBA. The three are stored a row at a time, frame 0's row y
    /// then frame 1's then frame 2's, which is the order a loop writing all three
    /// into a card header reads them in. Index 0 is the background and the card's
    /// own header zeroes it, so it is this icon's transparency.
    /// </summary>
    static byte[] Decode(byte[] clut, byte[] pixels, int frame)
    {
        var rgba = new byte[Side * Side * 4];
        for (int y = 0; y < Side; y++)
        {
            int row = (y * Frames + frame) * (Side / 2);
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

    static bool Same(byte[] data, int at, byte[] want)
    {
        if (at < 0 || at + want.Length > data.Length) return false;
        for (int i = 0; i < want.Length; i++)
            if (data[at + i] != want[i])
                return false;
        return true;
    }

    static int IndexOf(byte[] data, byte[] want)
    {
        for (int i = 0; i + want.Length <= data.Length; i++)
            if (Same(data, i, want))
                return i;
        return -1;
    }
}
