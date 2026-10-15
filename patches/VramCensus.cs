using RecompOne.Runtime.Events;

namespace Kf1;

/// <summary>
/// KF1_VRAMCENSUS=&lt;seconds&gt;: which 64x16 blocks of VRAM change between world
/// ticks, i.e. whether the game animates textures or palettes (water, lava, palette
/// cycling). Samples once per tick in the game after KF1_VRAMCENSUS_AFTER seconds
/// (default 3), prints the blocks that changed on at least 10% of tick pairs, then
/// stops. The two display buffers, (0,0) and (0,240), are skipped. The CPU's VRAM
/// holds every upload but not what the GL backend draws. See "Textures" in
/// docs/SMOOTHING.md.
/// </summary>
public static class VramCensus
{
    const int W = 1024, H = 512;
    const int BW = 64, BH = 16, Cols = W / BW, Rows = H / BH;
    static long _settle = 3000;

    static int _seconds;
    static ushort[]? _prev;
    static long _prevTime;
    static long _firstSample = -1, _countStart, _countEnd;
    static long _ticksMark = -1;
    static bool _done;

    static readonly bool[] _excluded = new bool[Cols * Rows];
    static readonly int[] _pairPixels = new int[Cols * Rows];
    static readonly int[] _changedOn = new int[Cols * Rows];
    static readonly int[] _lastPixels = new int[Cols * Rows];
    static int _pairs, _anyPairs;

    public static void Install()
    {
        if (!int.TryParse(Environment.GetEnvironmentVariable("KF1_VRAMCENSUS"), out _seconds) || _seconds <= 0) return;
        if (int.TryParse(Environment.GetEnvironmentVariable("KF1_VRAMCENSUS_AFTER"), out int after) && after > 0) _settle = after * 1000L;
        for (int by = 0; by < Rows; by++)
            for (int bx = 0; bx < Cols; bx++)
            {
                int x0 = bx * BW, y0 = by * BH, y1 = y0 + BH - 1;
                // The two display buffers, (0,0) and (0,240), 320x240 each.
                _excluded[by * Cols + bx] = x0 < 320 && y0 < 480;
            }
        Event.AddListener<VSyncEvent>(_ => Sample());
    }

    // From the vblank; a sample is taken only when the world has ticked since the last.
    static void Sample()
    {
        if (_done || RecompOne.Runtime.Runtime.Gpu is not { } gpu) return;
        if (AgentBeacon.Overlay != "game") return;
        if (FramePacing.Ticks == _ticksMark) return;
        _ticksMark = FramePacing.Ticks;

        long now = Environment.TickCount64;
        if (_firstSample < 0)
        {
            _firstSample = now;
            _countStart = now + _settle;
            _countEnd = _countStart + _seconds * 1000L;
            Console.WriteLine($"[KF1] vramcensus: display origin ({gpu.DisplayX},{gpu.DisplayY}); settling {_settle / 1000} s");
        }

        var vram = gpu.Vram;
        // Count a pair only when both of its samples fall in the counting window.
        if (_prev != null && now >= _countStart && _prevTime >= _countStart) Compare(vram);
        _prev ??= new ushort[vram.Length];
        vram.AsSpan().CopyTo(_prev);
        _prevTime = now;

        if (now < _countEnd) return;
        // That the copy holds the textures at all.
        long nz = 0, nzAll = 0;
        for (int i = 0; i < vram.Length; i++) if (vram[i] != 0) { nzAll++; if (i % 1024 >= 320 || i / 1024 >= 480) nz++; }
        Console.WriteLine($"[KF1] vramcensus: {nz} non-zero pixels outside the buffers, {nzAll} in all");
        _done = true;
        Report();
    }

    static void Compare(ushort[] vram)
    {
        Array.Clear(_pairPixels);
        int total = 0;
        for (int y = 0; y < H; y++)
        {
            int row = y * W, by = (y / BH) * Cols;
            for (int x = 0; x < W; x++)
            {
                int b = by + x / BW;
                if (_excluded[b]) continue;
                if (vram[row + x] != _prev![row + x]) { _pairPixels[b]++; total++; }
            }
        }
        for (int b = 0; b < _pairPixels.Length; b++)
            if (_pairPixels[b] > 0) { _changedOn[b]++; _lastPixels[b] = _pairPixels[b]; }
        _pairs++;
        if (total > 0) _anyPairs++;
    }

    static void Report()
    {
        Console.WriteLine($"[KF1] vramcensus: {_pairs} tick pairs");
        Console.WriteLine($"[KF1] vramcensus: changed on {_anyPairs} of {_pairs} ticks");
        if (_anyPairs == 0) Console.WriteLine("[KF1] vramcensus: nothing changed outside the display buffers");

        for (int b = 0; b < _changedOn.Length; b++)
            if (_changedOn[b] > 0 && _changedOn[b] * 10 >= _pairs)
                Console.WriteLine($"[KF1] vramcensus: block x={(b % Cols) * BW},y={(b / Cols) * BH} (64x16) " +
                                  $"changed on {_changedOn[b]} of {_pairs} ticks");

        var top = Enumerable.Range(0, _changedOn.Length)
            .Where(b => _changedOn[b] > 0)
            .OrderByDescending(b => _changedOn[b])
            .Take(5);
        foreach (int b in top)
            Console.WriteLine($"[KF1] vramcensus: top block x={(b % Cols) * BW},y={(b / Cols) * BH} " +
                              $"changed on {_changedOn[b]} of {_pairs} ticks; last change {_lastPixels[b]} distinct pixels");

        Console.WriteLine("[KF1] vramcensus: done");
    }
}
