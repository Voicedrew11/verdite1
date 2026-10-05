using System.Numerics;
using ImGuiNET;
using Silk.NET.Input;
using Silk.NET.Maths;
using Silk.NET.OpenGL;
using Silk.NET.OpenGL.Extensions.ImGui;
using Silk.NET.Windowing;
using RecompOne.Runtime.Config;
using RecompOne.Runtime.Diagnostics;
using RecompOne.Runtime.Hardware;
using RecompOne.Runtime.Host.Window;

namespace RecompOne.Runtime.Host;

public static class HostWindow
{
    private static IWindow? _window;
    private static GL? _gl;
    private static ImGuiController? _imgui;
    private static bool _headless;
    private static Gpu? _gpu;

    private static uint _displayTex;
    private static uint _vramTex;
    private static uint _ramTex;
    private static Hle.GlCore? _glBackend;

    private static byte[] _rgbDisplay = [];
    private static byte[] _rgbVram = [];
    private static byte[] _ramFront = new byte[Memory.RamLogger.Width * Memory.RamLogger.Height * 4];
    private static byte[] _ramBack = new byte[Memory.RamLogger.Width * Memory.RamLogger.Height * 4];
    private static Task? _ramTask;
    private static volatile bool _ramReady;
    private static int _ramFrame;

    private static bool _layoutPending = true;
    private static bool _closed;

    private const int RedockCooldownFrames = 8;
    private static int _redockCooldown;

    public static void RequestLayout()
    {
        _layoutPending = true;
    }

    private static float _dpiScale = 1f;

    public static float DpiScale => _dpiScale;

    private static unsafe float QueryDpiScale()
    {
        try
        {
            var glfw = Silk.NET.GLFW.Glfw.GetApi();
            var monitor = glfw.GetPrimaryMonitor();
            if (monitor != null)
            {
                glfw.GetMonitorContentScale(monitor, out var xs, out var ys);
                var s = MathF.Max(xs, ys);
                if (s >= 0.5f && s <= 8f) return s;
            }
        }
        catch (Exception e)
        {
            Console.WriteLine($"[Host] cant read scale: {e.Message}");
        }

        try
        {
            var fb = _window!.FramebufferSize;
            var size = _window.Size;
            if (size.X > 0 && fb.X > 0)
            {
                var s = (float)fb.X / size.X;
                if (s >= 0.5f && s <= 8f) return s;
            }
        }
        catch
        {
        }

        return 1f;
    }

    private static GraphicsAPI[] ApiChain()
    {
        if (OperatingSystem.IsMacOS())
            return
            [
                new GraphicsAPI(ContextAPI.OpenGL, ContextProfile.Core, GlDebug.Flags(ContextFlags.ForwardCompatible),
                    new APIVersion(4, 1))
            ];

        var requested = Hle.GpuBackendFactory.Parse(ConfigManager.View.GpuBackend);
        var core45 = new GraphicsAPI(ContextAPI.OpenGL, ContextProfile.Core, GlDebug.Flags(ContextFlags.Default),
            new APIVersion(4, 5));
        var core33 = new GraphicsAPI(ContextAPI.OpenGL, ContextProfile.Core, GlDebug.Flags(ContextFlags.Default),
            new APIVersion(3, 3));
        var compat21 = new GraphicsAPI(ContextAPI.OpenGL, ContextProfile.Compatability, GlDebug.Flags(ContextFlags.Default),
            new APIVersion(2, 1));

        return requested switch
        {
            Hle.GlBackendKind.Gl21 => [compat21],
            Hle.GlBackendKind.Gl33 => [core33, compat21],
            _ => [core45, core33, compat21]
        };
    }

    /// <summary>
    /// What the window calls itself to the desktop, which is how a compositor
    /// finds its desktop entry -- and so its icon -- on Wayland.
    /// </summary>
    public static string AppId = "recompone";

    /// <summary>
    /// GLFW leaves the Wayland app id and the X11 class empty, so a compositor has
    /// nothing to match a desktop entry against. Silk 2.22 has no name for the
    /// Wayland hint, which is GLFW 3.4's <c>GLFW_WAYLAND_APP_ID</c> (0x00026001).
    /// </summary>
    private static void HintAppId(string id)
    {
        if (string.IsNullOrWhiteSpace(id)) return;
        try
        {
            var glfw = Silk.NET.GLFW.Glfw.GetApi();
            glfw.WindowHintString(0x00026001, id);
            glfw.WindowHintString((int)Silk.NET.GLFW.WindowHintString.X11ClassName, id);
            glfw.WindowHintString((int)Silk.NET.GLFW.WindowHintString.X11InstanceName, id);
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[Host] app id: {e.Message}");
        }
    }

    public static void Initialize(string title)
    {
        ConfigManager.Load();
        Runtime.ApplyDefaults();
        Pgxp.Pgxp.Load();
        Interp.Interp.Load();

        foreach (var api in ApiChain())
            try
            {
                var options = WindowOptions.Default with
                {
                    Size = new Vector2D<int>(1280, 720),
                    Title = title,
                    // 0064. ApplySwapInterval owns the interval; Silk's puts 1 back over it.
                    VSync = false,
                    UpdatesPerSecond = 0,
                    FramesPerSecond = 0,
                    WindowState = ConfigManager.View.Fullscreen && !ConfigManager.View.Borderless
                        ? WindowState.Fullscreen
                        : WindowState.Normal,
                    API = api
                };
                _window = Silk.NET.Windowing.Window.Create(options);
                FrameClock.VSync = ConfigManager.View.VSync;
                Interp.Interp.VSync = ConfigManager.View.VSync;
                Interp.Interp.RefreshRate = QueryRefreshRate();
                Console.WriteLine($"[Host] monitor refresh: {Interp.Interp.RefreshRate} hz");
                _window.Load += OnLoad;
                _window.Render += OnRender;
                _window.Closing += OnClosing;
                _window.Move += OnMove;
                HintAppId(AppId);
                _window.Initialize();
                if (ConfigManager.View.Fullscreen && ConfigManager.View.Borderless) SetFullscreen(true);
                Console.WriteLine($"[Host] gl context {api.Version.MajorVersion}.{api.Version.MinorVersion} {api.Profile}");
                return;
            }
            catch (Exception e)
            {
                Console.Error.WriteLine(
                    $"[Host] context {api.Version.MajorVersion}.{api.Version.MinorVersion} unavailable: {e.Message}");
                _window = null;
            }

        Console.Error.WriteLine("[Host] no usable gl context were found");
        _headless = true;
    }

    public static string Title
    {
        get => _window?.Title ?? "";
        set
        {
            if (_window != null) _window.Title = value ?? "";
        }
    }

    public static void SetTitle(string title)
    {
        Title = title;
    }

    private static Silk.NET.Core.RawImage[]? _pendingIcons;

    public static void SetIcon(byte[] data)
    {
        try
        {
            var rgba = Decode(data, out var w, out var h);
            if (rgba == null)
            {
                Console.Error.WriteLine("[Host] icon format not supported");
                return;
            }

            SetIcon(rgba, w, h);
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[Host] failed to set icon: {e.Message}");
        }
    }

    public static void SetIcon(byte[] rgba, int width, int height)
    {
        if (width <= 0 || height <= 0 || rgba.Length < width * height * 4)
        {
            Console.Error.WriteLine("[Host] icon pixel buffer does not match its size");
            return;
        }

        SetIcons([(rgba, width, height)]);
    }

    /// <summary>
    /// Several sizes of one icon. GLFW picks the nearest to what the desktop asks
    /// for, so a pixel-art icon supplied at its exact multiples is never resampled.
    /// </summary>
    public static void SetIcons(IReadOnlyList<(byte[] Rgba, int Width, int Height)> images)
    {
        var list = new List<Silk.NET.Core.RawImage>(images.Count);
        foreach (var (rgba, w, h) in images)
        {
            if (w <= 0 || h <= 0 || rgba.Length < w * h * 4)
            {
                Console.Error.WriteLine("[Host] icon pixel buffer does not match its size");
                continue;
            }

            list.Add(new Silk.NET.Core.RawImage(w, h, rgba));
        }

        if (list.Count == 0) return;
        _pendingIcons = list.ToArray();
        Apply(_pendingIcons);
    }

    public static void ClearIcon()
    {
        _pendingIcons = null;
        if (_window == null) return;
        try
        {
            _window.SetWindowIcon(ReadOnlySpan<Silk.NET.Core.RawImage>.Empty);
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[Host] failed to clear icon: {e.Message}");
        }
    }

    private static void Apply(Silk.NET.Core.RawImage[] icons)
    {
        if (_window == null) return;
        try
        {
            _window.SetWindowIcon(icons);
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[Host] failed to set icon: {e.Message}");
        }
    }

    private static byte[]? Decode(byte[] data, out int width, out int height)
    {
        width = height = 0;
        if (data.Length < 4) return null;

        if (data[0] == 0 && data[1] == 0 && data[2] == 1 && data[3] == 0)
        {
            var best = LargestIcoEntry(data);
            if (best == null) return null;
            data = best;
        }

        var img = StbImageSharp.ImageResult.FromMemory(data, StbImageSharp.ColorComponents.RedGreenBlueAlpha);
        if (img == null) return null;
        width = img.Width;
        height = img.Height;
        return img.Data;
    }

    private static byte[]? LargestIcoEntry(byte[] ico)
    {
        int count = BitConverter.ToUInt16(ico, 4);
        var bestArea = -1;
        byte[]? best = null;

        for (var i = 0; i < count; i++)
        {
            var e = 6 + i * 16;
            if (e + 16 > ico.Length) break;

            var w = ico[e] == 0 ? 256 : ico[e];
            var h = ico[e + 1] == 0 ? 256 : ico[e + 1];
            var size = BitConverter.ToInt32(ico, e + 8);
            var offset = BitConverter.ToInt32(ico, e + 12);
            if (size <= 0 || offset < 0 || offset + size > ico.Length) continue;

            var png = size > 8 && ico[offset] == 0x89 && ico[offset + 1] == 0x50 &&
                      ico[offset + 2] == 0x4E && ico[offset + 3] == 0x47;
            if (!png) continue;

            var area = w * h;
            if (area <= bestArea) continue;
            bestArea = area;
            best = ico.AsSpan(offset, size).ToArray();
        }

        return best;
    }

    public static void Present(Gpu? gpu)
    {
        _gpu = gpu;
        if (_headless || _window == null) return;
        // 0007. The pad is polled outside the frame loop, so a game that waits
        // on it without vsyncing does not read one frozen snapshot forever.
        _pumpedAt = _renderedAt = _pumpClock.Elapsed.TotalMilliseconds;
        var events = Profiler.Begin(Profiler.HostEvents);
        try
        {
            _window.DoEvents();
        }
        catch (Exception e)
        {
            Console.WriteLine(e.Message);
        }

        if (_window.IsClosing)
        {
            Runtime.Shutdown();
            Environment.Exit(0);
        }

        InputManager.Poll();
        if (InputManager.ConsumeTopBarToggle())
        {
            ConfigManager.View.HideTopBar = !ConfigManager.View.HideTopBar;
            ConfigManager.SaveView(PanelManager.Panels);
        }

        if (InputManager.ConsumeFullscreenToggle())
        {
            ConfigManager.View.Fullscreen = !ConfigManager.View.Fullscreen;
            SetFullscreen(ConfigManager.View.Fullscreen);
            ConfigManager.SaveView(PanelManager.Panels);
        }

        Profiler.End(events);

        // 0064. VSync on Wayland is held here, since the swap there does not wait.
        if (_cpuVSync)
        {
            var wait = Profiler.Begin(Profiler.VSyncWait);
            FrameClock.WaitRefresh(Interp.Interp.RefreshRate);
            Profiler.End(wait);
        }

        // 0045. What OnRender does not claim for itself is Silk's own swap, which
        // is where the thread waits on the driver.
        var render = Profiler.Begin(Profiler.HostRender);
        RenderFrame();
        Profiler.End(render);
        GlDebug.Poll(_gl);
        FrameClock.MarkPresent();
        _presentedAt = _pumpClock.Elapsed.TotalMilliseconds;
    }

    // ---- how VSync is kept off Wayland (0066) -----------------------------------
    //
    // On Windows, in a window the compositor presents -- windowed, or the
    // borderless display mode -- the interval stays 0, the frame is composed and
    // flushed, and the swap waits for the display's own vertical blank (VBlankWait,
    // the kernel's vblank event). On an integrated Radeon the interval fell into
    // stretches of 30-55 fps, because its swap blocks until the flip it queues;
    // this held 99-100% of frame intervals within a millisecond of 16.7 in four
    // alternating runs. It cannot tear there, since the compositor takes whole
    // frames. GLFW's fullscreen bypasses the compositor, where a flip at interval 0
    // is not held to the blank and it does tear (seen in play), so fullscreen keeps
    // the interval. Waiting for the blank and then swapping at interval 1 does not
    // help: it locks at 30, one refresh in the wait and one in the swap.
    //
    // Elsewhere VSync is the swap interval, and its swap is deferred to the next present. A blocking swap
    // waits for the GPU to finish the frame and then for the flip, and this port
    // presents from inside the game's own VSync on the game's own thread, so the
    // frame's heaviest GPU work -- the occlusion pass and the composite, issued at
    // present -- could not overlap the next frame's game code the way it does with
    // VSync off: with SSAO on High, 56.0 fps swapped at once against 60.0 deferred,
    // alternating, in both rounds. One refresh of latency.
    //
    // KF2_SWAP=interval takes the deferred interval in a composed window too,
    // KF2_SWAP=immediate the interval swapped where it was composed, and
    // KF2_SWAP=vblank the vblank wait in fullscreen as well (it tears there).

    private enum SwapMode { Silk, Deferred, VBlank }

    private static readonly string SwapAsked =
        (Environment.GetEnvironmentVariable("KF2_SWAP") ?? "").Trim().ToLowerInvariant();

    private static SwapMode _swapMode = SwapMode.Silk;
    private static bool _swapPending;
    private static bool _vblankFailed;

    /// <summary>Compose the window's frame and put it on the screen the way the
    /// swap mode says.</summary>
    private static void RenderFrame()
    {
        if (_swapMode == SwapMode.VBlank)
        {
            _window!.DoRender();
            // Read again: the settings window is drawn inside DoRender and can turn
            // VSync over, and Silk has then swapped this frame itself or not at all.
            if (_swapMode != SwapMode.VBlank) return;
            _gl?.Flush();
            var wait = Profiler.Begin(Profiler.VSyncWait);
            var waited = VBlankWait.Wait();
            Profiler.End(wait);
            _window.GLContext?.SwapBuffers();
            if (waited) return;
            // A wait that fails returns at once, which is no VSync at all.
            Console.Error.WriteLine("[Host] the vblank wait failed; VSync falls back to the swap interval");
            _vblankFailed = true;
            ApplySwapInterval();
            return;
        }

        var deferred = _swapMode == SwapMode.Deferred;
        if (deferred) SwapPending();
        _window!.DoRender();
        if (_swapMode != SwapMode.Deferred) return;
        _gl?.Flush();
        _swapPending = true;
    }

    /// <summary>Put the frame that is waiting on the screen, if one is.</summary>
    private static void SwapPending()
    {
        if (!_swapPending) return;
        _swapPending = false;
        _window!.GLContext?.SwapBuffers();
    }

    /// <summary>Take the swap from Silk, or hand it back with anything waiting shown
    /// first.</summary>
    private static void SetSwapMode(SwapMode mode)
    {
        if (_window == null) return;
        if (mode != SwapMode.Deferred) SwapPending();
        _swapMode = mode;
        _window.ShouldSwapAutomatically = mode == SwapMode.Silk;
    }

    /// <summary>The vblank of the monitor the window is on now.</summary>
    private static bool OpenVBlank() =>
        _window?.Native?.Win32 is { } w32 && VBlankWait.Open(w32.HDC);

    public static bool Ready => !_headless && _window != null;
    
    public static void PumpEvents()
    {
        if (_headless || _window == null) return;
        
        try
        {
            _window.DoEvents();
        }
        catch (Exception e)
        {
            Console.WriteLine(e.Message);
        }
        
        if (_window.IsClosing)
        {
            Runtime.Shutdown();
            Environment.Exit(0);
        }
        
        InputManager.Poll();
        
        if (InputManager.ConsumeTopBarToggle())
        {
            ConfigManager.View.HideTopBar = !ConfigManager.View.HideTopBar;
            ConfigManager.SaveView(PanelManager.Panels);
        }
        
        if (InputManager.ConsumeFullscreenToggle())
        {
            ConfigManager.View.Fullscreen = !ConfigManager.View.Fullscreen;
            SetFullscreen(ConfigManager.View.Fullscreen);
            ConfigManager.SaveView(PanelManager.Panels);
        }
    }
    
    public static void Compose(Gpu? gpu, bool counts = true)
    {
        _gpu = gpu;
        if (_headless || _window == null) return;
        
        try
        {
            RenderFrame();
        }
        catch (NotImplementedException) //doesnt fucking work
        {
        }
        
        if (counts) FrameClock.MarkPresent();
    }
    
    internal static void Pump()
    {
        if (_headless || _window == null) return;
        try
        {
            _window.DoEvents();
        }
        catch
        {
        }

        if (_window.IsClosing)
        {
            Runtime.Shutdown();
            Environment.Exit(0);
        }

        RenderFrame();
    }

    static readonly System.Diagnostics.Stopwatch _pumpClock = System.Diagnostics.Stopwatch.StartNew();
    static double _pumpedAt = double.NegativeInfinity;
    static double _renderedAt = double.NegativeInfinity;

    /// <summary>
    /// Take in host events and refresh the pad, without waiting for a frame.
    ///
    /// For code that runs outside the frame loop: a game busy-waiting on the
    /// controller never reaches Present, and input that is only polled there can
    /// never change under it. Does nothing if the host was pumped less than
    /// minIntervalMs ago, so a caller in a tight loop is free to ask every time.
    /// </summary>
    public static void PumpInput(double minIntervalMs)
    {
        if (_headless || _window == null) return;
        double now = _pumpClock.Elapsed.TotalMilliseconds;
        if (now - _pumpedAt < minIntervalMs) return;
        _pumpedAt = now;

        try { _window.DoEvents(); } catch { }
        if (_window.IsClosing) { Runtime.Shutdown(); Environment.Exit(0); }
        InputManager.Poll();

        // Keep drawing while the game is stuck outside its frame loop, so the UI
        // stays live rather than going grey -- but at display rate, not at the
        // rate the game happens to poll the pad.
        //
        // Only once the game has stopped presenting, measured from where the last
        // Present *ended*. Measured from its start, a frame of 16 ms or more --
        // every frame, once the swap waits for a 60 Hz vblank -- read as stuck,
        // and the pad read drew and swapped a second time: two blocking swaps a
        // frame, 30 fps and 22 ms in "buffer swap + driver" with VSync on.
        //
        // A deferred swap is not left waiting that long: a game that has stopped
        // presenting for two refreshes gets its last frame on the screen now.
        if (_swapPending && now - _presentedAt >= SwapStaleMs) SwapPending();
        if (now - _presentedAt < KeepAliveAfterMs) return;
        if (now - _renderedAt < 16.0) return;
        _renderedAt = now;
        RenderFrame();
    }

    const double KeepAliveAfterMs = 250.0;
    const double SwapStaleMs = 34.0;
    static double _presentedAt = double.NegativeInfinity;

    public static void Shutdown()
    {
        if (!_headless && _window != null && !_window.IsClosing)
            _window.Close();
        InputManager.Shutdown();
    }

    /// <summary>Cover the screen or not, the way <c>ConfigManager.View.Borderless</c>
    /// says: GLFW's fullscreen, or a borderless window the size of the monitor.</summary>
    public static void SetFullscreen(bool on)
    {
        if (_window == null) return;
        // Windows only, where the vblank wait needs a composed window. Wayland lets
        // no client place itself, and an X11 window manager fits an undecorated
        // one to the work area, so elsewhere a monitor-sized window never covers
        // the screen; there Borderless is the window manager's fullscreen.
        var borderless = on && ConfigManager.View.Borderless && OperatingSystem.IsWindows();

        if (!borderless && _borderlessActive)
        {
            _window.WindowBorder = WindowBorder.Resizable;
            _window.Position = _windowedPosition;
            _window.Size = _windowedSize;
            _borderlessActive = false;
        }

        if (borderless)
        {
            if (_window.WindowState != WindowState.Normal) _window.WindowState = WindowState.Normal;
            if (!_borderlessActive)
            {
                _windowedPosition = _window.Position;
                _windowedSize = _window.Size;
            }

            // The monitor the window is on, whole: taskbar included, as a game
            // expects. The compositor still presents it, which is the point -- it
            // never tears, so VSync can be held on the display's own blank.
            if (MonitorRect() is { } bounds)
            {
                _window.WindowBorder = WindowBorder.Hidden;
                _window.Position = bounds.Origin;
                // One row past the bottom. A window exactly the monitor's size is
                // promoted off the compositor to a flip of its own, and at interval 0
                // that tears; one that overhangs still covers the screen, taskbar and
                // all, and stays composed.
                _window.Size = bounds.Size + new Vector2D<int>(0, 1);
                _borderlessActive = true;
            }
        }
        else
        {
            _window.WindowState = on ? WindowState.Fullscreen : WindowState.Normal;
        }

        if (on) SetAutoIconify(false);
        ApplySwapInterval();
    }

    /// <summary>The whole of the monitor the window is on. Silk's <c>IMonitor.Bounds</c>
    /// is <c>glfwGetMonitorWorkarea</c>, the screen less the taskbar, and a window that
    /// size is not one the shell treats as covering the screen: measured 1920x1128 on a
    /// 1920x1200 monitor, under the taskbar. The monitor is found by that work area,
    /// then sized from its origin and its current mode.</summary>
    private static unsafe Rectangle<int>? MonitorRect()
    {
        if (_window?.Monitor?.Bounds is not { } work) return null;
        try
        {
            var glfw = Silk.NET.GLFW.Glfw.GetApi();
            var monitors = glfw.GetMonitors(out var count);
            for (var i = 0; i < count; i++)
            {
                glfw.GetMonitorWorkarea(monitors[i], out var wx, out var wy, out var ww, out var wh);
                if (wx != work.Origin.X || wy != work.Origin.Y || ww != work.Size.X || wh != work.Size.Y)
                    continue;
                var mode = glfw.GetVideoMode(monitors[i]);
                if (mode == null) break;
                glfw.GetMonitorPos(monitors[i], out var x, out var y);
                return new Rectangle<int>(x, y, mode->Width, mode->Height);
            }
        }
        catch (Exception e)
        {
            Console.WriteLine($"[Host] cant read the monitor's size: {e.Message}");
        }

        return work;
    }

    private static bool _borderlessActive;
    private static Vector2D<int> _windowedPosition = new(100, 100);
    private static Vector2D<int> _windowedSize = new(1280, 720);

    private static unsafe void SetAutoIconify(bool on)
    {
        try
        {
            var handle = _window?.Native?.Glfw;
            if (handle is not { } h) return;
            Silk.NET.GLFW.Glfw.GetApi().SetWindowAttrib(
                (Silk.NET.GLFW.WindowHandle*)h,
                Silk.NET.GLFW.WindowAttributeSetter.AutoIconify, on);
        }
        catch (Exception e)
        {
            Console.WriteLine($"[Host] auto-iconify unavailable: {e.Message}");
        }
    }

    public static bool IsKeyDown(Key k)
    {
        return InputManager.IsKeyDown(k);
    }

    // The pad, for a port that draws its own binding table. Both of these are
    // public on InputManager already and the class is internal, so unlike the
    // mouse block below nothing had to be added there -- these two forwards are
    // the whole of the reach.
    public static bool IsPadConnected(int pad) => InputManager.IsPadConnected(pad);

    /// <summary>The first pad button held down right now, or null. Face and
    /// shoulder buttons are their SDL index, the triggers are 100/101 and the
    /// stick directions 102-109 -- the encoding <c>GamepadBindings</c> stores
    /// and a binding table has to display, so a caller needs no second table to
    /// interpret the answer.</summary>
    public static int? GetFirstPressedPadButton(int pad = 0) => InputManager.GetFirstPressedPadButton(pad);

    /// <summary>One named pad button, in the same encoding, held down right now.
    /// <see cref="GetFirstPressedPadButton"/> answers only about the lowest
    /// index held, so a button high in the enum -- the DualSense mute key is
    /// SDL's Misc1, 15 -- is invisible to it while anything else is down.</summary>
    public static bool IsPadButtonDown(int button, int pad = 0) => InputManager.IsPadButtonDown(button, pad);

    // The mouse, for a port that wants to steer with it. InputManager owns the
    // IMouse and is internal, so these are the way out of the assembly -- the
    // same role IsKeyDown already plays for the keyboard.
    public static bool MouseAvailable => InputManager.MouseAvailable;

    /// <summary>Lock the pointer to the window and hide it, or give it back.
    /// Read it back after setting it: a platform that refuses the mode leaves
    /// this false.</summary>
    public static bool MouseCaptured
    {
        get => InputManager.MouseCaptured;
        set => InputManager.MouseCaptured = value;
    }

    /// <summary>Motion since the last call, in window pixels, and cleared by
    /// it.</summary>
    public static (float X, float Y) TakeMouseMotion() => InputManager.TakeMouseMotion();

    public static float TakeMouseWheel() => InputManager.TakeMouseWheel();

    public static bool IsMouseButtonDown(MouseButton button) => InputManager.IsMouseButtonDown(button);

    public static void RequestDiscPath()
    {
        PopupManager.Open<DiscPickerPopup>();
    }

    public static void WaitForValidDisc() // wait for disc path to be valid before running it!!
    {
        if (_headless || _window == null) return;

        while (StartupNoticePopup.NeedsAck)
            HandOverFrame();

        while (true)
        {
            var path = ConfigManager.Game.CdPath;
            if (!string.IsNullOrWhiteSpace(path) && File.Exists(path) && Runtime.ValidateDisc(path) == null)
                return;

            HandOverFrame();
        }
    }
    
    private static void HandOverFrame()
    {
        var closing = false;

        GpuJobs.Run(() =>
        {
            try
            {
                _window!.DoEvents();
            }
            catch
            {
            }

            if (_window!.IsClosing)
            {
                closing = true;
                return;
            }

            InputManager.Poll();
            RenderFrame();
        });

        if (!closing) return;

        Runtime.Shutdown();
        Environment.Exit(0);
    }

    private static void OnLoad()
    {
        var input = _window!.CreateInput();
        InputManager.Initialize(input);

        if (_pendingIcons is { } icons) Apply(icons);

        _gl = GL.GetApi(_window);
        GlDebug.Install(_gl);
        _gl.ClearColor(0.08f, 0.08f, 0.08f, 1f);

        var fb = _window!.FramebufferSize;
        _gl.Viewport(0, 0, (uint)fb.X, (uint)fb.Y);
        _window.FramebufferResize += size => _gl?.Viewport(0, 0, (uint)size.X, (uint)size.Y);
        _displayTex = CreateTexture(_gl);
        _vramTex = CreateTexture(_gl);
        _ramTex = CreateTexture(_gl);

        Hle.GlVram.Scale = ConfigManager.View.RenderScale;
        _glBackend = (Hle.GlCore)Hle.GpuBackendFactory.Create(_gl,
            Hle.GpuBackendFactory.Parse(ConfigManager.View.GpuBackend));
        _glBackend.InitGl();
        Hle.GpuHle.Active = _glBackend.Ready;
        // Upstream wraps the GL backend in its frame-interpolation backend, which
        // records every primitive into a FrameGraph and issues nothing until
        // Runtime.PresentLoop calls Compose. This port never enters that loop --
        // Program.cs calls Entry.Run directly and presents from inside the game's
        // own VSync -- so the wrapper swallowed the whole frame and the window drew
        // black. Frame interpolation is deliberately not carried here (see the
        // checkout section in CLAUDE.md), so the GL backend is used unwrapped and
        // Interp.Backend stays null.
        Hle.GpuHle.Backend = _glBackend;
        ApplySwapInterval();

        _imgui = new ImGuiController(_gl, _window, input, null, ConfigureImGui);

        PanelManager.Register(new OutputPanel());
        PanelManager.Register(new VramViewerPanel());
        PanelManager.Register(new TextureInspectorPanel());
        PanelManager.Register(new CpuStatePanel());
        PanelManager.Register(new RamMapPanel());
        PanelManager.Register(new MemoryEditorPanel());
        PanelManager.Register(new SpuViewerPanel());
        PanelManager.Register(new CdDebugPanel());
        PanelManager.Register(new ConsolePanel());
        PanelManager.Register(new OverlayEventsPanel());

        PopupManager.Register(new SettingsPopup());
        PopupManager.Register(new ModsPopup());
        PopupManager.Register(new ModLoadingPopup());
        PopupManager.Register(new NoticePopup());
        PopupManager.Register(new StartupNoticePopup());
        PopupManager.Register(new DiscPickerPopup());

        MainMenuBar.RegisterBuiltins();

        SettingsRegistry.Register(new InterfaceSettingsSection());
        SettingsRegistry.Register(new InputSettingsSection());
        SettingsRegistry.Register(new DisplaySettingsSection());
        SettingsRegistry.Register(new PathsSettingsSection());
        SettingsRegistry.Register(new AudioSettingsSection());

        ConfigManager.ApplyViewToPanels(PanelManager.Panels);

        var cdPath = ConfigManager.Game.CdPath;
        if (string.IsNullOrWhiteSpace(cdPath) || !File.Exists(cdPath) || Runtime.ValidateDisc(cdPath) != null)
            PopupManager.Open<DiscPickerPopup>();
    }

    private static void ConfigureImGui()
    {
        _dpiScale = QueryDpiScale();
        Console.WriteLine($"[Host] display scale: {_dpiScale:0.##}x");

        var io = ImGui.GetIO();
        io.ConfigFlags |= ImGuiConfigFlags.DockingEnable;
        io.ConfigWindowsMoveFromTitleBarOnly = true;
        io.FontGlobalScale = ConfigManager.View.UiScale;
        unsafe
        {
            io.NativePtr->IniFilename = null;
        }

        FontSet.Load(16f * _dpiScale);
        Localization.Load();
        Theme.Load();

        if (ConfigManager.ApplyImGuiLayout())
            _layoutPending = false;

        if (ConfigManager.View.Fullscreen) SetAutoIconify(false);
    }

    public static void SetVSync(bool on)
    {
        Interp.Interp.VSync = on;
        ApplySwapInterval();
        FrameClock.Resync();
    }
    
    public static void AdvanceFrame()
    {
        _glBackend?.AdvanceFrame();
    }
    
    public static void RefreshVSync()
    {
        ApplySwapInterval();
    }
    
    private static void ApplySwapInterval()
    {
        if (_headless || _window == null) return;
        
        var refresh = QueryRefreshRate();
        if (refresh > 0 && refresh != Interp.Interp.RefreshRate)
        {
            Interp.Interp.RefreshRate = refresh;
            Console.WriteLine($"[Host] monitor refresh: {refresh} hz");
        }
        
        // 0064. Silk's own VSync set interval 1 over the -1 (adaptive) asked for
        // here, so 1 is what ran, and it stays 1 off Wayland. On Wayland a swap on
        // a hidden window waits until it is shown, and the game presents from
        // inside its own VSync, so minimising stopped everything. The compositor
        // never tears, so the swap stays at 0 there and the refresh is held on the
        // CPU instead.
        var vsync = Interp.Interp.VSync;
        var wayland = IsWayland();
        var interval = vsync && !wayland ? 1 : 0;
        _cpuVSync = vsync && wayland;
        FrameClock.VSync = vsync && !wayland;

        // 0066. The display's own vblank on Windows, the deferred interval elsewhere.
        var mode = SwapMode.Silk;
        if (interval == 1)
        {
            // Composed: windowed or borderless, where the compositor presents the
            // window and never tears. GLFW's fullscreen bypasses it.
            var composed = _window.WindowState != WindowState.Fullscreen;
            var vblank = SwapAsked == "vblank" || (composed && SwapAsked is not ("interval" or "immediate"));
            if (vblank && !_vblankFailed && OperatingSystem.IsWindows() && OpenVBlank())
            {
                mode = SwapMode.VBlank;
                interval = 0;
            }
            else if (SwapAsked != "immediate")
            {
                mode = SwapMode.Deferred;
            }
        }

        if (mode != SwapMode.VBlank) VBlankWait.Close();

        // Silk applies its own VSync lazily, inside the first DoRender after the
        // property is set, so a Silk left at the options' false wrote interval 0
        // over this one before the first frame: VSync on read back as
        // wglGetSwapIntervalEXT 0 on Windows, and tore, until the setting was
        // toggled in play. Silk is told the same thing, so it re-applies ours.
        _window.VSync = interval != 0;
        SetSwapInterval(interval);
        SetSwapMode(mode);

        Console.WriteLine($"[Host] swap interval: {interval}" +
                          (_cpuVSync ? $" (Wayland: vsync held on the CPU at {RefreshHz()} hz)" : "") +
                          (mode == SwapMode.Deferred ? " (swap deferred to the next present)" : "") +
                          (mode == SwapMode.VBlank ? " (swap after the display's own vblank)" : ""));
    }

    /// <summary>The window moved, perhaps to another monitor: wait for that one's
    /// vblank from now on.</summary>
    private static void OnMove(Vector2D<int> _)
    {
        if (_swapMode == SwapMode.VBlank && !OpenVBlank()) ApplySwapInterval();
    }

    private static bool _cpuVSync;

    private static int RefreshHz() => Interp.Interp.RefreshRate > 0 ? Interp.Interp.RefreshRate : 60;

    private static void SetSwapInterval(int interval)
    {
        try
        {
            Silk.NET.GLFW.Glfw.GetApi().SwapInterval(interval);
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[Host] swap interval {interval}: {e.Message}");
        }
    }

    // GLFW 3.4's glfwGetPlatform, which Silk 2.22 does not bind.
    private const int GlfwPlatformWayland = 0x00060003;

    private static unsafe bool IsWayland()
    {
        try
        {
            var glfw = Silk.NET.GLFW.Glfw.GetApi();
            if (!glfw.Context.TryGetProcAddress("glfwGetPlatform", out var fn) || fn == 0) return false;
            return ((delegate* unmanaged<int>)fn)() == GlfwPlatformWayland;
        }
        catch
        {
            return false;
        }
    }
    
    private static unsafe int QueryRefreshRate()
    {
        try
        {
            var glfw = Silk.NET.GLFW.Glfw.GetApi();
            var monitor = glfw.GetPrimaryMonitor();
            if (monitor == null) return 0;
            
            var mode = glfw.GetVideoMode(monitor);
            if (mode == null) return 0;
            
            var rate = mode->RefreshRate;
            return rate is > 0 and <= 1000 ? rate : 0;
        }
        catch (Exception e)
        {
            Console.WriteLine($"[Host] cant read refresh rate: {e.Message}");
            return 0;
        }
    }

    private static Action<Vector2D<int>>? _imguiResized;
    private static bool _imguiResizedMissing;

    // 0080. Silk's ImGuiController learns the window's size only from the Resize
    // event, and GLFW on Wayland raises no window-size callback on leaving
    // fullscreen (only the framebuffer one), so the interface stayed laid out
    // for the fullscreen size with its top -- the menu bar -- off the window.
    // io.DisplaySize still holds what the controller last applied.
    private static void SyncImGuiSize()
    {
        var size = _window!.Size;
        if (size.X <= 0 || size.Y <= 0 || _imguiResizedMissing) return;
        var io = ImGui.GetIO().DisplaySize;
        if (io.X <= 0 || ((int)io.X == size.X && (int)io.Y == size.Y)) return;

        _imguiResized ??= typeof(ImGuiController)
            .GetMethod("WindowResized", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)
            ?.CreateDelegate<Action<Vector2D<int>>>(_imgui);
        if (_imguiResized == null)
        {
            _imguiResizedMissing = true;
            Console.WriteLine("[Host] ImGuiController has no WindowResized; a missed resize cannot be recovered");
            return;
        }

        _imguiResized(size);
    }

    private static void OnRender(double dt)
    {
        var gl = _gl!;
        var update = Profiler.Begin(Profiler.ImGuiUpdate);
        SyncImGuiSize();
        _imgui!.Update((float)dt);
        Profiler.End(update);

        // 0018. Silk's ImGuiController computes io.DisplayFramebufferScale as
        // FramebufferSize / window size with both sides int, so the ratio
        // truncates before it is ever a float. On a display the compositor runs
        // at a fractional scale -- KDE's 1.15, say -- the framebuffer is 1.15x
        // the logical window and 1.15 becomes 1. RenderImDrawData then sizes
        // both its GL viewport and every scissor rect from DisplaySize *
        // FramebufferScale, so the whole interface is drawn into a logical-sized
        // box in the bottom-left of a larger framebuffer: dead margins along the
        // top and right, and the panels clipped where they cross them. An
        // integer scale divides exactly, which is why this is invisible on a
        // 1:1 monitor and only appears on the fractionally scaled one.
        //
        // Update() has already run NewFrame, so layout for this frame is fixed
        // and still in logical units -- input is untouched, and this only
        // restores the mapping onto the framebuffer. Render() reads
        // io.DisplayFramebufferScale when it fills the draw data, so setting it
        // anywhere between the two is what the backend sees.
        var wsz = _window!.Size;
        var wfb = _window.FramebufferSize;
        if (wsz.X > 0 && wsz.Y > 0)
            ImGui.GetIO().DisplayFramebufferScale =
                new Vector2((float)wfb.X / wsz.X, (float)wfb.Y / wsz.Y);

        gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
        var fbDef = _window!.FramebufferSize;
        gl.Viewport(0, 0, (uint)fbDef.X, (uint)fbDef.Y);
        var clear = Theme.Background;
        gl.ClearColor(clear.X, clear.Y, clear.Z, 1f);
        gl.Clear(ClearBufferMask.ColorBufferBit);

        Runtime.RamLog.Tick();
        Memory.RamLogger.TrackReads =
            PanelManager.Get<RamMapPanel>()?.IsOpen == true ||
            PanelManager.Get<MemoryEditorPanel>()?.IsOpen == true;
        Memory.RamLogger.TrackWrites = Memory.RamLogger.TrackReads;

        var gpu = _gpu;
        if (gpu != null)
        {
            //Beside the picture's rectangle, the size of the picture in the
            //game's own pixels -- the two together are what turns a window pixel
            //back into a game one. Taken here rather than in SetTexture because
            //the GL backend passes that a render-scaled target.
            (OutputView.GameW, OutputView.GameH) = (gpu.DisplayWidth, gpu.DisplayHeight);

            if (Hle.GpuHle.Active && _glBackend is { Ready: true } && gpu.DisplayEnabled)
            {
                var wf = _window!.FramebufferSize;
                var display = Profiler.Begin(Profiler.Display);
                var (tex, tw, th, aspect) = _glBackend.PresentDisplay(
                    gpu.DisplayX, gpu.DisplayY,
                    gpu.DisplayWidth, gpu.DisplayHeight,
                    gpu.Display24Bit,
                    wf.X, wf.Y);
                Profiler.End(display);
                if (tex != 0) OutputPanel.SetTexture(tex, tw, th, aspect);
                gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
                gl.Viewport(0, 0, (uint)wf.X, (uint)wf.Y);
            }
            else
            {
                UploadDisplayTexture(gl, gpu);
            }

            if (PanelManager.Get<VramViewerPanel>()?.IsOpen == true)
                UploadVramTexture(gl, gpu);
        }

        if (PanelManager.Get<RamMapPanel>()?.IsOpen == true)
        {
            QueueRamConvert();
            if (_ramReady) FlushRamTexture(gl);
        }

        var panels = Profiler.Begin(Profiler.Panels);
        if (!ConfigManager.View.HideTopBar)
            MainMenuBar.Draw();

        DrawDockspace();
        PanelManager.DrawPanels();
        PopupManager.Draw();
        Profiler.End(panels);
        gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
        gl.Viewport(0, 0, (uint)fbDef.X, (uint)fbDef.Y);
        var imgui = Profiler.Begin(Profiler.ImGuiRender);
        _imgui.Render();
        Profiler.End(imgui);
    }

    private static void DrawDockspace()
    {
        var viewport = ImGui.GetMainViewport();
        ImGui.SetNextWindowPos(viewport.WorkPos);
        ImGui.SetNextWindowSize(viewport.WorkSize);
        ImGui.SetNextWindowViewport(viewport.ID);

        const ImGuiWindowFlags hostFlags = ImGuiWindowFlags.NoDocking |
                                           ImGuiWindowFlags.NoTitleBar |
                                           ImGuiWindowFlags.NoCollapse |
                                           ImGuiWindowFlags.NoResize |
                                           ImGuiWindowFlags.NoMove |
                                           ImGuiWindowFlags.NoBringToFrontOnFocus |
                                           ImGuiWindowFlags.NoBackground;

        ImGui.PushStyleVar(ImGuiStyleVar.WindowRounding, 0f);
        ImGui.PushStyleVar(ImGuiStyleVar.WindowBorderSize, 0f);
        ImGui.PushStyleVar(ImGuiStyleVar.WindowPadding, Vector2.Zero);
        ImGui.Begin("##DockHost", hostFlags);
        ImGui.PopStyleVar(3);
        var dockId = ImGui.GetID("##MainDock");
        var openCount = PanelManager.Panels.Count(p => p.IsOpen && p is not IFloatingPanel);
        var dockFlags = openCount <= 1 ? (ImGuiDockNodeFlags)4096 : ImGuiDockNodeFlags.None;
        ImGui.DockSpace(dockId, Vector2.Zero, dockFlags);

        if (openCount <= 1 && !OutputPanel.IsDocked && _redockCooldown == 0)
            _layoutPending = true;

        if (_redockCooldown > 0) _redockCooldown--;

        if (_layoutPending)
        {
            _layoutPending = false;
            _redockCooldown = RedockCooldownFrames;
            if (PanelManager.Get<OutputPanel>() is { } output)
                DockBuilder.SetupCenterLayout(dockId, viewport.WorkSize, output.Title());
        }

        ImGui.End();
    }

    private static void OnClosing()
    {
        if (_closed) return;
        _closed = true;
        ConfigManager.SaveView(PanelManager.Panels);
        ConfigManager.SaveGame();
        PanelManager.Shutdown();
        PopupManager.Shutdown();
        _glBackend?.Dispose();
        _imgui?.Dispose();
        _gl?.DeleteTexture(_displayTex);
        _gl?.DeleteTexture(_vramTex);
        _gl?.DeleteTexture(_ramTex);
    }

    public static uint UploadPng(byte[] png)
    {
        try
        {
            var img = StbImageSharp.ImageResult.FromMemory(png, StbImageSharp.ColorComponents.RedGreenBlueAlpha);
            if (img == null || img.Width <= 0 || img.Height <= 0) return 0;

            int w = img.Width, h = img.Height;
            if (w == h) return UploadTexture(img.Data, w, h);

            var s = Math.Min(w, h);
            var ox = (w - s) / 2;
            var oy = (h - s) / 2;
            var square = new byte[s * s * 4];
            for (var y = 0; y < s; y++)
                Array.Copy(img.Data, ((oy + y) * w + ox) * 4, square, y * s * 4, s * 4);
            return UploadTexture(square, s, s);
        }
        catch
        {
            return 0;
        }
    }

    public static uint UploadTexture(byte[] rgba, int width, int height)
    {
        if (_gl == null || width <= 0 || height <= 0) return 0;
        var needed = width * height * 4;
        if (rgba.Length < needed) return 0;
        var tex = CreateTexture(_gl);
        _gl.BindTexture(TextureTarget.Texture2D, tex);
        _gl.TexImage2D<byte>(TextureTarget.Texture2D, 0, InternalFormat.Rgba,
            (uint)width, (uint)height, 0, PixelFormat.Rgba, PixelType.UnsignedByte, rgba.AsSpan(0, needed));
        return tex;
    }

    private static uint CreateTexture(GL gl)
    {
        var tex = gl.GenTexture();
        gl.BindTexture(TextureTarget.Texture2D, tex);
        gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Nearest);
        gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Nearest);
        gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
        gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
        return tex;
    }

    private static void UploadDisplayTexture(GL gl, Gpu gpu)
    {
        int w = gpu.DisplayWidth, h = gpu.DisplayHeight;
        if (!gpu.DisplayEnabled || w <= 0 || h <= 0) return;
        var needed = w * h * 3;
        if (_rgbDisplay.Length < needed) _rgbDisplay = new byte[needed];
        ConvertDisplay(gpu, w, h);
        gl.BindTexture(TextureTarget.Texture2D, _displayTex);
        gl.TexImage2D<byte>(TextureTarget.Texture2D, 0, InternalFormat.Rgb, (uint)w, (uint)h, 0,
            PixelFormat.Rgb, PixelType.UnsignedByte, _rgbDisplay.AsSpan(0, needed));
        OutputPanel.SetTexture(_displayTex, w, h);
    }

    private static ushort[] _vramView = new ushort[Gpu.VramWidth * Gpu.VramHeight];

    private static void UploadVramTexture(GL gl, Gpu gpu)
    {
        const int sz = Gpu.VramWidth * Gpu.VramHeight * 3;
        if (_rgbVram.Length < sz) _rgbVram = new byte[sz];
        ushort[] src;
        if (Hle.GpuHle.Active && _glBackend is { Ready: true })
        {
            _glBackend.ReadVram(0, 0, Gpu.VramWidth, Gpu.VramHeight, _vramView);
            src = _vramView;
        }
        else
        {
            src = gpu.Vram;
        }

        ConvertVramToBuffer(src, _rgbVram);
        gl.BindTexture(TextureTarget.Texture2D, _vramTex);
        gl.TexImage2D<byte>(TextureTarget.Texture2D, 0, InternalFormat.Rgb, Gpu.VramWidth, Gpu.VramHeight, 0,
            PixelFormat.Rgb, PixelType.UnsignedByte, _rgbVram.AsSpan(0, sz));
        VramViewerPanel.SetTexture(_vramTex, Gpu.VramWidth, Gpu.VramHeight);
    }

    private static void QueueRamConvert()
    {
        if (_ramTask is { IsCompleted: false }) return;
        if (++_ramFrame < 6) return;
        _ramFrame = 0;
        var psMem = Runtime.Mem as Memory.PSMemory;
        if (psMem == null) return;
        var ram = psMem.RamBuffer;
        var back = _ramBack;
        _ramTask = Task.Run(() => Runtime.RamLog.BuildTexture(ram, back))
            .ContinueWith(_ =>
            {
                (_ramFront, _ramBack) = (_ramBack, _ramFront);
                _ramReady = true;
            }, TaskContinuationOptions.ExecuteSynchronously);
    }

    private static void FlushRamTexture(GL gl)
    {
        _ramReady = false;
        gl.BindTexture(TextureTarget.Texture2D, _ramTex);
        gl.TexImage2D<byte>(TextureTarget.Texture2D, 0, InternalFormat.Rgba,
            Memory.RamLogger.Width, Memory.RamLogger.Height, 0,
            PixelFormat.Rgba, PixelType.UnsignedByte, _ramFront);
        RamMapPanel.SetTexture(_ramTex);
    }

    private static void ConvertDisplay(Gpu gpu, int w, int h)
    {
        var vram = gpu.Vram;
        int dx = gpu.DisplayX, dy = gpu.DisplayY;
        var o = 0;
        if (gpu.Display24Bit)
            for (var y = 0; y < h; y++)
            {
                var lineByte = ((dy + y) * Gpu.VramWidth + dx) * 2;
                for (var x = 0; x < w; x++)
                {
                    var bo = lineByte + x * 3;
                    _rgbDisplay[o++] = VramByte(vram, bo);
                    _rgbDisplay[o++] = VramByte(vram, bo + 1);
                    _rgbDisplay[o++] = VramByte(vram, bo + 2);
                }
            }
        else
            for (var y = 0; y < h; y++)
            {
                var line = ((dy + y) & (Gpu.VramHeight - 1)) * Gpu.VramWidth;
                for (var x = 0; x < w; x++)
                {
                    var px = vram[line + ((dx + x) & (Gpu.VramWidth - 1))];
                    _rgbDisplay[o++] = (byte)((px & 0x1F) << 3);
                    _rgbDisplay[o++] = (byte)(((px >> 5) & 0x1F) << 3);
                    _rgbDisplay[o++] = (byte)(((px >> 10) & 0x1F) << 3);
                }
            }
    }

    private static void ConvertVramToBuffer(ushort[] vram, byte[] output)
    {
        var o = 0;
        for (var y = 0; y < Gpu.VramHeight; y++)
        for (var x = 0; x < Gpu.VramWidth; x++)
        {
            var px = vram[y * Gpu.VramWidth + x];
            output[o++] = (byte)((px & 0x1F) << 3);
            output[o++] = (byte)(((px >> 5) & 0x1F) << 3);
            output[o++] = (byte)(((px >> 10) & 0x1F) << 3);
        }
    }

    private static byte VramByte(ushort[] vram, int byteOffset)
    {
        var hw = (byteOffset >> 1) & (Gpu.VramWidth * Gpu.VramHeight - 1);
        var v = vram[hw];
        return (byte)((byteOffset & 1) == 0 ? v & 0xFF : v >> 8);
    }
}