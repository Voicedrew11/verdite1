using RecompOne.Runtime.Cdrom.Chd;

namespace RecompOne.Runtime.Cdrom;

public static class DiscImage
{
    /// <summary>
    /// 0093. A port's layer over every image this opens, given the image and its
    /// path: a patch applied to sectors as they are read, say. Null passes the image
    /// through, which is the default.
    /// </summary>
    public static Func<IDiscImage, string, IDiscImage>? Decorate;

    public static IDiscImage Open(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new ArgumentException("disc path i empty", nameof(path));
        if (!File.Exists(path))
            throw new FileNotFoundException($"disc image not found: {path}", path);

        IDiscImage image = Detect(path) switch
        {
            DiscFormat.Chd => ChdImage.Open(path),
            DiscFormat.CueBin => CueBinImage.Open(path),
            _ => throw new NotSupportedException($"unsupported disc format: {path}")
        };
        return Decorate?.Invoke(image, path) ?? image;
    }

    public static DiscFormat Detect(string path)
    {
        var ext = Path.GetExtension(path);
        if (ext.Equals(".chd", StringComparison.OrdinalIgnoreCase)) return DiscFormat.Chd;
        if (ext.Equals(".cue", StringComparison.OrdinalIgnoreCase)) return DiscFormat.CueBin;
        return HasChdMagic(path) ? DiscFormat.Chd : DiscFormat.Unknown;
    }

    private static bool HasChdMagic(string path)
    {
        try
        {
            using var s = File.OpenRead(path);
            Span<byte> magic = stackalloc byte[8];
            return s.Read(magic) == 8 && magic.SequenceEqual(ChdFile.Magic);
        }
        catch
        {
            return false;
        }
    }
}

public enum DiscFormat
{
    Unknown,
    CueBin,
    Chd
}