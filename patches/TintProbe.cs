using RecompOne.Runtime.Events;

namespace Kf1;

/// <summary>
/// KF1_TINTPROBE=1: whether the full-screen tints (death fade, damage flash, fade-in)
/// are drawn on every frame or only on tick frames, and the largest flat
/// semi-transparent box seen (the tints are not yet identified in this game). Each
/// window of two seconds prints the tint counts, a one-character-a-frame strip, and
/// that box. The reading goes in "The tints between ticks" in docs/SMOOTHING.md.
/// </summary>
public static class TintProbe
{
    const int EdgeSlack = 2;

    static bool _on;

    // The frame being drawn: its identity, whether a tick built it, and what it held.
    static long _frame = -1;
    static bool _frameTicked, _frameTinted, _frameLarge;
    static long _frameBestArea;
    static int _fL, _fT, _fR, _fB;

    // The report window.
    static long _frames, _tickFrames, _tinted, _tintedTick, _large;
    static long _bestArea;
    static int _bL, _bT, _bR, _bB;
    static readonly System.Text.StringBuilder _strip = new();
    static long _windowStart;

    public static void Install()
    {
        var v = Environment.GetEnvironmentVariable("KF1_TINTPROBE");
        if (string.IsNullOrWhiteSpace(v) || v == "0") return;
        _on = true;
        _windowStart = Environment.TickCount64;
        Event.AddListener<RenderPrimEvent>(OnPrim);
        Event.AddListener<VSyncEvent>(_ => Close());
        Console.WriteLine("[KF1] tintprobe: on");
    }

    static void OnPrim(RenderPrimEvent e)
    {
        Close();
        // Read while the frame's own ordering table is drawn: the iteration that
        // built it decided its tick at stage 1, and the next has not begun.
        _frameTicked = FramePacing.IterationTicked;
        if (!e.SemiTransparent || e.Gouraud) return;

        int left = e.DrawLeft, right = e.DrawRight + 1;
        int lo = int.MaxValue, hi = int.MinValue, top = int.MaxValue, bottom = int.MinValue;
        for (int i = 0; i < e.Count; i++)
        {
            lo = Math.Min(lo, e.X[i]); hi = Math.Max(hi, e.X[i]);
            top = Math.Min(top, e.Y[i]); bottom = Math.Max(bottom, e.Y[i]);
        }

        // Full-screen tint: full width and most of the height. A stretched tint has
        // already left the clip rectangle, so "at least as wide" rather than "on the edges".
        if (!_frameTinted && lo <= left + EdgeSlack && hi >= right - EdgeSlack
            && bottom - top >= (e.DrawBottom - e.DrawTop) / 2)
            _frameTinted = true;

        // Large: at least half the draw area's width and height. Keep the biggest.
        long w = hi - lo, h = bottom - top;
        if (2 * w >= right - left && 2 * h >= e.DrawBottom + 1 - e.DrawTop && w * h > _frameBestArea)
        {
            _frameLarge = true;
            _frameBestArea = w * h;
            _fL = lo; _fT = top; _fR = hi; _fB = bottom;
        }
    }

    // A new frame begins once FramePacing has counted the last one's boundary;
    // book the finished frame then.
    static void Close()
    {
        if (!_on) return;
        long f = FramePacing.Frames;
        if (f == _frame) return;
        if (_frame >= 0 && AgentBeacon.Overlay == "game")
        {
            _frames++;
            if (_frameTicked) _tickFrames++;
            if (_frameTinted) { _tinted++; if (_frameTicked) _tintedTick++; }
            if (_frameLarge)
            {
                _large++;
                if (_frameBestArea > _bestArea)
                {
                    _bestArea = _frameBestArea;
                    _bL = _fL; _bT = _fT; _bR = _fR; _bB = _fB;
                }
            }
            if (_strip.Length < 600)
                _strip.Append(_frameTinted ? (_frameTicked ? 'T' : 't') : (_frameTicked ? '.' : ','));
        }
        _frame = f;
        _frameTicked = _frameTinted = _frameLarge = false;
        _frameBestArea = 0;

        if (Environment.TickCount64 - _windowStart < 2000) return;
        if (_tinted > 0 || _large > 0)
        {
            Console.WriteLine($"[KF1] tintprobe: {_frames} frames ({_tickFrames} ticked), " +
                              $"tinted {_tinted} ({_tintedTick} ticked, {_tinted - _tintedTick} idle)");
            Console.WriteLine($"[KF1] tintprobe: {_strip}");
        }
        if (_large > 0)
            Console.WriteLine($"[KF1] tintprobe: large flat semi-transparent on {_large} frame(s), " +
                              $"largest box ({_bL},{_bT})-({_bR},{_bB}) {_bR - _bL}x{_bB - _bT}");
        Console.Out.Flush();
        _frames = _tickFrames = _tinted = _tintedTick = _large = 0;
        _bestArea = 0;
        _strip.Clear();
        _windowStart = Environment.TickCount64;
    }
}
