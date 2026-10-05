using System.Diagnostics;

namespace RecompOne.Runtime.Host;

internal static class FrameClock
{
    private const int SlipFrames = 8;

    private static readonly Stopwatch _clock = Stopwatch.StartNew();

    private static double _due;
    private static long _count;

    private static double _fpsAccumMs;
    private static int _fpsFrames;

    private static double _frameMark;

    private static double _presentStartMs;
    private static int _presentFrames;

    public static bool VSync { get; set; }

    public static double LastFrameMs { get; private set; }

    public static double Fps { get; private set; }

    public static double PresentFps { get; private set; }

    //The GUEST vblank rate. Since upstream drove Interrupts.VBlankCount and
    //ClockMs off this clock, moving it moves the emulated vblank grid and every
    //game clock hanging off it -- so the host ceiling below must never touch it.
    public static double FrameMs => Sdk.LibGpu.Pal ? 1000.0 / 50.0 : 1000.0 / 60.0;

    public static double Now => _clock.Elapsed.TotalMilliseconds;

    public static long Count => _count;

    public static double Due => _due;

    public static int Catch()
    {
        var now = Now;

        if (_due <= 0.0)
        {
            _due = now + FrameMs;
            return 0;
        }

        if (now < _due) return 0;

        var frame = FrameMs;
        var ticks = (int)((now - _due) / frame) + 1;

        _due = ticks > SlipFrames ? now + frame : _due + ticks * frame;
        _count += ticks;
        return ticks;
    }

    public static void Force()
    {
        var now = Now;
        _count++;
        _due = now + FrameMs;
    }

    public static void Resync()
    {
        _due = Now + FrameMs;
        _nextFrameMs = Now;
    }

    public static void MarkPresent()
    {
        var now = Now;
        _presentFrames++;

        var elapsed = now - _presentStartMs;
        if (elapsed < 1000.0) return;

        PresentFps = _presentFrames * 1000.0 / elapsed;
        _presentStartMs = now;
        _presentFrames = 0;
    }

    public static void MarkFrame()
    {
        var now = Now;

        if (_frameMark <= 0.0)
        {
            _frameMark = now;
            return;
        }

        LastFrameMs = now - _frameMark;
        _frameMark = now;

        _fpsAccumMs += LastFrameMs;
        _fpsFrames++;
        if (_fpsAccumMs < 1000.0) return;

        Fps = _fpsFrames * 1000.0 / _fpsAccumMs;
        _fpsAccumMs = 0;
        _fpsFrames = 0;
    }

    //The HOST ceiling, and the whole of what is left of patch 0025. Upstream
    //throttles in PresentLoop, which this port never enters -- it presents from
    //inside the game's own VSync -- so the ceiling is spent here instead, from
    //Runtime.PresentFrame, and is kept strictly apart from FrameMs above.
    //
    //It paces per *VSync call*, not per rendered frame -- a frame can carry more
    //than one -- so a caller that needs an exact frame rate sets this as a
    //permissive ceiling and keeps its own deadline at the frame boundary.
    private const double SpinMs = 1.5;

    private static double _nextFrameMs;

    //-1 means "never set": follow the host rate, which is what upstream does and
    //is the only value that knows about PAL. 0 is off.
    private static double _targetFps = -1.0;

    public static double TargetFps
    {
        get => _targetFps < 0.0 ? (Sdk.LibGpu.Pal ? 50.0 : 60.0) : _targetFps;
        set => _targetFps = value > 0.0 ? value : 0.0;
    }

    private static double CeilingMs =>
        _targetFps < 0.0 ? FrameMs :
        _targetFps > 0.0 ? 1000.0 / _targetFps : 0.0;

    public static double LastWaitMs { get; private set; }

    public static void Throttle()
    {
        //0045.
        var profile = Diagnostics.Profiler.Begin(Diagnostics.Profiler.Throttle);
        ThrottleCore();
        Diagnostics.Profiler.End(profile);
    }

    private static void ThrottleCore()
    {
        var now = Now;

        var frameMs = CeilingMs;
        if (frameMs <= 0.0)
        {
            //Off. Keep the grid on the present, so that turning it back on does
            //not arrive with a frame of debt already owed.
            _nextFrameMs = now;
            LastWaitMs = 0;
            return;
        }

        _nextFrameMs += frameMs;
        var wait = _nextFrameMs - now;

        if (wait < -100)
        {
            _nextFrameMs = now;
            LastWaitMs = 0;
            return;
        }

        if (wait <= 0)
        {
            LastWaitMs = 0;
            return;
        }

        if (VSync && wait < frameMs * 0.75)
        {
            LastWaitMs = 0;
            return;
        }

        var sleepUntil = _nextFrameMs - SpinMs;
        if (now < sleepUntil)
        {
            var ms = (int)(sleepUntil - now);
            if (ms > 0) Thread.Sleep(ms);
        }

        while (Now < _nextFrameMs)
            Thread.SpinWait(48);

        LastWaitMs = wait;
    }

    //0064. VSync on Wayland, where the swap is left at interval 0 so a hidden
    //window cannot block the game: one present per refresh, on its own grid.
    private static double _nextRefreshMs;

    public static void WaitRefresh(int hz)
    {
        var periodMs = 1000.0 / (hz > 0 ? hz : 60);
        var now = Now;

        _nextRefreshMs += periodMs;
        var wait = _nextRefreshMs - now;
        if (wait <= 0)
        {
            //A late frame is not paid back with a burst.
            if (wait < -periodMs) _nextRefreshMs = now;
            return;
        }

        var sleepUntil = _nextRefreshMs - SpinMs;
        if (now < sleepUntil)
        {
            var ms = (int)(sleepUntil - now);
            if (ms > 0) Thread.Sleep(ms);
        }

        while (Now < _nextRefreshMs)
            Thread.SpinWait(48);
    }
}
