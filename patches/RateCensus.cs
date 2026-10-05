using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;

namespace Kf1;

/// <summary>
/// KF1_RATECENSUS=&lt;seconds&gt; (with KF1_FPS above the tick rate): which RAM words
/// change between two frames on which no main-loop stage ran, i.e. what still
/// moves at the render rate. Prints the busiest runs once, then stops. See
/// "What still runs at the render rate" in docs/GAME_INTERNALS.md.
/// </summary>
public static class RateCensus
{
    const int Lo = 0x10000 / 4, Hi = 0x200000 / 4;

    static int _seconds;
    static uint[]? _prev;
    static int[]? _idle;
    static int _idleIntervals;
    static long _framesMark = -1;
    static bool _prevIdle;
    static long _start = -1;
    static bool _done;

    public static void Install()
    {
        if (!int.TryParse(Environment.GetEnvironmentVariable("KF1_RATECENSUS"), out _seconds) || _seconds <= 0) return;
        _prev = new uint[Hi];
        _idle = new int[Hi];
        Event.AddListener<VSyncEvent>(_ => Sample());
    }

    // From the vblank, once per new frame boundary; a frame is idle when the main
    // loop iteration that built it skipped its stages.
    static void Sample()
    {
        if (_done || RecompOne.Runtime.Runtime.Mem is not PSMemory m) return;
        if (AgentBeacon.Overlay != "game") return;
        if (FramePacing.Frames == _framesMark) return;
        _framesMark = FramePacing.Frames;

        var ram = System.Runtime.InteropServices.MemoryMarshal.Cast<byte, uint>(m.Ram[..0x200000]);
        bool idle = !FramePacing.IterationTicked;
        if (idle && _prevIdle)
        {
            _idleIntervals++;
            for (int i = Lo; i < Hi; i++) if (ram[i] != _prev![i]) _idle![i]++;
        }
        ram.CopyTo(_prev);
        _prevIdle = idle;

        if (_start < 0) _start = Environment.TickCount64;
        if (Environment.TickCount64 - _start < _seconds * 1000L) return;
        _done = true;
        Report();
    }

    static void Report()
    {
        int n = _idleIntervals;
        Console.WriteLine($"[KF1] ratecensus: {n} idle-to-idle frame pairs; runs of words changing on >= 25% of them:");
        int i = Lo;
        while (i < Hi)
        {
            if (_idle![i] * 4 < n) { i++; continue; }
            int s = i, peak = 0;
            while (i < Hi && (_idle[i] * 4 >= n || (i + 1 < Hi && _idle[i + 1] * 4 >= n)))
            {
                peak = Math.Max(peak, _idle[i]);
                i++;
            }
            Console.WriteLine($"[KF1] ratecensus: 0x{0x80000000u + (uint)s * 4:X8}-0x{0x80000000u + (uint)i * 4:X8} " +
                              $"({i - s} words) peak {100.0 * peak / Math.Max(1, n):0}%");
        }
        Console.WriteLine("[KF1] ratecensus: done");
    }
}
