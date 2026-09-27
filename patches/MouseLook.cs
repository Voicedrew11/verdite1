using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hardware;
using RecompOne.Runtime.Host;
using RecompOne.Runtime.Host.Window;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;
using Silk.NET.Input;
using MouseButton = Silk.NET.Input.MouseButton;

namespace Kf2;

/// <summary>
/// Mouse look for King's Field.
///
/// The player's rotation is an SVECTOR at <c>0x800A0838</c>: pitch at
/// <c>+0</c>, which stage A (<c>func_80018880</c>) holds inside +/-0xBF (about 17
/// degrees either side of level) as it applies the look buttons, and yaw at
/// <c>+2</c>, 12 bits to the circle, which the walking code reads signed to
/// decide which way a step goes. Stage A reads the pad once a tick, and this
/// spends the mouse at the end of it: the motion since the last tick added to yaw
/// and to pitch, pitch clamped where the game clamps it. The turn therefore lands
/// before stage B copies the player into the camera and before the next tick
/// walks, as the D-pad's does.
///
/// The picture does not wait for the tick: <see cref="ViewCarry"/> adds the motion
/// not yet spent to the view it draws, and the share of the last tick's turn that
/// came from the mouse is shown whole rather than interpolated, so the mouse moves
/// the view on the frame it moves. That is King's Field II's "The mouse leads the
/// tick" (docs/INPUT.md), in two lines of ViewCarry.
///
/// Escape captures the pointer and lets it go; a popup opening takes it back. The
/// three mouse buttons press pad buttons while it is captured.
///
///     KF2_MOUSE=0                   off
///     KF2_MOUSE_TURN=1.0 KF2_MOUSE_LOOK=1.0 KF2_MOUSE_INVERTY=1
///     KF2_MOUSE_BUTTONS=Square,Triangle,Cross   left, right, middle
///     KF2_MOUSE_LEAD=0              the view waits for the tick
/// </summary>
public static class MouseLook
{
    const uint Pitch = 0x800A0838, Yaw = 0x800A083A;
    const short PitchLimit = 0xBF;
    const uint StageA = 0x80018880;

    const float UnitsPerDegree = 4096f / 360f;
    const float DegreesPerPixel = 0.15f;
    const int StepCap = 1024;
    const long StaleMs = 250;

    public static bool Enabled = true;
    public static bool Lead = true;
    public static float TurnSens = 1f, LookSens = 1f;
    public static bool InvertY;
    public static ushort LeftButton = Controller.Square, RightButton = Controller.Triangle, MiddleButton = Controller.Cross;
    public static Key CaptureKey = Key.Escape;

    public static bool Captured { get; private set; }

    static readonly ModInfo _self = new()
    {
        Id = "kf1.mouselook",
        Name = "Mouse look",
        Version = "1.0",
        Description = "Turns and tips King's Field's view with the mouse.",
    };

    static float _pendTurn, _pendPitch;
    static long _polled, _spentAt;
    static long _checked;

    /// <summary>What the mouse turned the player by at the last tick, after the
    /// game's clamp: <see cref="ViewCarry"/> shows it whole.</summary>
    public static int SpentYaw { get; private set; }
    public static int SpentPitch { get; private set; }

    public static void Configure()
    {
        Env("KF2_MOUSE", ref Enabled);
        Env("KF2_MOUSE_LEAD", ref Lead);
        Env("KF2_MOUSE_INVERTY", ref InvertY);
        EnvF("KF2_MOUSE_TURN", ref TurnSens);
        EnvF("KF2_MOUSE_LOOK", ref LookSens);
        var buttons = Environment.GetEnvironmentVariable("KF2_MOUSE_BUTTONS");
        if (!string.IsNullOrWhiteSpace(buttons))
        {
            var names = buttons.Split(',', StringSplitOptions.TrimEntries);
            if (names.Length > 0) LeftButton = Button(names[0], LeftButton);
            if (names.Length > 1) RightButton = Button(names[1], RightButton);
            if (names.Length > 2) MiddleButton = Button(names[2], MiddleButton);
        }
        var key = Environment.GetEnvironmentVariable("KF2_MOUSE_KEY");
        if (!string.IsNullOrWhiteSpace(key) && Enum.TryParse<Key>(key.Trim(), true, out var k)) CaptureKey = k;
    }

    static void Env(string name, ref bool v)
    {
        var s = Environment.GetEnvironmentVariable(name);
        if (!string.IsNullOrWhiteSpace(s)) v = s.Trim() != "0";
    }

    static void EnvF(string name, ref float v)
    {
        var s = Environment.GetEnvironmentVariable(name);
        if (float.TryParse(s, System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out var f)) v = f;
    }

    static ushort Button(string name, ushort fallback) => name.ToLowerInvariant() switch
    {
        "none" => 0,
        "cross" => Controller.Cross, "circle" => Controller.Circle,
        "square" => Controller.Square, "triangle" => Controller.Triangle,
        "l1" => Controller.L1, "r1" => Controller.R1, "l2" => Controller.L2, "r2" => Controller.R2,
        "start" => Controller.Start, "select" => Controller.Select,
        _ => fallback,
    };

    public static void Install()
    {
        if (!Enabled) return;

        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            PanelManager.Register(MouseIndicator.Instance);
            Console.WriteLine($"[KF1] mouse: {CaptureKey} captures the pointer (turn x{TurnSens:0.##}, look x{LookSens:0.##})");
        });

        Event.AddListener<KeyboardEvent>(e =>
        {
            if (!e.Pressed || e.Key != (int)CaptureKey) return;
            if (!Captured && PopupManager.AnyOpen) return;
            SetCaptured(!Captured);
        });

        HookAttach.OnOverlayLoad("mouse look", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var target = SymbolRegistry.Resolve("game", null, StageA);
        if (target == null) return false;
        HookManager.AddPost(_self, target, typeof(MouseLook).GetMethod(nameof(AfterStageA),
            BindingFlags.Public | BindingFlags.Static)!);
        HookManager.Commit();
        return HookAttach.Installed(target);
    }

    static readonly Action<PadReadEvent> _buttons = e =>
    {
        if (e.Port != 0 || !Captured) return;
        long now = Environment.TickCount64;
        if (now != _checked)
        {
            _checked = now;
            if (PopupManager.AnyOpen) { SetCaptured(false); return; }
        }

        ushort press = 0;
        if (HostWindow.IsMouseButtonDown(MouseButton.Left)) press |= LeftButton;
        if (HostWindow.IsMouseButtonDown(MouseButton.Right)) press |= RightButton;
        if (HostWindow.IsMouseButtonDown(MouseButton.Middle)) press |= MiddleButton;
        if (press == 0) return;
        // The buffer is active low with its bytes the other way round from
        // Controller's layout (see Mouse's note on the same line).
        e.Buttons &= (ushort)~(ushort)((press >> 8) | (press << 8));
    };

    public static void SetCaptured(bool on)
    {
        if (on == Captured) return;
        HostWindow.MouseCaptured = on;
        Captured = HostWindow.MouseCaptured;
        if (Captured) Event.AddListener(_buttons);
        else Event.RemoveListener(_buttons);
        HostWindow.TakeMouseMotion();
        _pendTurn = _pendPitch = 0f;
        _polled = _spentAt = Environment.TickCount64;
        MouseIndicator.Show(Captured);
    }

    /// <summary>Drain the host's motion into the pending sum. Every picture calls
    /// this through <see cref="Pending"/>, and the tick through its spend.</summary>
    // KF2_MOUSE_SYNTH=30: pretend the pointer is captured and moving 30 pixels
    // right every tick. A test of the spend and the lead without a person.
    static readonly float Synth = float.TryParse(Environment.GetEnvironmentVariable("KF2_MOUSE_SYNTH"),
        System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var v) ? v : 0f;

    static void Poll()
    {
        long now = Environment.TickCount64;
        var (dx, dy) = HostWindow.TakeMouseMotion();
        long gap = now - _polled;
        _polled = now;
        // Motion while the look is not being spent (a menu, a load) is not a turn
        // anybody asked for.
        if (!Captured || gap > StaleMs || now - _spentAt > StaleMs)
        {
            _pendTurn = _pendPitch = 0f;
            return;
        }
        const float units = DegreesPerPixel * UnitsPerDegree;
        _pendTurn += -dx * units * TurnSens;
        _pendPitch += dy * units * LookSens * (InvertY ? -1f : 1f);
    }

    /// <summary>Motion drawn but not yet spent, for the view to show now.</summary>
    public static (int Turn, int Pitch) Pending
    {
        get
        {
            if (!Captured || !Lead) return (0, 0);
            Poll();
            return ((int)Math.Clamp(_pendTurn, -StepCap, StepCap), (int)Math.Clamp(_pendPitch, -StepCap, StepCap));
        }
    }

    public static void AfterStageA(CpuContext c, IMemory m)
    {
        SpentYaw = SpentPitch = 0;
        if (Synth != 0f)
        {
            Captured = true;
            _pendTurn += -Synth * DegreesPerPixel * UnitsPerDegree * TurnSens;
            _spentAt = _polled = Environment.TickCount64;
        }
        if (!Captured) { _spentAt = Environment.TickCount64; return; }
        Poll();
        _spentAt = Environment.TickCount64;

        int turn = (int)Math.Clamp(_pendTurn, -StepCap, StepCap);
        int pitch = (int)Math.Clamp(_pendPitch, -StepCap, StepCap);
        _pendTurn -= turn;
        _pendPitch -= pitch;
        if (turn == 0 && pitch == 0) return;

        ushort yaw0 = m.ReadU16(Yaw);
        // Not masked: the game keeps the low sixteen bits of whatever it sums.
        m.WriteU16(Yaw, (ushort)(yaw0 + turn));
        short p0 = (short)m.ReadU16(Pitch);
        short p1 = (short)Math.Clamp(p0 + pitch, -PitchLimit, PitchLimit);
        m.WriteU16(Pitch, (ushort)p1);

        SpentYaw = turn;
        SpentPitch = p1 - p0;
    }
}
