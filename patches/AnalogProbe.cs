using RecompOne.Runtime.Memory;

namespace Kf1;

/// <summary>
/// The measurement behind <see cref="Analog"/> (KF1_ANALOG_PROBE=1): what the sticks
/// asked for against what stage A then did, every 10 s; KF1_ANALOG_PROBE=trace adds a
/// line for every tick a stick or the mouse drove. The claim it checks is
/// Verdite2's: a stick at a share of its throw turns by that share of the D-pad's
/// step, not by zero (the carry) and not by more (the original's speed).
///
/// Called once a tick from Analog.Tick, after this tick's velocities are written and
/// before stage A applies them, so the yaw it sees moved is the previous tick's step.
/// </summary>
public static class AnalogProbe
{
    const uint Yaw = 0x800A083A, Pitch = 0x800A0838;
    const uint StrafeVel = 0x800A0840, FwdVel = 0x800A0842, TurnVel = 0x800A0846, PitchVel = 0x800A0848;
    const uint Position = 0x800A0824;
    const double Seconds = 10;

    static long _at;
    static int _ticks, _look, _move, _mouse, _turned, _sumStep, _maxStep;
    static bool _lookLast, _have;
    static ushort _yaw;
    static float _rx, _ry, _lx, _ly, _turnWant, _stickTurn, _mTurn, _mPitch;
    static int _rate, _speed;
    static bool _noteLook, _noteMove, _moveLast;

    /// <summary>A line a tick while anything drove (KF1_ANALOG_PROBE=trace).</summary>
    public static bool Trace = Environment.GetEnvironmentVariable("KF1_ANALOG_PROBE")?.Trim().ToLowerInvariant() == "trace";
    static int _tick;
    static int _xPrev, _zPrev;

    internal static void NoteLook(float x, float y, float lx, float turnWant, float stickTurn, int rate, float mTurn, float mPitch)
    {
        _noteLook = x != 0f || y != 0f || lx != 0f;
        (_rx, _ry, _turnWant, _stickTurn, _rate, _mTurn, _mPitch) = (x, y, turnWant, stickTurn, rate, mTurn, mPitch);
        if (mTurn != 0f || mPitch != 0f) _mouse++;
    }

    internal static void NoteMove(float x, float y, int speed)
    {
        _noteMove = true;
        (_lx, _ly, _speed) = (x, y, speed);
    }

    internal static void Tick(IMemory m)
    {
        if (!Analog.ProbeOn) { _have = false; _noteLook = _noteMove = false; return; }
        ushort yaw = m.ReadU16(Yaw);
        if (_have && _lookLast)
        {
            int d = (yaw - _yaw) & 0xFFF;
            if (d >= 0x800) d -= 0x1000;
            if (d != 0) _turned++;
            _sumStep += Math.Abs(d);
            _maxStep = Math.Max(_maxStep, Math.Abs(d));
        }
        if (Trace && (_lookLast || _moveLast || _noteLook || _noteMove))
        {
            int d = _have ? ((yaw - _yaw) & 0xFFF) is var u && u >= 0x800 ? u - 0x1000 : (yaw - _yaw) & 0xFFF : 0;
            int x = (int)m.ReadU32(Position), z = (int)m.ReadU32(Position + 8);
            Console.WriteLine($"[KF1] analog trace {_tick}: yaw {yaw} ({d:+0;-0;0}) pitch {(short)m.ReadU16(Pitch)} " +
                              $"moved ({x - _xPrev},{z - _zPrev}); now vel turn {(short)m.ReadU16(TurnVel)} pitch {(short)m.ReadU16(PitchVel)} " +
                              $"fwd {(short)m.ReadU16(FwdVel)} strafe {(short)m.ReadU16(StrafeVel)}");
        }
        _tick++;
        _xPrev = (int)m.ReadU32(Position);
        _zPrev = (int)m.ReadU32(Position + 8);
        _moveLast = _noteMove;
        _yaw = yaw;
        _have = true;
        _ticks++;
        if (_noteLook) _look++;
        if (_noteMove) _move++;
        _lookLast = _noteLook;
        _noteLook = _noteMove = false;

        long now = Environment.TickCount64;
        if (_at == 0) _at = now;
        if (now - _at < Seconds * 1000) return;
        _at = now;
        Console.WriteLine(
            $"[KF1] analog probe: {_ticks} ticks, {(Analog.Tank ? "3D" : "2D")}; look {_look}, move {_move}, mouse {_mouse}; " +
            $"R ({_rx:0.00},{_ry:0.00}) L ({_lx:0.00},{_ly:0.00}); turn want {_turnWant:0.0} eased {_stickTurn:0.0} at rate {_rate}, " +
            $"walk speed {_speed}; yaw moved on {_turned} of {_look} look ticks, mean |step| {(_look > 0 ? (double)_sumStep / _look : 0):0.0}, max {_maxStep}; " +
            $"vel turn {(short)m.ReadU16(TurnVel)} pitch {(short)m.ReadU16(PitchVel)} fwd {(short)m.ReadU16(FwdVel)} strafe {(short)m.ReadU16(StrafeVel)}; " +
            $"yaw {yaw} pitch {(short)m.ReadU16(Pitch)} pos ({(int)m.ReadU32(Position)},{(int)m.ReadU32(Position + 8)})");
        _ticks = _look = _move = _mouse = _turned = _sumStep = _maxStep = 0;
    }
}
