using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hardware;
using RecompOne.Runtime.Host;
using RecompOne.Runtime.Host.Window;

namespace Kf2;

/// <summary>
/// Twin-stick control for King's Field on a gamepad.
///
/// The runtime binds the left stick onto the D-pad, which in this game walks and
/// turns: a tank control on the stick, and nothing on the right one. This makes
/// the left stick walk and strafe -- its X presses L1/R1 (the game's strafe, see
/// the control map in docs/KF1.md) instead of Left/Right, unless the D-pad itself
/// is held -- and the right stick turn and look, proportionally, spent at the end
/// of stage A with the mouse (<see cref="MouseLook"/>). The stick's turn goes into
/// the tick like the D-pad's and is interpolated by the view like it, not led.
///
///     KF2_ANALOG=0                 the runtime's own mapping
///     KF2_ANALOG_TURN=1.0 KF2_ANALOG_LOOK=1.0 KF2_ANALOG_DEADZONE=0.15 KF2_ANALOG_INVERTY=1
/// </summary>
public static class TwinStick
{
    public static bool Enabled = true;
    public static float TurnSens = 1f, LookSens = 1f, Deadzone = 0.15f;
    public static bool InvertY;

    public const string OnKey = "kf1.analog.on", TurnKey = "kf1.analog.turn", LookKey = "kf1.analog.look",
        DeadzoneKey = "kf1.analog.deadzone", InvertKey = "kf1.analog.inverty";
    static readonly HashSet<string> _pinned = new();

    /// <summary>Full deflection: about 1.5 times the D-pad's turn (which measures
    /// about 560 units a second), in 12-bit angle units a second.</summary>
    const float TurnPerSecond = 840f;
    const float LookPerSecond = 600f;

    // SDL button codes the runtime's bindings use: the D-pad's own Left and Right.
    const int DpadLeft = 13, DpadRight = 14;

    public static void Configure()
    {
        if (Env("KF2_ANALOG", ref Enabled)) _pinned.Add(OnKey);
        if (Env("KF2_ANALOG_INVERTY", ref InvertY)) _pinned.Add(InvertKey);
        if (EnvF("KF2_ANALOG_TURN", ref TurnSens)) _pinned.Add(TurnKey);
        if (EnvF("KF2_ANALOG_LOOK", ref LookSens)) _pinned.Add(LookKey);
        if (EnvF("KF2_ANALOG_DEADZONE", ref Deadzone)) _pinned.Add(DeadzoneKey);
    }

    static bool Env(string n, ref bool v)
    {
        var s = Environment.GetEnvironmentVariable(n);
        if (string.IsNullOrWhiteSpace(s)) return false;
        v = s.Trim() != "0";
        return true;
    }

    static bool EnvF(string n, ref float v)
    {
        if (!float.TryParse(Environment.GetEnvironmentVariable(n), System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out var f)) return false;
        v = f;
        return true;
    }

    public static void Install()
    {
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            var view = RecompOne.Runtime.Runtime.View;
            if (!_pinned.Contains(OnKey)) Enabled = view.GetBool(OnKey, Enabled);
            if (!_pinned.Contains(InvertKey)) InvertY = view.GetBool(InvertKey, InvertY);
            if (!_pinned.Contains(TurnKey)) TurnSens = view.GetFloat(TurnKey, TurnSens);
            if (!_pinned.Contains(LookKey)) LookSens = view.GetFloat(LookKey, LookSens);
            if (!_pinned.Contains(DeadzoneKey)) Deadzone = view.GetFloat(DeadzoneKey, Deadzone);
        });
        // Always listening; it reads Enabled on each pad read, so the setting can
        // change in play.
        Event.AddListener<PadReadEvent>(Strafe);
    }

    static float Axis(byte raw)
    {
        float v = (raw - 128) / 127f;
        float a = Math.Abs(v);
        if (a < Deadzone) return 0f;
        return Math.Sign(v) * Math.Min(1f, (a - Deadzone) / (1f - Deadzone));
    }

    /// <summary>Left stick X as strafe: the binding put it on Left/Right; take it
    /// off there (unless the D-pad is what is held) and press L1/R1 instead. The
    /// buffer is active low with its bytes swapped from Controller's layout.</summary>
    static void Strafe(PadReadEvent e)
    {
        if (!Enabled || e.Port != 0 || !HostWindow.IsPadConnected(0)) return;
        float x = Axis(Controller.LeftX);
        if (x == 0f) return;

        static ushort Sw(ushort b) => (ushort)((b >> 8) | (b << 8));
        if (x < 0 && !HostWindow.IsPadButtonDown(DpadLeft))
        {
            e.Buttons |= Sw(Controller.Left);
            e.Buttons &= (ushort)~Sw(Controller.L1);
        }
        else if (x > 0 && !HostWindow.IsPadButtonDown(DpadRight))
        {
            e.Buttons |= Sw(Controller.Right);
            e.Buttons &= (ushort)~Sw(Controller.R1);
        }
    }

    static long _lastTake = Environment.TickCount64;

    /// <summary>The right stick's turn and look for one tick, in game units:
    /// turn positive to the left, pitch positive down, as the mouse's.</summary>
    internal static (int Turn, int Pitch) TakeLook()
    {
        long now = Environment.TickCount64;
        double dt = Math.Clamp((now - _lastTake) / 1000.0, 0.0, 0.1);
        _lastTake = now;
        if (!Enabled || !HostWindow.IsPadConnected(0)) return (0, 0);
        float x = Axis(Controller.RightX), y = Axis(Controller.RightY);
        // A squared response: fine aim near the middle, full speed at the edge.
        float tx = x * Math.Abs(x), ty = y * Math.Abs(y);
        int turn = (int)Math.Round(-tx * TurnPerSecond * TurnSens * dt);
        int pitch = (int)Math.Round(ty * LookPerSecond * LookSens * dt * (InvertY ? -1 : 1));
        return (turn, pitch);
    }
}
