using System.Diagnostics;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Diagnostics;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hardware;
using RecompOne.Runtime.Memory;

namespace RecompOne.Runtime.Sdk;

public static class LibEtc
{
    internal static double LastWaitMs = double.NegativeInfinity;
    
    private static int _vcount;
    private static readonly VSyncEvent _vsyncEvent = new();

    /// <summary>
    /// Which of the two vblank timelines VSync runs on.
    ///
    /// Upstream grew its own after the pin: Interrupts owns a wall-clock grid and
    /// VSync *blocks* in WaitVBlanks until the count reaches its target, which is
    /// what the hardware does and is also a hard 60 Hz ceiling on every VSync
    /// call. This port cannot have that ceiling -- FramePacing hands FrameClock a
    /// deliberately permissive rate and keeps its own deadline at DrawOTag, and
    /// MenuPacing, LoadPacing and SpriteAnim are all measured against a VSync that
    /// returns immediately. So 0021's non-blocking grid stays the default and
    /// upstream's is here beside it, to be compared rather than replaced.
    ///
    /// KF2_VSYNC=block is the console switch.
    /// </summary>
    public static bool BlockingVSync;

    //0042. Presents counted where they happen, not where a hook says they do. A
    //port that paces from hooks on this function cannot use those hooks to notice
    //that they have stopped running, so this is counted in the body itself: the
    //calls that reach PresentFrame, and on request the managed stack of one of
    //them, which shows whether the call still came through the hook trampoline.
    public static long VSyncCalls;
    public static volatile bool CaptureNextStack;
    public static string? CapturedStack;

    //The vblank is time, not a call. On hardware the interrupt fires every 16.7 ms
    //whether or not the game is ready for it: a loop blocked on a CD read misses
    //pictures, not vblanks. This port advanced _vcount once per VSync call instead,
    //so every game-side clock hung on the vblank -- the music sequencer among them --
    //ran at the rendered frame rate: half speed on the CD-bound title screen (one
    //VSync call per ~15 fps picture), half again at KF2_FPS=15 (two calls per 66 ms
    //frame). Advance on a wall-clock grid and deliver the vblanks missed since the
    //last call as a burst, which is what the hardware's interrupt would have done
    //across the same gap.
    private static readonly Stopwatch VBlankClock = Stopwatch.StartNew();
    private static double _nextVBlankMs;
    private static bool _timelineStarted;

    private const double VBlankMs = 1000.0 / 60.0;

    //A host stall longer than this (window drag, breakpoint) resyncs the grid
    //instead of fast-forwarding the game through the whole gap.
    private const int MaxCatchUpVBlanks = 120;

    private static double HblankHz => LibGpu.Pal ? 15625.0 : 15734.0; //correct?

    private static int _lastVSyncCount;
    private static double _lastVSyncMs;

    public static void VSync(CpuContext c, IMemory m)
    {
        var mode = (int)c.A0;
        if (Log.VSyncOn) Log.Sdk($"VSync({mode})");

        if (mode < 0)
        {
            c.V0 = BlockingVSync ? (uint)Interrupts.VBlankCount : (uint)_vcount;
            return;
        }

        if (mode == 1)
        {
            //The pin returned 0 here and the port's measurements were taken
            //against that, so only the blocking timeline reports real hblanks.
            c.V0 = BlockingVSync ? Elapsed() : 0;
            return;
        }

        VSyncCalls++;
        if (CaptureNextStack)
        {
            CaptureNextStack = false;
            CapturedStack = Environment.StackTrace;
        }

        LastWaitMs = Interrupts.ClockMs;
        //Upstream's frame interpolator feeds off this whichever timeline runs; it
        //is inert unless Interp is enabled.
        Interp.VideoRate.Push(mode == 0 ? 1 : mode);

        //0045. Only the presenting call is a section; the queries above return at
        //once. Not a try/finally: PresentFrame's hard reset is thrown before it
        //opens a section, and anything left open is closed by the next outer End.
        var profile = Profiler.Begin(Profiler.VSync);
        try
        {
            Present(c, m, mode);
        }
        finally
        {
            Profiler.End(profile);
        }
    }

    private static void Present(CpuContext c, IMemory m, int mode)
    {
        Runtime.PresentFrame();

        if (BlockingVSync)
        {
            WaitVBlanks(c, m, mode == 0 ? 1 : mode);
            var elapsed = Elapsed();
            _lastVSyncCount = Interrupts.VBlankCount;
            _lastVSyncMs = Interrupts.ClockMs;
            _vcount++;

            if (Event.HasAnyListeners<VSyncEvent>())
            {
                var e = _vsyncEvent;
                e.Context = c;
                e.Memory = m;
                e.Frame = _vcount;
                Event.Dispatch(e);
            }

            c.V0 = elapsed;
            return;
        }

        var vblank = Profiler.Begin(Profiler.VBlank);
        AdvanceVBlanks(c, m);
        Profiler.End(vblank);
        c.V0 = 0;
    }

    //0091. A game that waits for its vblank handler without calling VSync gets its
    //vblanks from the interrupt poll instead. Off unless the port sets it.
    public static bool VBlankFromPoll;
    private static bool _advancing;

    internal static void PollVBlanks(CpuContext c, IMemory m)
    {
        if (BlockingVSync || _advancing) return;
        AdvanceVBlanks(c, m);
    }

    private static void AdvanceVBlanks(CpuContext c, IMemory m)
    {
        _advancing = true;
        try
        {
            AdvanceVBlanksOnGrid(c, m);
        }
        finally
        {
            _advancing = false;
        }
    }

    private static void AdvanceVBlanksOnGrid(CpuContext c, IMemory m)
    {
        var now = VBlankClock.Elapsed.TotalMilliseconds;
        if (!_timelineStarted)
        {
            _timelineStarted = true;
            _nextVBlankMs = now;
        }
        else if (now - _nextVBlankMs > MaxCatchUpVBlanks * VBlankMs)
        {
            // The host was stopped, so discard its stale vblanks; retain only
            // the current boundary below rather than fast-forwarding the game.
            _nextVBlankMs = now;
        }

        var n = 0;
        while (_nextVBlankMs <= now && n < MaxCatchUpVBlanks)
        {
            _nextVBlankMs += VBlankMs;
            n++;
            TickVBlank(c, m);
        }

        //The loop cap is the same size as the resync threshold, so it only bites at
        //the exact boundary; if it ever does, snap the grid to now rather than carry
        //the leftover gap into the next call and lag a frame behind for good.
        if (n == MaxCatchUpVBlanks && _nextVBlankMs < now) _nextVBlankMs = now;
    }

    private static void TickVBlank(CpuContext c, IMemory m)
    {
        _vcount++;

        //0021: RCntCNT3/EvSpINT, the vblank root counter's event, is not delivered
        //here. IRQ 0 below delivers it (Interrupts.ServiceIrq -> DeliverIrqEvents),
        //as on upstream's blocking timeline; delivering it here as well ran every
        //handler on it twice a vblank.

        if (Event.HasAnyListeners<VSyncEvent>())
        {
            var e = _vsyncEvent;
            e.Context = c;
            e.Memory = m;
            e.Frame = _vcount;
            Event.Dispatch(e);
        }

        //The PSY-Q vblank callback (IRQ 0) belongs to the vblank too, not to the
        //VSync call -- the game's own frame counter hangs off it.
        Runtime.DispatchIrq(0);
    }

    private static uint Elapsed()
    {
        return (uint)((Interrupts.ClockMs - _lastVSyncMs) * HblankHz / 1000.0) & 0xFFFF;
    }

    private const double SleepMarginMs = 2.0;

    private static void WaitVBlanks(CpuContext c, IMemory m, int count)
    {
        var profile = Profiler.Begin(Profiler.VBlankWait);
        WaitVBlanksCore(c, m, count);
        Profiler.End(profile);
    }

    private static void WaitVBlanksCore(CpuContext c, IMemory m, int count)
    {
        var target = _lastVSyncCount + count;

        while (Interrupts.VBlankCount < target)
        {
            if (Interrupts.Turbo)
            {
                Interrupts.ForceVBlank(c, m);
                continue;
            }

            var remaining = Interrupts.MsToNextVBlank;
            if (remaining > SleepMarginMs)
            {
                var ms = (int)(remaining - SleepMarginMs);
                if (ms > 0) Thread.Sleep(ms);
            }
            else
            {
                Thread.SpinWait(64);
            }

            Interrupts.PollNow(c, m);
        }
    }
}
