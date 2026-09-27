using System.Diagnostics;
using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// King's Field at any frame rate: the frame gate redraws while it waits.
///
/// The main loop (<c>func_800146B8</c>, from <c>0x8001482C</c>) runs eight logic
/// stages, the renderer <c>func_8001FDE4</c>, and then the frame gate
/// <c>func_800149F4</c>, which spins until the vblank counter its event handler
/// bumps (<c>0x80057B0C</c>) is three past the last frame's (<c>0x80057B10</c>):
/// a 20 fps game, with about 48 ms of every 50 spent in that spin. Measured with
/// <see cref="StageProbe"/>, the renderer is the only stage that projects a vertex
/// or calls DrawOTag, and the eight before it draw nothing.
///
/// So this replaces the gate with the same wait, and while it waits, calls the
/// renderer again at the target rate. **The world is untouched**: the logic
/// stages still run once per three vblanks, on the game's own clock, so there is
/// no tick clock of the port's to keep in step with the game's (which is most of
/// what King's Field II's FramePacing had to be). A redraw only redraws; what
/// makes it worth drawing is <see cref="ViewCarry"/>, which moves the camera
/// between the last two ticks. See "Frame pacing" in docs/KF1.md.
///
/// Only the main loop's gate redraws (its return address is <c>0x800148D0</c>),
/// and only once the renderer has run since the last gate. The gate's eight other
/// callers are modal loops -- doors, menus, the area load -- which keep the
/// original wait.
///
///     KF2_FPS=60           frames a second (the default); 20 or off is the game's own
///     KF2_FPS_PROBE=1      a line a second: frames drawn, ticks, redraws, spin
/// </summary>
public static class GateRedraw
{
    public const uint Gate = 0x800149F4;
    public const uint Renderer = 0x8001FDE4;
    const uint VCount = 0x80057B0C, VLast = 0x80057B10;
    const uint MainLoopGateReturn = 0x800148D0;

    /// <summary>Three vblanks: how long the game's frame is.</summary>
    public const double TickMs = 3000.0 / 60.0;

    /// <summary>The game's own rate. A target at or below it redraws nothing.</summary>
    public const double GameFps = 20.0;

    public const string FpsKey = "kf1.fps";

    static readonly ModInfo _self = new()
    {
        Id = "kf1.gateredraw",
        Name = "Frame pacing",
        Version = "1.0",
        Description = "Redraws the world at the target rate while the frame gate waits.",
    };

    static double? _forcedFps;
    static bool _probe;

    /// <summary>Frames a second to draw. 20 is the game's own and redraws nothing.</summary>
    public static double TargetFps { get; private set; } = 60.0;

    public static bool Redrawing => TargetFps > GameFps + 0.001;

    /// <summary>True for the length of a redraw's renderer call.</summary>
    public static bool InRedraw { get; private set; }

    /// <summary>When the last game-driven render of the main loop began: the
    /// start of the tick the carried view is interpolating across.</summary>
    public static double TickStartMs { get; internal set; } = double.NegativeInfinity;

    static bool _renderedSinceGate;
    static double _lastRenderMs = double.NegativeInfinity;

    // probe
    static long _ticks, _redraws, _renders, _modalGates;
    static double _spinMs;
    static long _vsyncMark;
    static double _windowStart;

    public static void Configure(string? fps, string? probe)
    {
        if (!string.IsNullOrWhiteSpace(fps))
        {
            var f = fps.Trim().ToLowerInvariant();
            if (f == "off") _forcedFps = GameFps;
            else if (double.TryParse(f, System.Globalization.NumberStyles.Float,
                         System.Globalization.CultureInfo.InvariantCulture, out var v) && v > 0)
                _forcedFps = Math.Clamp(v, GameFps, 360.0);
        }
        _probe = !string.IsNullOrWhiteSpace(probe) && probe.Trim() != "0";
        _census = probe?.Trim() == "2";
    }

    public static void Install()
    {
        if (_forcedFps is { } forced) SetTargetFps(forced);

        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            if (_forcedFps == null)
                SetTargetFps(RecompOne.Runtime.Runtime.View.GetFloat(FpsKey, 60f));
            Console.WriteLine(Redrawing
                ? $"[KF1] frame pacing: {TargetFps:0.#} fps drawn, the world at the game's own 20"
                : "[KF1] frame pacing: off, the game's own 20 fps");
        });

        HookAttach.OnOverlayLoad("frame pacing", Attach);
        Event.AddListener<OverlayLoadedEvent>(_ =>
        {
            _renderedSinceGate = false;
            TickStartMs = double.NegativeInfinity;
        });
    }

    public static void SetTargetFps(double fps)
    {
        TargetFps = Math.Clamp(fps, GameFps, 360.0);
        // Each redraw presents through the renderer's own VSync call (inside its
        // buffer flip, func_8001C050), and the host ceiling applies per VSync call,
        // so it must not undercut the rate.
        RecompOne.Runtime.Runtime.TargetFps = Redrawing ? Math.Max(60.0, TargetFps * 2.0) : 60.0;
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var gate = SymbolRegistry.Resolve("game", null, Gate);
        if (gate == null) return false;
        const BindingFlags f = BindingFlags.Public | BindingFlags.Static;
        HookManager.AddReplace(_self, gate, typeof(GateRedraw).GetMethod(nameof(GateReplace), f)!);
        HookManager.Commit();
        bool ok = HookAttach.Installed(gate);
        Console.WriteLine($"[KF1] frame pacing: gate {(ok ? "hooked" : "NOT hooked")}");
        return ok;
    }

    static double Now => Interrupts.ClockMs;

    /// <summary>Called by <see cref="ViewCarry"/> at the head of every renderer call.</summary>
    internal static void NoteRender(bool mainLoop)
    {
        _renderedSinceGate = true;
        _lastRenderMs = Now;
        _renders++;
        if (mainLoop && !InRedraw)
        {
            TickStartMs = _lastRenderMs;
            _ticks++;
        }
    }

    public static void GateReplace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        if (!Redrawing || c.RA != MainLoopGateReturn || !_renderedSinceGate)
        {
            if (c.RA != MainLoopGateReturn) _modalGates++;
            _renderedSinceGate = false;
            orig(c, m);
            return;
        }

        _renderedSinceGate = false;
        double frameMs = 1000.0 / TargetFps;
        var spin = Stopwatch.StartNew();
        double redrawMs = 0;

        while (true)
        {
            Interrupts.PollNow(c, m);

            uint cur = m.ReadU32(VCount), last = m.ReadU32(VLast);
            if (cur > last + 2 || cur < last)
            {
                m.WriteU32(VLast, cur);
                break;
            }

            double now = Now;
            double nextTick = TickStartMs + TickMs;
            // A redraw is due a frame after the last picture, and is only worth
            // drawing if it lands clear of the tick's own frame.
            if (now - _lastRenderMs >= frameMs - 0.25 && nextTick - now > frameMs * 0.5)
            {
                var t0 = spin.Elapsed.TotalMilliseconds;
                Redraw(c, m);
                redrawMs += spin.Elapsed.TotalMilliseconds - t0;
                continue;
            }

            double wait = Math.Min(_lastRenderMs + frameMs, Math.Max(nextTick, now)) - now;
            if (wait > 2.0) Thread.Sleep(1);
            else Thread.SpinWait(64);
        }

        _spinMs += spin.Elapsed.TotalMilliseconds - redrawMs;
        if (_probe) Report();
    }

    // KF2_FPS_PROBE=2: which RAM words a redraw changes. A redraw should only
    // rebuild the frame's primitives; anything else it moves is state the renderer
    // steps per call, which the redraws now run several times a tick.
    static bool _census;
    static byte[]? _before;
    static readonly Dictionary<uint, int> _changed = new();
    static long _censusRedraws;

    static void Redraw(CpuContext c, IMemory m)
    {
        if (_census && m is PSMemory pm)
        {
            _before ??= new byte[0x200000];
            pm.Ram[..0x200000].CopyTo(_before);
        }
        var snap = c.Snapshot();
        c.A0 = 0;
        c.A1 = 0;
        InRedraw = true;
        try
        {
            Dispatcher.Call(c, m, Renderer);
        }
        finally
        {
            InRedraw = false;
            c.Restore(snap);
        }
        _redraws++;
        if (_census && _before != null && m is PSMemory pm2) Census(pm2.Ram);
    }

    static void Census(ReadOnlySpan<byte> ram)
    {
        _censusRedraws++;
        for (int i = 0; i < 0x200000; i += 4)
            if (ram[i] != _before![i] || ram[i + 1] != _before[i + 1] || ram[i + 2] != _before[i + 2] || ram[i + 3] != _before[i + 3])
            {
                uint a = 0x80000000u + (uint)i;
                _changed[a] = _changed.TryGetValue(a, out var n) ? n + 1 : 1;
            }
        if (_censusRedraws != 200) return;

        // Runs of changed words, gaps of up to 16 bytes joined.
        var keys = _changed.Keys.OrderBy(k => k).ToList();
        var runs = new List<(uint Start, uint End, int Words, int MaxHits)>();
        foreach (var k in keys)
        {
            if (runs.Count > 0 && k - runs[^1].End <= 16)
                runs[^1] = (runs[^1].Start, k, runs[^1].Words + 1, Math.Max(runs[^1].MaxHits, _changed[k]));
            else runs.Add((k, k, 1, _changed[k]));
        }
        Console.WriteLine($"[KF1] redraw census over {_censusRedraws} redraws: {keys.Count} words in {runs.Count} runs");
        foreach (var r in runs.OrderByDescending(r => r.MaxHits).ThenBy(r => r.Start).Take(60))
            Console.WriteLine($"    0x{r.Start:X8}-0x{r.End + 3:X8}  {r.Words,6} words, changed in up to {r.MaxHits} redraws");
    }

    static void Report()
    {
        double now = Environment.TickCount64 / 1000.0;
        if (_windowStart == 0) { _windowStart = now; return; }
        double secs = now - _windowStart;
        if (secs < 1.0) return;
        Console.WriteLine($"[KF1] fps: {_renders / secs:0.0} drawn ({_redraws / secs:0.0} redraws), " +
                          $"{_ticks / secs:0.0} ticks/s, {_spinMs / Math.Max(1, _ticks):0.0} ms spun per tick, " +
                          $"{_modalGates / secs:0.0} modal gates/s, target {TargetFps:0.#}, " +
                          $"presents (VSync calls) {RecompOne.Runtime.Sdk.LibEtc.VSyncCalls - _vsyncMark}");
        _vsyncMark = RecompOne.Runtime.Sdk.LibEtc.VSyncCalls;
        _renders = _redraws = _ticks = _modalGates = 0;
        _spinMs = 0;
        _windowStart = now;
    }
}
