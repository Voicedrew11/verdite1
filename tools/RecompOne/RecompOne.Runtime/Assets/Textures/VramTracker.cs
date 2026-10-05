namespace RecompOne.Runtime.Assets.Textures;

public static class VramTracker
{
    public const int BlockW = 16;
    public const int BlockH = 16;
    public const int Cols = 1024 / BlockW;
    public const int Rows = 512 / BlockH;

    private static readonly int[] _gen = new int[Cols * Rows];
    private static readonly bool[] _gpuDirty = new bool[Cols * Rows];
    private static int _clock;

    /// <summary>0060. Moves on every mark, so a caller can skip re-reading
    /// generations while nothing has been written.</summary>
    public static int Clock => _clock;

    /// <summary>0073. Every image a LoadImage finished writing: x, y, w, h. The port's
    /// texture census counts what the game uploads at a time.</summary>
    public static Action<int, int, int, int>? Uploaded;

    // 0073. Which LoadImage last wrote each VRAM word, by id into a ring of rectangles.
    // A texture's key is normalised to the image the game uploaded it as, so that
    // one piece of art has one key whatever UVs a face reads it through.
    private static readonly ushort[] _uploadAt = new ushort[1024 * 512];
    private static readonly (short X, short Y, short W, short H)[] _uploads = new (short, short, short, short)[65536];
    private static int _uploadNext;

    public static void NoteUpload(int x, int y, int w, int h)
    {
        if (w <= 0 || h <= 0) return;
        // One image loaded in pieces -- this game splits a 128x128 sheet into 100 rows
        // and 28 -- is one image: a load that continues the previous one straight down,
        // at the same x and width, extends it.
        ushort id;
        var prev = _uploads[_uploadNext];
        if (_uploadNext != 0 && prev.X == x && prev.W == w && prev.Y + prev.H == y && prev.H + h <= 512)
        {
            id = (ushort)_uploadNext;
            _uploads[id] = (prev.X, prev.Y, prev.W, (short)(prev.H + h));
        }
        else
        {
            _uploadNext = _uploadNext % 65535 + 1;
            id = (ushort)_uploadNext;
            _uploads[id] = ((short)x, (short)y, (short)w, (short)h);
        }
        for (var r = 0; r < h; r++)
        {
            var row = ((y + r) & 511) * 1024;
            if (x + w <= 1024) _uploadAt.AsSpan(row + x, w).Fill(id);
            else
                for (var c = 0; c < w; c++)
                    _uploadAt[row + ((x + c) & 1023)] = id;
        }

        Uploaded?.Invoke(x, y, w, h);
    }

    /// <summary>The rectangle of the LoadImage that last wrote VRAM word (x, y).</summary>
    public static bool UploadAt(int x, int y, out int ux, out int uy, out int uw, out int uh)
    {
        var id = _uploadAt[(y & 511) * 1024 + (x & 1023)];
        (ux, uy, uw, uh) = id == 0 ? default : _uploads[id];
        return id != 0 && x >= ux && y >= uy && x < ux + uw && y < uy + uh;
    }

    public static void Reset()
    {
        Array.Clear(_gen);
        Array.Clear(_gpuDirty);
        _clock = 0;
    }

    private static void Bounds(int x, int y, int w, int h, out int c0, out int r0, out int c1, out int r1)
    {
        if (w < 0)
        {
            x += w;
            w = -w;
        }

        if (h < 0)
        {
            y += h;
            h = -h;
        }

        c0 = Math.Clamp(x / BlockW, 0, Cols - 1);
        r0 = Math.Clamp(y / BlockH, 0, Rows - 1);
        c1 = Math.Clamp((x + Math.Max(0, w - 1)) / BlockW, 0, Cols - 1);
        r1 = Math.Clamp((y + Math.Max(0, h - 1)) / BlockH, 0, Rows - 1);
    }

    public static void MarkCpuWrite(int x, int y, int w, int h)
    {
        var stamp = ++_clock;
        Bounds(x, y, w, h, out var c0, out var r0, out var c1, out var r1);
        for (var r = r0; r <= r1; r++)
        for (var c = c0; c <= c1; c++)
        {
            var i = r * Cols + c;
            _gen[i] = stamp;
            _gpuDirty[i] = false;
        }
    }

    public static void MarkGpuWrite(int x, int y, int w, int h)
    {
        var stamp = ++_clock;
        Bounds(x, y, w, h, out var c0, out var r0, out var c1, out var r1);
        for (var r = r0; r <= r1; r++)
        for (var c = c0; c <= c1; c++)
        {
            var i = r * Cols + c;
            _gen[i] = stamp;
            _gpuDirty[i] = true;
        }
    }

    public static int Generation(int x, int y, int w, int h)
    {
        Bounds(x, y, w, h, out var c0, out var r0, out var c1, out var r1);
        var acc = 0;
        for (var r = r0; r <= r1; r++)
        for (var c = c0; c <= c1; c++)
            acc = acc * 31 + _gen[r * Cols + c];
        return acc;
    }

    public static bool IsGpuDirty(int x, int y, int w, int h)
    {
        Bounds(x, y, w, h, out var c0, out var r0, out var c1, out var r1);
        for (var r = r0; r <= r1; r++)
        for (var c = c0; c <= c1; c++)
            if (_gpuDirty[r * Cols + c])
                return true;
        return false;
    }
}