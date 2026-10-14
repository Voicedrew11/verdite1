using System.Numerics;
using Silk.NET.Input;
using Silk.NET.SDL;
using RecompOne.Runtime.Config;
using RecompOne.Runtime.Hardware;
using EventBus = RecompOne.Runtime.Events.Event;
using KeyboardEvent = RecompOne.Runtime.Events.KeyboardEvent;
using MouseEvent = RecompOne.Runtime.Events.MouseEvent;
using ControllerEvent = RecompOne.Runtime.Events.ControllerEvent;
using MouseAction = RecompOne.Runtime.Events.MouseAction;
using EvMouseButton = RecompOne.Runtime.Events.MouseButton;

namespace RecompOne.Runtime.Host;

internal static unsafe class InputManager
{
    private static IKeyboard? _keyboard;
    private static IMouse? _mouse;
    private static Sdl? _sdl;
    private static GameController* _pad0;
    private static GameController* _pad1;

    // The handle whose gyroscope this file last switched, and whether it is on.
    // A handle comes back from GameControllerOpen with its sensors off, so a
    // rescan forgets both (0102).
    private static GameController* _gyroPad;
    private static bool _gyroOn;

    // The port's rumble (0104). Poll runs on more than one thread, so the motors
    // are driven under one lock. _hd is the Switch pad's own HID handle while HD
    // rumble is on, opened beside SDL's (_hdTried: asked once per pad).
    private static readonly object _rumbleLock = new();
    private static GameController* _rumblePad;
    private static HidDevice* _hd;
    private static bool _hdTried;
    private static byte _hdCount;
    private static Controller.RumbleWave? _rumbleSent;
    private static long _rumbleSentMs;

    /// <summary>A standing wave is sent again this often: a Switch pad lets a
    /// rumble go after a while, and SDL's two-motor rumble is asked for in spans.</summary>
    private const long RumbleRefreshMs = 30;

    private const int AxisThreshold = 8000;
    private const int StickThreshold = 16000;
    private const int LeftTrigger = 100;
    private const int RightTrigger = 101;
    private const int LeftStickLeft = 102;
    private const int LeftStickRight = 103;
    private const int LeftStickUp = 104;
    private const int LeftStickDown = 105;
    private const int RightStickLeft = 106;
    private const int RightStickRight = 107;
    private const int RightStickUp = 108;
    private const int RightStickDown = 109;
    private static bool _topBarToggle;
    private static bool _fullscreenToggle;


    public static bool ConsumeTopBarToggle()
    {
        var v = _topBarToggle;
        _topBarToggle = false;
        return v;
    }

    public static bool ConsumeFullscreenToggle()
    {
        var v = _fullscreenToggle;
        _fullscreenToggle = false;
        return v;
    }

    public static void Initialize(IInputContext input)
    {
        if (input.Keyboards.Count > 0)
        {
            _keyboard = input.Keyboards[0];
            _keyboard.KeyDown += OnKeyDown;
            _keyboard.KeyUp += OnKeyUp;
        }

        if (input.Mice.Count > 0)
        {
            _mouse = input.Mice[0];
            _mouse.MouseMove += OnMouseMove;
            _mouse.MouseDown += OnMouseDown;
            _mouse.MouseUp += OnMouseUp;
            _mouse.Scroll += OnScroll;
        }


        try
        {
            _sdl = Sdl.GetApi();
            _sdl.SetHint("SDL_JOYSTICK_RAWINPUT", "0");
            _sdl.InitSubSystem(Sdl.InitGamecontroller);
            Rescan();
        }
        catch
        {
            _sdl = null;
        }
    }

    public static bool IsConnected => _pad0 != null;

    public static bool IsPadConnected(int pad)
    {
        return pad == 0 ? _pad0 != null : _pad1 != null;
    }

    public static bool IsKeyDown(Key k)
    {
        return _keyboard?.IsKeyPressed(k) ?? false;
    }

    // ---- the mouse, as a motion source rather than a pointer -----------------
    //
    // A pointer runs out of desktop halfway through a turn, so a game played with
    // the mouse needs the lock: GLFW's disabled cursor reports an unbounded
    // virtual position, and the difference between two of them is the motion
    // itself. Raw mode is the same thing with the desktop's pointer acceleration
    // taken out, which is what a look axis wants, so it is preferred where the
    // platform has it.
    //
    // The accumulator runs whether or not anything has asked for capture -- it is
    // one subtraction per callback -- and is cleared on the mode change, because
    // the pointer teleports when the cursor is locked or let go and that jump is
    // not motion anyone asked for.
    static Vector2 _mousePos;
    static bool _mouseSeen;
    static float _mouseDx, _mouseDy;
    static float _mouseWheel;
    static bool _mouseCaptured;

    public static bool MouseAvailable => _mouse != null;

    /// <summary>
    /// Lock the pointer to the window and hide it, or give it back. Silently
    /// stays off if there is no mouse or the platform refuses the mode, so a
    /// caller can ask and then read this back to see whether it happened.
    /// </summary>
    public static bool MouseCaptured
    {
        get => _mouseCaptured;
        set
        {
            if (_mouse == null || value == _mouseCaptured) return;

            var cursor = _mouse.Cursor;
            var mode = value
                ? (cursor.IsSupported(CursorMode.Raw) ? CursorMode.Raw : CursorMode.Disabled)
                : CursorMode.Normal;

            try { cursor.CursorMode = mode; }
            catch (Exception e)
            {
                Console.Error.WriteLine($"[Host] mouse capture unavailable: {e.Message}");
                return;
            }

            _mouseCaptured = value;
            _mouseSeen = false;
            _mouseDx = _mouseDy = 0f;
        }
    }

    /// <summary>Motion since the last call, in window pixels, and cleared by
    /// it.</summary>
    public static (float X, float Y) TakeMouseMotion()
    {
        var motion = (_mouseDx, _mouseDy);
        _mouseDx = _mouseDy = 0f;
        return motion;
    }

    /// <summary>Notches scrolled since the last call, positive away from the
    /// user, and cleared by it. Drained rather than polled for the reason
    /// <see cref="TakeMouseMotion"/> is: the host produces these as discrete
    /// events, and a caller reading a level would miss a notch that arrived
    /// between two reads or spend the same one twice.
    ///
    /// A **float** because a wheel is not the only device that produces one: a
    /// discrete wheel steps it by exactly +-1, but a trackpad's two-finger
    /// scroll arrives in fractions of a notch, and rounding those away makes a
    /// whole class of pointing device report nothing at all.</summary>
    public static float TakeMouseWheel()
    {
        var wheel = _mouseWheel;
        _mouseWheel = 0f;
        return wheel;
    }

    public static bool IsMouseButtonDown(MouseButton button) =>
        _mouse != null && _mouse.IsButtonPressed(button);

    public static void Poll()
    {
        Controller.Analog = ConfigManager.Game.PadKind == PadKind.Analog;
        Controller.Analog2 = ConfigManager.Game.PadKind2 == PadKind.Analog;


        PollGamepadEvents();
        PollKeyboard();
        PollGamepads();
        PollGyro();
        PollRumble();
        Controller.Connected2 = _pad1 != null || HasAnyKey(ConfigManager.Game.Keys2);
    }

    public static int? GetFirstPressedPadButton(int pad = 0)
    {
        var ctrl = pad == 0 ? _pad0 : _pad1;
        if (_sdl == null || ctrl == null) return null;
        for (var b = 0; b < (int)GameControllerButton.Max; b++)
            if (_sdl.GameControllerGetButton(ctrl, (GameControllerButton)b) != 0)
                return b;
        if (Pressed(ctrl, LeftTrigger)) return LeftTrigger;
        if (Pressed(ctrl, RightTrigger)) return RightTrigger;
        for (var b = LeftStickLeft; b <= RightStickDown; b++)
            if (Pressed(ctrl, b))
                return b;
        return null;
    }

    /// <summary>One binding held down right now, in the same encoding
    /// <see cref="GetFirstPressedPadButton"/> returns -- so triggers and stick
    /// directions answer here too.</summary>
    public static bool IsPadButtonDown(int button, int pad = 0)
    {
        var ctrl = pad == 0 ? _pad0 : _pad1;
        return _sdl != null && ctrl != null && Pressed(ctrl, button);
    }

    private static bool IsStickBinding(int b)
    {
        return b is >= LeftStickLeft and <= RightStickDown;
    }

    private static (GameControllerAxis Axis, bool Positive) AxisBinding(int b)
    {
        return b switch
        {
            LeftStickLeft => (GameControllerAxis.Leftx, false),
            LeftStickRight => (GameControllerAxis.Leftx, true),
            LeftStickUp => (GameControllerAxis.Lefty, false),
            LeftStickDown => (GameControllerAxis.Lefty, true),
            RightStickLeft => (GameControllerAxis.Rightx, false),
            RightStickRight => (GameControllerAxis.Rightx, true),
            RightStickUp => (GameControllerAxis.Righty, false),
            _ => (GameControllerAxis.Righty, true)
        };
    }

    public static void Shutdown()
    {
        MouseCaptured = false;
        CloseControllers();
        PadHaptics.Shutdown(_sdl);
        _sdl?.QuitSubSystem(Sdl.InitGamecontroller);
        _sdl?.Dispose();
        _sdl = null;
    }

    private static void PollGamepadEvents()
    {
        if (_sdl == null) return;
        Event ev;
        var changed = false;
        var anyCtrl = EventBus.HasAnyListeners<ControllerEvent>();
        while (_sdl.PollEvent(&ev) != 0)
        {
            if (ev.Type == (uint)EventType.Controllerdeviceadded) changed = true;
            if (ev.Type == (uint)EventType.Controllerdeviceremoved) changed = true;
            if (!anyCtrl) continue;
            if (ev.Type == (uint)EventType.Controllerbuttondown || ev.Type == (uint)EventType.Controllerbuttonup)
                EventBus.Dispatch(new ControllerEvent
                {
                    Device = ev.Cbutton.Which,
                    Button = ev.Cbutton.Button,
                    Pressed = ev.Type == (uint)EventType.Controllerbuttondown
                });
            else if (ev.Type == (uint)EventType.Controlleraxismotion)
                EventBus.Dispatch(new ControllerEvent
                {
                    Device = ev.Caxis.Which,
                    Axis = ev.Caxis.Axis,
                    Value = ev.Caxis.Value / 32768f
                });
        }

        if (changed) Rescan();
    }

    private static void CloseControllers()
    {
        _gyroPad = null;
        _gyroOn = false;
        lock (_rumbleLock) ForgetRumble();

        if (_pad0 != null)
        {
            _sdl?.GameControllerClose(_pad0);
            _pad0 = null;
        }

        if (_pad1 != null)
        {
            _sdl?.GameControllerClose(_pad1);
            _pad1 = null;
        }
    }

    public readonly record struct PadDevice(string Id, string Name);

    private static readonly List<PadDevice> _devices = [];

    public static IReadOnlyList<PadDevice> Devices
    {
        get
        {
            lock (_devices)
            {
                return _devices.ToArray();
            }
        }
    }

    public static void RefreshDevices()
    {
        Rescan();
    }

    private static string DeviceId(int joystickIndex)
    {
        if (_sdl == null) return "";
        var guid = _sdl.JoystickGetDeviceGUID(joystickIndex);
        var text = new byte[33];
        fixed (byte* p = text)
        {
            _sdl.JoystickGetGUIDString(guid, p, text.Length);
        }

        var len = Array.IndexOf(text, (byte)0);
        return System.Text.Encoding.ASCII.GetString(text, 0, len < 0 ? text.Length : len);
    }

    private static string DeviceName(int joystickIndex)
    {
        if (_sdl == null) return "";
        var name = _sdl.GameControllerNameForIndexS(joystickIndex);
        return string.IsNullOrWhiteSpace(name) ? $"Controller {joystickIndex}" : name;
    }

    private static void Rescan()
    {
        if (_sdl == null) return;
        CloseControllers();

        var found = new List<(int Index, string Id, string Name)>();
        var n = _sdl.NumJoysticks();
        for (var i = 0; i < n; i++)
        {
            if (_sdl.IsGameController(i) != SdlBool.True) continue;
            found.Add((i, DeviceId(i), DeviceName(i)));
        }

        lock (_devices)
        {
            _devices.Clear();
            foreach (var f in found) _devices.Add(new PadDevice(f.Id, f.Name));
        }

        var used = new HashSet<int>();
        _pad0 = OpenFor(found, ConfigManager.Game.PadDevice, used);
        _pad1 = OpenFor(found, ConfigManager.Game.PadDevice2, used);
    }

    private static GameController* OpenFor(List<(int Index, string Id, string Name)> found, string wanted,
        HashSet<int> used)
    {
        if (_sdl == null) return null;

        var pick = -1;
        if (!string.IsNullOrEmpty(wanted))
        {
            foreach (var f in found)
                if (f.Id == wanted && used.Add(f.Index))
                {
                    pick = f.Index;
                    break;
                }

            if (pick < 0) return null;
        }
        else
        {
            foreach (var f in found)
                if (used.Add(f.Index))
                {
                    pick = f.Index;
                    break;
                }

            if (pick < 0) return null;
        }

        var ctrl = _sdl.GameControllerOpen(pick);
        if (ctrl == null) used.Remove(pick);
        return ctrl;
    }

    private static void PollKeyboard()
    {
        var kb = _keyboard;
        if (kb == null)
        {
            Controller.State = 0xFFFF;
            Controller.State2 = 0xFFFF;
            return;
        }

        Controller.State = KeyState(kb, ConfigManager.Game.Keys);
        Controller.State2 = KeyState(kb, ConfigManager.Game.Keys2);
    }

    private static ushort KeyState(IKeyboard kb, KeyBindings cfg)
    {
        ushort s = 0xFFFF;

        void B(string keyName, ushort bit)
        {
            if (Enum.TryParse<Key>(keyName, out var k) && kb.IsKeyPressed(k))
                s &= (ushort)~bit;
        }

        B(cfg.Cross, Controller.Cross);
        B(cfg.Circle, Controller.Circle);
        B(cfg.Square, Controller.Square);
        B(cfg.Triangle, Controller.Triangle);
        B(cfg.L1, Controller.L1);
        B(cfg.R1, Controller.R1);
        B(cfg.L2, Controller.L2);
        B(cfg.R2, Controller.R2);
        B(cfg.L3, Controller.L3);
        B(cfg.R3, Controller.R3);
        B(cfg.Start, Controller.Start);
        B(cfg.Select, Controller.Select);
        B(cfg.Up, Controller.Up);
        B(cfg.Down, Controller.Down);
        B(cfg.Left, Controller.Left);
        B(cfg.Right, Controller.Right);

        return s;
    }

    private static bool HasAnyKey(KeyBindings cfg)
    {
        return cfg.Cross.Length > 0 || cfg.Circle.Length > 0 || cfg.Square.Length > 0 || cfg.Triangle.Length > 0 ||
               cfg.L1.Length > 0 || cfg.R1.Length > 0 || cfg.L2.Length > 0 || cfg.R2.Length > 0 ||
               cfg.L3.Length > 0 || cfg.R3.Length > 0 || cfg.Start.Length > 0 || cfg.Select.Length > 0 ||
               cfg.Up.Length > 0 || cfg.Down.Length > 0 || cfg.Left.Length > 0 || cfg.Right.Length > 0;
    }

    private static void PollGamepads()
    {
        if (_sdl == null) return;

        if (_pad0 != null)
        {
            var bind = ConfigManager.Game.PadFor(0);
            Controller.State = PadState(_pad0, bind, Controller.State);
            Controller.LeftX = Axis(_pad0, bind.LeftStickX);
            Controller.LeftY = Axis(_pad0, bind.LeftStickY);
            Controller.RightX = Axis(_pad0, bind.RightStickX);
            Controller.RightY = Axis(_pad0, bind.RightStickY);
        }

        if (_pad1 != null)
        {
            var bind = ConfigManager.Game.PadFor(1);
            Controller.State2 = PadState(_pad1, bind, Controller.State2);
            Controller.LeftX2 = Axis(_pad1, bind.LeftStickX);
            Controller.LeftY2 = Axis(_pad1, bind.LeftStickY);
            Controller.RightX2 = Axis(_pad1, bind.RightStickX);
            Controller.RightY2 = Axis(_pad1, bind.RightStickY);
        }
        else
        {
            Controller.LeftX2 = Controller.LeftY2 = Controller.RightX2 = Controller.RightY2 = 0x80;
        }
    }

    /// <summary>Pad 1's gyroscope into <see cref="Controller.GyroX"/> and its
    /// siblings, switched on only while a port asks (<see cref="Controller.WantGyro"/>):
    /// a pad streams its motion in a larger report once it is on (0102).</summary>
    private static void PollGyro()
    {
        if (_pad0 != _gyroPad)
        {
            _gyroPad = _pad0;
            _gyroOn = false;
        }

        var want = Controller.WantGyro && _sdl != null && _pad0 != null &&
                   _sdl.GameControllerHasSensor(_pad0, SensorType.Gyro) == SdlBool.True;
        if (want != _gyroOn && _sdl != null && _pad0 != null)
        {
            var ok = _sdl.GameControllerSetSensorEnabled(_pad0, SensorType.Gyro,
                want ? SdlBool.True : SdlBool.False) == 0;
            _gyroOn = want && ok;
        }

        Controller.Gyro = _gyroOn;
        if (!_gyroOn)
        {
            Controller.GyroX = Controller.GyroY = Controller.GyroZ = 0f;
            return;
        }

        var data = stackalloc float[3];
        if (_sdl!.GameControllerGetSensorData(_pad0, SensorType.Gyro, data, 3) != 0) return;
        Controller.GyroX = data[0];
        Controller.GyroY = data[1];
        Controller.GyroZ = data[2];
    }

    /// <summary>The port's <see cref="Controller.Rumble"/> out to pad 1 (0104): as
    /// HD rumble while <see cref="Controller.WantHdRumble"/> holds and the pad is a
    /// Switch Pro Controller or a single Joy-Con, as haptics on a DualSense over USB
    /// (<see cref="PadHaptics"/>, 0105), otherwise through SDL's two motors. Nothing
    /// is sent while no port asks, so the game's own rumble (<see cref="SetRumble"/>)
    /// is left alone.</summary>
    private static void PollRumble()
    {
        lock (_rumbleLock)
        {
            if (_pad0 != _rumblePad)
            {
                ForgetRumble();
                _rumblePad = _pad0;
            }
            if (_sdl == null || _pad0 == null)
            {
                Controller.HdRumble = false;
                return;
            }

            if (!Controller.WantHdRumble)
            {
                if (_hd != null) CloseHd();
                PadHaptics.Close(_sdl);
                _hdTried = false;
            }
            else if (!_hdTried)
            {
                OpenHd();
            }
            Controller.HdRumble = _hd != null || PadHaptics.Open;

            var wave = Controller.Rumble;
            if (wave == null || (wave.Low <= 0f && wave.High <= 0f))
            {
                if (_rumbleSent != null) StopRumble();
                _rumbleSent = null;
                return;
            }

            // The haptics are a stream, topped up on every poll rather than resent.
            if (PadHaptics.Open)
            {
                PadHaptics.Feed(_sdl, wave);
                _rumbleSent = wave;
                return;
            }

            var now = Environment.TickCount64;
            if (ReferenceEquals(wave, _rumbleSent) && now - _rumbleSentMs < RumbleRefreshMs) return;

            if (_hd != null && !WriteHd(wave))
            {
                Console.WriteLine("[Input] HD rumble: the pad refused a report; two-motor rumble instead");
                CloseHd();
            }
            if (_hd == null)
                _sdl.GameControllerRumble(_pad0, Motor(wave.Low), Motor(wave.High), (uint)(RumbleRefreshMs * 4));

            _rumbleSent = wave;
            _rumbleSentMs = now;
        }
    }

    private static ushort Motor(float amp) => (ushort)(Math.Clamp(amp, 0f, 1f) * 65535f);

    private static void OpenHd()
    {
        _hdTried = true;
        if (PadHaptics.TryOpen(_sdl!, _pad0)) return;
        var type = _sdl!.GameControllerGetType(_pad0);
        if (type is not (GameControllerType.NintendoSwitchPro or GameControllerType.NintendoSwitchJoyconLeft
                         or GameControllerType.NintendoSwitchJoyconRight))
            return;

        // SDL's HIDAPI driver has the pad, and its path is the HID device's; a
        // second handle on it only writes. A pad SDL reads some other way (a
        // kernel driver's event node) has no such path and keeps the two motors.
        var path = _sdl.GameControllerPathS(_pad0);
        if (string.IsNullOrEmpty(path)) return;
        _sdl.HidInit();
        _hd = _sdl.HidOpenPath(path, 0);
        Console.WriteLine(_hd != null ? $"[Input] HD rumble: {type} at {path}"
                                      : $"[Input] HD rumble: could not open {path}; two-motor rumble instead");
    }

    private static void CloseHd()
    {
        WriteHd(null);
        _sdl!.HidClose(_hd);
        _hd = null;
    }

    private static void StopRumble()
    {
        if (PadHaptics.Open) PadHaptics.Stop(_sdl!);
        else if (_hd != null) WriteHd(null);
        else _sdl!.GameControllerRumble(_pad0, 0, 0, 0);
    }

    private static void ForgetRumble()
    {
        if (_sdl != null) PadHaptics.Close(_sdl);
        if (_hd != null) CloseHd();
        else if (_rumbleSent != null && _sdl != null && _rumblePad != null)
            _sdl.GameControllerRumble(_rumblePad, 0, 0, 0);
        _hdTried = false;
        _rumbleSent = null;
        _rumblePad = null;
        Controller.HdRumble = false;
    }

    /// <summary>One rumble report (output report 0x10: the id, a counter, then four
    /// bytes for the left actuator and four for the right), or the neutral one for
    /// null. A single Joy-Con reads its own half, so both carry the wave.</summary>
    private static bool WriteHd(Controller.RumbleWave? wave)
    {
        var buf = stackalloc byte[10];
        buf[0] = 0x10;
        buf[1] = (byte)(_hdCount++ & 0x0F);
        if (wave == null)
        {
            EncodeHd(160f, 0f, 320f, 0f, buf + 2);
        }
        else
        {
            EncodeHd(wave.LowHz, wave.Low, wave.HighHz, wave.High, buf + 2);
        }
        for (int i = 0; i < 4; i++) buf[6 + i] = buf[2 + i];
        return _sdl!.HidWrite(_hd, buf, 10) >= 0;
    }

    /// <summary>
    /// One actuator's four bytes, as the reverse-engineered rumble table has them
    /// (dekuNukem's Nintendo_Switch_Reverse_Engineering, rumble_data_table.md): a
    /// frequency is <c>round(log2(hz / 10) * 32)</c>, the high band's taken from
    /// 0x60 four times over (81.75..1252 Hz) and the low band's from 0x40
    /// (40.875..626 Hz); an amplitude is a code 0..100, doubled for the high band
    /// and for the low band halved onto 0x40 with its odd step in the top bit of
    /// the frequency's byte. Silence at 160 and 320 Hz is <c>00 01 40 40</c>.
    /// </summary>
    private static void EncodeHd(float lowHz, float low, float highHz, float high, byte* o)
    {
        var hf = (HdFreq(highHz, 81.75f, 1252f) - 0x60) * 4;
        var lf = HdFreq(lowHz, 40.875f, 626f) - 0x40;
        var ha = HdAmp(high);
        var la = HdAmp(low);
        o[0] = (byte)(hf & 0xFF);
        o[1] = (byte)((ha * 2) | ((hf >> 8) & 0x01));
        o[2] = (byte)(lf | ((la & 1) != 0 ? 0x80 : 0));
        o[3] = (byte)(0x40 + la / 2);
    }

    private static int HdFreq(float hz, float min, float max) =>
        (int)MathF.Round(MathF.Log2(Math.Clamp(float.IsFinite(hz) ? hz : min, min, max) / 10f) * 32f);

    /// <summary>The table's amplitude code: two logarithmic pieces above 0.12 and a
    /// straight line below, 100 at full (0xC8 on the high band, the table's top).</summary>
    private static int HdAmp(float amp)
    {
        if (!(amp > 0f)) return 0;
        amp = Math.Min(amp, 1f);
        var code = amp > 0.23f ? MathF.Log2(amp * 8.7f) * 32f
                 : amp > 0.12f ? MathF.Log2(amp * 17f) * 16f
                 : amp / 0.12f * 16f;
        return Math.Clamp((int)MathF.Round(code), 0, 100);
    }

    private static byte Axis(GameController* ctrl, int index)
    {
        if (_sdl == null || index < 0) return 0x80;
        return AxisToByte(_sdl.GameControllerGetAxis(ctrl, (GameControllerAxis)index));
    }

    private static ushort PadState(GameController* ctrl, GamepadBindings pad, ushort s)
    {
        s = Apply(ctrl, pad.Cross, Controller.Cross, s);
        s = Apply(ctrl, pad.Circle, Controller.Circle, s);
        s = Apply(ctrl, pad.Square, Controller.Square, s);
        s = Apply(ctrl, pad.Triangle, Controller.Triangle, s);
        s = Apply(ctrl, pad.L1, Controller.L1, s);
        s = Apply(ctrl, pad.R1, Controller.R1, s);
        s = Apply(ctrl, pad.L2, Controller.L2, s);
        s = Apply(ctrl, pad.R2, Controller.R2, s);
        s = Apply(ctrl, pad.L3, Controller.L3, s);
        s = Apply(ctrl, pad.R3, Controller.R3, s);
        s = Apply(ctrl, pad.Start, Controller.Start, s);
        s = Apply(ctrl, pad.Select, Controller.Select, s);
        s = Apply(ctrl, pad.Up, Controller.Up, s);
        s = Apply(ctrl, pad.Down, Controller.Down, s);
        s = Apply(ctrl, pad.Left, Controller.Left, s);
        s = Apply(ctrl, pad.Right, Controller.Right, s);
        return s;
    }

    private static ushort Apply(GameController* ctrl, int[] bindings, ushort bit, ushort s)
    {
        foreach (var binding in bindings)
            if (Pressed(ctrl, binding))
                return (ushort)(s & ~bit);
        return s;
    }

    private static bool Pressed(GameController* ctrl, int binding)
    {
        if (_sdl == null) return false;
        if (binding == LeftTrigger)
            return _sdl.GameControllerGetAxis(ctrl, GameControllerAxis.Triggerleft) > AxisThreshold;
        if (binding == RightTrigger)
            return _sdl.GameControllerGetAxis(ctrl, GameControllerAxis.Triggerright) > AxisThreshold;
        if (IsStickBinding(binding))
        {
            var (axis, positive) = AxisBinding(binding);
            var v = _sdl.GameControllerGetAxis(ctrl, axis);
            return positive ? v > StickThreshold : v < -StickThreshold;
        }

        return _sdl.GameControllerGetButton(ctrl, (GameControllerButton)binding) != 0;
    }

    private static byte AxisToByte(short axis)
    {
        var f = Math.Clamp(axis * 1.3f / 32768.0f, -1.0f, 1.0f);
        return (byte)Math.Clamp((int)MathF.Round((f + 1.0f) * 127.5f), 0, 255);
    }

    public static void SetRumble(byte large, byte small)
    {
        if (_sdl == null || _pad0 == null) return;
        var lo = (ushort)(large * 257);
        var hi = small != 0 ? (ushort)65535 : (ushort)0;
        var duration = large == 0 && small == 0 ? 0u : 500u;
        _sdl.GameControllerRumble(_pad0, lo, hi, duration);
    }

    private static void OnKeyDown(IKeyboard kb, Key key, int _)
    {
        if (key == Key.F1) _topBarToggle = true;
        if (key == Key.F11) _fullscreenToggle = true;

        if (EventBus.HasAnyListeners<KeyboardEvent>())
            EventBus.Dispatch(new KeyboardEvent
            {
                Key = (int)key,
                Pressed = true
            });
    }

    private static void OnKeyUp(IKeyboard kb, Key key, int _)
    {
        if (EventBus.HasAnyListeners<KeyboardEvent>())
            EventBus.Dispatch(new KeyboardEvent
            {
                Key = (int)key,
                Pressed = false
            });
    }

    private static void OnMouseMove(IMouse mouse, Vector2 position)
    {
        if (_mouseSeen)
        {
            _mouseDx += position.X - _mousePos.X;
            _mouseDy += position.Y - _mousePos.Y;
        }
        _mousePos = position;
        _mouseSeen = true;

        if (EventBus.HasAnyListeners<MouseEvent>())
            EventBus.Dispatch(new MouseEvent
            {
                Action = MouseAction.Move,
                X = (int)position.X,
                Y = (int)position.Y
            });
    }

    private static void OnMouseDown(IMouse mouse, MouseButton mouseButton)
    {
        if (EventBus.HasAnyListeners<MouseEvent>())
            EventBus.Dispatch(new MouseEvent
            {
                Action = MouseAction.Button,
                Button = MapMouseButton(mouseButton),
                Pressed = true,
                X = (int)mouse.Position.X,
                Y = (int)mouse.Position.Y
            });
    }

    private static void OnMouseUp(IMouse mouse, MouseButton mouseButton)
    {
        if (EventBus.HasAnyListeners<MouseEvent>())
            EventBus.Dispatch(new MouseEvent
            {
                Action = MouseAction.Button,
                Button = MapMouseButton(mouseButton),
                Pressed = false,
                X = (int)mouse.Position.X,
                Y = (int)mouse.Position.Y
            });
    }

    private static void OnScroll(IMouse mouse, ScrollWheel wheel)
    {
        _mouseWheel += wheel.Y;

        if (EventBus.HasAnyListeners<MouseEvent>())
            EventBus.Dispatch(new MouseEvent
            {
                Action = MouseAction.Wheel,
                Wheel = wheel.Y,
                X = (int)mouse.Position.X,
                Y = (int)mouse.Position.Y
            });
    }

    private static EvMouseButton MapMouseButton(MouseButton button)
    {
        return button switch
        {
            MouseButton.Left => EvMouseButton.Left,
            MouseButton.Right => EvMouseButton.Right,
            MouseButton.Middle => EvMouseButton.Middle,
            _ => EvMouseButton.None
        };
    }
}