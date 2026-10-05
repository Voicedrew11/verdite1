namespace RecompOne.Runtime.Hle;

/// <summary>
/// Diagnostic, <c>KF2_VRAMCHECK=1</c>. A CPU mirror of what the game put in VRAM
/// (uploads, copies, fills), compared against the backend's 1x sample VRAM after
/// every operation that writes it. A GPU draw or a writeback is adopted as truth
/// inside its own rectangle; any pixel that changes anywhere else is reported with
/// the operation that changed it.
/// </summary>
sealed class VramCheck
{
    public static readonly bool On = Environment.GetEnvironmentVariable("KF2_VRAMCHECK") == "1";

    const int W = VramShadow.Width, H = VramShadow.Height;
    readonly ushort[] _mirror = new ushort[W * H];
    readonly ushort[] _back = new ushort[W * H];
    bool _init;
    int _reports;
    long _ops;

    public void Upload(int x, int y, int w, int h, ReadOnlySpan<ushort> px)
    {
        for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
        {
            int xx = x + i, yy = y + j;
            if ((uint)xx < W && (uint)yy < H) _mirror[yy * W + xx] = px[j * w + i];
        }
    }

    public void Copy(int sx, int sy, int dx, int dy, int w, int h)
    {
        var tmp = new ushort[Math.Max(0, w * h)];
        for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
        {
            int xx = sx + i, yy = sy + j;
            tmp[j * w + i] = (uint)xx < W && (uint)yy < H ? _mirror[yy * W + xx] : (ushort)0;
        }
        Upload(dx, dy, w, h, tmp);
    }

    public void Fill(int x, int y, int w, int h, ushort c)
    {
        for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
        {
            int xx = x + i, yy = y + j;
            if ((uint)xx < W && (uint)yy < H) _mirror[yy * W + xx] = c;
        }
    }

    /// <summary>After an operation. <paramref name="adopt"/>: the rectangle is the
    /// GPU's to write, so it is taken as read rather than checked.</summary>
    public void Check(IGlVram vram, string op, int ax, int ay, int aw, int ah, bool adopt)
    {
        _ops++;
        vram.ReadRect(0, 0, W, H, _back);
        if (!_init)
        {
            _back.CopyTo(_mirror, 0);
            _init = true;
            return;
        }

        int n = 0, x0 = W, y0 = H, x1 = -1, y1 = -1, first = -1;
        for (int y = 0; y < H; y++)
        {
            int row = y * W;
            bool inY = adopt && y >= ay && y < ay + ah;
            for (int x = 0; x < W; x++)
            {
                if (_back[row + x] == _mirror[row + x]) continue;
                if (inY && x >= ax && x < ax + aw) continue;
                n++;
                if (first < 0) first = row + x;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }

        if (n > 0 && _reports < 60)
        {
            _reports++;
            Console.WriteLine($"[vramcheck] op {_ops} {op} ({ax},{ay} {aw}x{ah}): {n} pixel(s) changed outside it, " +
                              $"box ({x0},{y0})-({x1},{y1}), first ({first % W},{first / W}) " +
                              $"mirror {_mirror[first]:X4} vram {_back[first]:X4}");
        }
        _back.CopyTo(_mirror, 0);
    }
}
