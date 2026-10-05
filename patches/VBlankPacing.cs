using System.Diagnostics;
using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// Hold every VSync outside the renderer to a real vblank, the way the console's did.
///
///     KF1_VBLANKPACING=0        leave them on the runtime's clock -- comparison only
///     KF1_VBLANKPACING_PROBE=1  a line a second: calls held, by mode, and the mean wait
///
/// The runtime's LibEtc.VSync presents and returns at once for any mode but 1
/// (only the per-call ceiling throttles it), so a loop that counts VSync calls as
/// a delay collapses at any frame rate above 60. This class keeps its own 60 Hz
/// wall-clock grid and holds such calls: mode 0 one vblank, mode n &gt;= 2 n. Queries
/// (mode &lt; 0 and 1) pass through. Calls made inside the renderer (stage I) belong
/// to <see cref="FramePacing"/>'s frame boundary, or in a modal loop to the frame
/// gate after it, and are exempt. Only GAME.EXE, only with FramePacing on.
/// Verdite3's VBlankPacing. See "Menus and loading screens wait for a vblank" in
/// docs/DEVELOPMENT.md.
/// </summary>
public static class VBlankPacing
{
    const uint VSyncThunk = 0x800555E0;  // LibEtc.VSync, GAME.EXE's copy
    const uint Renderer = 0x8001FDE4;     // the renderer, stage I; its flip is FramePacing's

    /// <summary>One vblank, the unit both modes are counted in. Not LibEtc's
    /// _vcount: that only advances from a VSync call, so waiting on it deadlocks.</summary>
    const double VBlankMs = 1000.0 / 60.0;

    /// <summary>A stall this far behind resyncs the grid instead of fast-forwarding.</summary>
    const double ResyncPeriods = 4.0;

    /// <summary>Spin the last stretch; Thread.Sleep granularity is a few ms.</summary>
    const double SpinMs = 1.5;

    static readonly ModInfo _self = new()
    {
        Id = "kf1.vblankpacing",
        Name = "VBlank pacing",
        Version = "1.0",
        Description = "Waits a real vblank for VSync calls outside the renderer, as the console did.",
    };

    static readonly Stopwatch _clock = Stopwatch.StartNew();

    public static bool Enabled { get; set; } = true;
    static bool _probe;

    static bool _inGame;
    static bool _inRenderer;
    static int _depth;

    /// <summary>When the next held call may return, in ms. Negative seeds the grid.</summary>
    static double _due = -1.0;

    // What the vblank hook did, for the probe window.
    static double _windowStart;
    static long[] _heldByMode = new long[8];
    static double _waitMs;
    static long _exempt;

    static readonly HashSet<string> _vsHooked = [];
    static bool _stageHooked;

    public static void Configure(string? enabled, string? probe)
    {
        if (!string.IsNullOrWhiteSpace(enabled)) Enabled = enabled != "0";
        if (!string.IsNullOrWhiteSpace(probe)) _probe = probe == "1";
    }

    public static bool ProbeOn { get => _probe; set => _probe = value; }

    public static void Install()
    {
        Event.AddListener<OverlayLoadedEvent>(e =>
        {
            // Only GAME.EXE has the renderer and the vsync-counted loops.
            _inGame = e.Name == "game";
            _depth = 0;
            _inRenderer = false;
        });
        HookAttach.OnOverlayLoad("vblank pacing", Attach, "See \"Menus and loading screens wait for a vblank\" in docs/DEVELOPMENT.md.");
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var self = typeof(VBlankPacing);
        MethodInfo M(string n) => self.GetMethod(n, BindingFlags.Public | BindingFlags.Static)!;

        MethodInfo? thunk = null, stage = null;

        if (!_vsHooked.Contains("game") && SymbolRegistry.Resolve("game", null, VSyncThunk) is { } t
            && HookManager.AddPre(_self, t, M(nameof(BeforeVSync)))) thunk = t;
        if (!_stageHooked && SymbolRegistry.Resolve("game", null, Renderer) is { } s)
        {
            if (HookManager.AddPre(_self, s, M(nameof(BeforeRenderer)), int.MinValue)
                && HookManager.AddPost(_self, s, M(nameof(AfterRenderer)), int.MaxValue)) stage = s;
        }

        HookManager.Commit();

        if (thunk != null && HookAttach.Installed(thunk)) _vsHooked.Add("game");
        if (stage != null && HookAttach.Installed(stage)) _stageHooked = true;

        Console.WriteLine($"[KF1] vblank pacing: {(Enabled ? "on" : "off")}, " +
                          $"VSync {(_vsHooked.Contains("game") ? "held" : "NOT held")}, " +
                          $"renderer {(_stageHooked ? "exempt" : "NOT exempt")}");
        return _vsHooked.Count == 1 && _stageHooked;
    }

    /// <summary>A VSync call about to run. Queries pass; a present outside the renderer
    /// waits its mode's vblanks on the 60 Hz grid.</summary>
    public static void BeforeVSync(CpuContext c, IMemory m)
    {
        if (!Enabled || !_inGame || !FramePacing.Enabled) return;

        int mode = (int)c.A0;
        if (mode < 0 || mode == 1) return;

        if (_inRenderer) { _exempt++; return; }

        int vblanks = mode == 0 ? 1 : mode;
        double now = _clock.Elapsed.TotalMilliseconds;

        // Seed on the first call, and resync rather than pay back a stall -- the
        // host was stopped, not the game running late.
        if (_due < 0.0 || _due < now - ResyncPeriods * VBlankMs) _due = now;
        _due += vblanks * VBlankMs;

        if (now < _due)
        {
            double sleepUntil = _due - SpinMs;
            if (now < sleepUntil && (int)(sleepUntil - now) is > 0 and var ms) Thread.Sleep(ms);
            while (_clock.Elapsed.TotalMilliseconds < _due) Thread.SpinWait(48);
        }

        _waitMs += _clock.Elapsed.TotalMilliseconds - now;
        if (mode < _heldByMode.Length) _heldByMode[mode]++;

        Probe(now);
    }

    /// <summary>Mark a renderer run, at the lowest order so the depth covers every
    /// other hook's call, and keep the flip's own VSync out of the wait.</summary>
    public static void BeforeRenderer(CpuContext c, IMemory m)
    {
        _depth++;
        _inRenderer = true;
    }

    /// <summary>Close it, at the highest order. Runs even if another pre skipped
    /// the body, since HookManager invokes every post.</summary>
    public static void AfterRenderer(CpuContext c, IMemory m)
    {
        if (_depth > 0) _depth--;
        _inRenderer = _depth > 0;
    }

    static void Probe(double now)
    {
        if (!_probe) return;
        if (_windowStart <= 0.0) { _windowStart = now; return; }
        double elapsed = now - _windowStart;
        if (elapsed < 1000.0) return;

        long held = 0;
        foreach (var n in _heldByMode) held += n;
        if (held == 0)
        {
            _windowStart = now;
            _waitMs = 0.0;
            _exempt = 0;
            Array.Clear(_heldByMode);
            return;
        }
        long mode0 = _heldByMode[0];

        Console.WriteLine($"[KF1] vblank pacing: {held * 1000.0 / elapsed:0.0} VSync call(s)/s held " +
                          $"({mode0 * 1000.0 / elapsed:0.0} mode 0, {(held - mode0) * 1000.0 / elapsed:0.0} mode >= 2), " +
                          $"mean wait {_waitMs / held:0.0} ms, {_exempt * 1000.0 / elapsed:0.0} exempted/s");
        Console.Out.Flush();

        _windowStart = now;
        _waitMs = 0.0;
        _exempt = 0;
        Array.Clear(_heldByMode);
    }
}
