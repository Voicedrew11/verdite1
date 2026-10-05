using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hardware;
using RecompOne.Runtime.Host;
using RecompOne.Runtime.Memory;

namespace RecompOne.Runtime;

public enum RunMode
{
    Retail,
    Devkit
}

public sealed class HardResetSignal : Exception;

public static class Runtime
{
    public static CpuContext? Cpu { get; private set; }
    public static IMemory? Mem { get; private set; }
    public static Gpu? Gpu;
    public static Spu? Spu;
    public static Mdec? Mdec;
    public static Hardware.Timers? Timers;
    public static Cdrom.CdController? Cd;

    public static RunMode Mode { get; private set; } = RunMode.Retail;

    public static void SetMode(RunMode mode)
    {
        Mode = mode;
        //devkit vs retail, devkits reads from sim and has more ram
    }

    public static uint RamSize { get; internal set; } = MemoryMap.RetailRamSize;
    public static uint RamWordMask => (RamSize - 1) & ~3u;
    public static string CdPath => Config.ConfigManager.Game.CdPath;

    public static Func<string, string?>? DiscValidator;

    public static string? ValidateDisc(string path)
    {
        try
        {
            return DiscValidator?.Invoke(path);
        }
        catch (Exception e)
        {
            return e.Message;
        }
    }

    public static Config.ViewConfig View => Config.ConfigManager.View;

    //0025. The host frame throttle's target, in frames a second; 0 turns it off.
    //Exposed here because FrameClock is internal to the runtime and a port's
    //pacing lives outside it. See FrameClock.TargetFps for what this does and
    //does not pace.
    public static double TargetFps
    {
        get => Host.FrameClock.TargetFps;
        set => Host.FrameClock.TargetFps = value;
    }

    public static void ResyncFrameClock()
    {
        Host.FrameClock.Resync();
    }

    private static readonly List<Action<Config.ViewConfig>> _defaults = [];

    public static void Defaults(Action<Config.ViewConfig> apply)
    {
        _defaults.Add(apply);
    }

    internal static void ApplyDefaults()
    {
        foreach (var apply in _defaults) apply(Config.ConfigManager.View);
    }

    public static void SaveView()
    {
        Config.ConfigManager.SaveView(Host.Window.PanelManager.Panels);
    }

    public static Hardware.MemoryCard CardA = new("carda.sav") { Enabled = true };
    public static Hardware.MemoryCard CardB = new("cardb.sav") { Enabled = true };

    private static void LoadMemoryCards()
    {
        var g = Config.ConfigManager.Game;
        CardA = new MemoryCard(Fallback(g.CardAPath, "carda.sav")) { Enabled = g.CardAEnabled };
        CardB = new MemoryCard(Fallback(g.CardBPath, "cardb.sav")) { Enabled = g.CardBEnabled };

        static string Fallback(string path, string def)
        {
            return string.IsNullOrWhiteSpace(path) ? def : path;
        }
    }

    public static readonly RamLogger RamLog = new();
    public static readonly Dispatch.OverlayEventLog OverlayLog = new();

    private static bool _hostReady;

    public static void Initialize(string title)
    {
        if (!_hostReady)
        {
            _hostReady = true;
            Diagnostics.ConsoleMirror.Install();
            Host.GpuJobs.Run(() => HostWindow.Initialize(title));
            Audio.Initialize();
        }

        LoadMemoryCards();
        Audio.SetMasterVolume(Config.ConfigManager.Game.Muted ? 0f : Config.ConfigManager.Game.MasterVolume);
        if (Event.HasAnyListeners<RuntimeReadyEvent>()) Event.Dispatch(new RuntimeReadyEvent());
    }

    public static void WaitForValidDisc()
    {
        HostWindow.WaitForValidDisc();
    }

    /// <summary>
    /// Take in host events and draw one frame, from outside the game's own frame
    /// loop.
    ///
    /// WaitForValidDisc already runs exactly this loop, but only ever for its own
    /// condition, so anything else that has to keep the window alive while it
    /// works had nothing to call. A port that must build its game assembly before
    /// there is a game to run is the case: the work is seconds long, it happens
    /// after Initialize and before the first frame, and a window that stops
    /// pumping for that long is a hung window as far as the desktop is concerned.
    ///
    /// HostWindow.Pump is internal and this is the same call, so a caller drives
    /// its own loop and owns its own progress UI -- Popup and PopupManager.Register
    /// are already public, so that UI needs nothing further from here.
    /// </summary>
    public static void Pump() => HostWindow.Pump();

    public static string Title
    {
        get => HostWindow.Title;
        set => HostWindow.Title = value;
    }

    public static void SetTitle(string title)
    {
        HostWindow.SetTitle(title);
    }

    public static void SetIcon(byte[] data)
    {
        HostWindow.SetIcon(data);
    }

    public static void SetIcon(byte[] rgba, int width, int height)
    {
        HostWindow.SetIcon(rgba, width, height);
    }

    /// <summary>
    /// What the window calls itself to the desktop. On Wayland it is the whole of
    /// how a compositor finds the window's desktop entry, and so its icon; set it
    /// before <see cref="Initialize"/>.
    /// </summary>
    public static string AppId
    {
        get => HostWindow.AppId;
        set => HostWindow.AppId = value;
    }

    public static void SetIcons(IReadOnlyList<(byte[] Rgba, int Width, int Height)> images)
    {
        HostWindow.SetIcons(images);
    }

    public static void ClearIcon()
    {
        HostWindow.ClearIcon();
    }

    public static void ShowNotice(string message)
    {
        Host.Window.NoticePopup.Show(message);
    }

    public static void SetStartupNotice(string message, string title = "common.notice",
        string ackKey = "StartupNoticeAck")
    {
        Host.Window.StartupNoticePopup.Set(message, title, ackKey);
    }

    public static void AddLanguages(string json)
    {
        Host.Window.Localization.Merge(json);
    }

    public static bool AddLanguages(System.Reflection.Assembly assembly, string resourceName)
    {
        return Host.Window.Localization.MergeEmbedded(assembly, resourceName);
    }

    public static void SetContext(CpuContext c, IMemory m)
    {
        Cpu = c;
        Mem = m;
    }

    private static volatile bool _hardResetPending;

    public static bool HardResetPending => _hardResetPending;

    public static void HardReset()
    {
        _hardResetPending = true;
    }

    private static void ResetForBoot()
    {
        Audio.Detach();

        Sdk.LibCd.Reset();
        Sdk.LibCdStream.Reset();
        Assets.Xa.XaRouter.Reset();
        Sdk.LibPad.Reset();
        Sdk.LibApi.Reset();
        Dispatch.Dispatcher.Reset();
        Bios.BiosB.Reset();
        OverlayLog.Clear();

        Cpu = null;
        Mem = null;
        Gpu = null;
        Spu = null;
        Cd = null;

        if (Hle.GpuHle.Backend is { Ready: true } backend)
        {
            backend.FillRect(0, 0, Gpu.VramWidth, Gpu.VramHeight, 0);
            backend.Flush();
        }
    }

    private static volatile bool _gameDone;
    private static readonly System.Diagnostics.Stopwatch _presentWatch = System.Diagnostics.Stopwatch.StartNew();
    private static double _nextPresentMs;
    
    public static void Run(Action boot)
    {
        Pgxp.PgxpGpu.Init();
        Host.GpuJobs.Claim();
        
        var thread = new Thread(() => RunGame(boot))
        {
            IsBackground = true,
            Name = "game"
        };
        
        thread.Start();
        
        while (!_gameDone)
            PresentLoop();
    }
    
    private static void RunGame(Action boot)
    {
        while (true)
            try
            {
                boot();
                break;
            }
            catch (HardResetSignal)
            {
                Console.WriteLine("[Runtime] hard reset, game restarting");
                ResetForBoot();
            }
            catch (Exception e)
            {
                Console.Error.WriteLine($"[Runtime] runtime has crashed: {e}");
                break;
            }
        
        _gameDone = true;
    }
    
    private static double _idleMark;
    
    private static void IdleRedraw()
    {
        var now = FrameClock.Now;
        if (now - _idleMark < FrameClock.FrameMs) return;
        
        _idleMark = now;
        HostWindow.Compose(Gpu, false);
        Host.GpuJobs.Drain();
    }
    
    private static void PresentLoop()
    {
        Host.GpuJobs.Drain();
        
        if (!HostWindow.Ready)
        {
            Thread.Sleep(1);
            return;
        }
        
        HostWindow.PumpEvents();
        
        var interp = Interp.Interp.Backend;
        
        if (interp == null || !interp.Acquire())
        {
            IdleRedraw();
            Thread.Sleep(1);
            return;
        }
        
        _idleMark = 0.0;
        HostWindow.AdvanceFrame();
        
        var frames = interp.BeginPresent();
        var pace = interp.PaceMs;
        
        for (var i = 0; i < frames; i++)
        {
            if (!interp.Affordable(i)) break;
            
            interp.Compose(i);
            Pace(pace);
            HostWindow.Compose(Gpu);
            Host.GpuJobs.Drain();
        }
        
        if (frames == 0)
        {
            interp.Compose(0);
        }
        
        interp.EndPresent();
    }
    
    //the display frames only read as smooth if they land evenly in time, so the schedule is held against the clock instead of leaving it to the swap
    private static void Pace(double intervalMs)
    {
        if (Interrupts.Turbo) return;

        var now = _presentWatch.Elapsed.TotalMilliseconds;
        
        if (intervalMs <= 0.0 || _nextPresentMs <= 0.0 || now - _nextPresentMs > 250.0)
        {
            _nextPresentMs = now + intervalMs;
            return;
        }
        
        var wait = _nextPresentMs - now;
        
        if (wait > 1.5) Thread.Sleep((int)(wait - 1.0));
        while (_presentWatch.Elapsed.TotalMilliseconds < _nextPresentMs) Thread.SpinWait(32);
        
        _nextPresentMs += intervalMs;
    }
    
    public static void PresentFrame()
    {
        if (_hardResetPending)
        {
            _hardResetPending = false;
            throw new HardResetSignal();
        }

        //0045. The frame boundary is the end of this call, not a hook, so the
        //profiler's frames do not depend on anything it is meant to be measuring.
        var profile = Diagnostics.Profiler.Begin(Diagnostics.Profiler.Present);
        try
        {
            PresentFrameCore();
        }
        finally
        {
            Diagnostics.Profiler.End(profile);
            Diagnostics.Profiler.FrameMark();
        }
    }

    private static void PresentFrameCore()
    {
        Interp.Interp.Backend?.Publish();

        // The port presents from inside the game's own VSync, on one thread:
        // LibEtc.VSync -> PresentFrame -> HostWindow.Present -> DrawPanels. The
        // merge to 0409bc2 moved presentation onto upstream's PresentLoop, which
        // only runs when the host calls Runtime.Run(boot) -- and this port's
        // hand-owned Program.cs calls Entry.Run directly, so nothing ever called
        // DoRender and the window stayed black while the game ran normally
        // underneath it. Restored here, where it was and where every measurement
        // in the port was taken.
        HostWindow.Present(Gpu);

        Audio.Attach(Spu);
        FrameClock.MarkFrame();
        // Upstream throttles in PresentLoop, which this port never enters.
        FrameClock.Throttle();
        var ticks = Diagnostics.Profiler.Begin(Diagnostics.Profiler.Ticks);
        Sdk.LibCd.Tick();
        if (Cpu != null && Mem != null) Sdk.LibMcrd.Tick(Cpu, Mem);
        if (Mem != null)
        {
            Bios.BiosB.RefreshPad(Mem);
            Sdk.LibPad.Refresh(Mem);
        } //is this correct?
        Diagnostics.Profiler.End(ticks);

        // Only on upstream's blocking timeline. On the pin's, LibEtc.TickVBlank
        // delivers IRQ 0 on its own wall-clock grid and a present is not a
        // vblank -- raising it here as well would deliver every vblank twice,
        // at the render rate rather than at 60 Hz.
        if (Sdk.LibEtc.BlockingVSync) Interrupts.Raise(0);
    }

    public static void DispatchIrq(int irq)
    {
        if (Cpu != null && Mem != null)
            Interrupts.Deliver(irq, Cpu, Mem);
    }

    public static void Shutdown()
    {
        Audio.Shutdown();
        HostWindow.Shutdown();
    }
}