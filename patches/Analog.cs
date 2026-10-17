using RecompOne.Runtime.Hardware;
using RecompOne.Runtime.Memory;
using Rt = RecompOne.Runtime.Runtime;

namespace Kf1;

/// <summary>
/// Analog camera and movement: Verdite2's twin sticks, at the original's speed, on
/// this game's stage A. See "Analog sticks" in docs/INPUT.md.
///
///     KF1_ANALOG=0              hands the sticks back (on by default)
///     KF1_ANALOG_LOOK=0         the right stick does not turn or look
///     KF1_ANALOG_MOVE_ENABLE=0  the left stick does not walk or strafe
///     KF1_TANK=1                3D (one stick): the left stick walks and turns,
///                               the right stick is not read
///     KF1_ANALOG_TURN/PITCH/MOVE=1.0            sensitivities; 1 is the D-pad's rate
///     KF1_ANALOG_DEADZONE/MOVEDEADZONE=0.15     deadzones
///     KF1_ANALOG_CURVE/MOVECURVE=1.35/1.0       response curves
///     KF1_ANALOG_ACCEL=1 ACCELMAX=2.2 ACCELTIME=0.5   the look ramp (off)
///     KF1_ANALOG_INSTANTSTOP=1                  the camera stops on release (off)
///     KF1_ANALOG_EASE=0                         the right stick neither eases in nor out
///     KF1_ANALOG_INVERTY/INVERTTURN/INVERTSTRAFE/INVERTFWD=1
///     KF1_ANALOG_PROBE=1        the control-state report, every 10 s
///
/// Stage A (func_80018880) reads its pad word once a tick and drives four
/// velocities in Verdite2's three-branch form: a held button adds an accel and
/// clamps, no button decays toward 0, then the angle or position takes the
/// velocity. So nothing is replaced: in the post hook on the pad read that
/// <see cref="MouseLook"/> already owns, the velocity is pre-loaded with
/// <c>step ∓ accel</c> and the matching button set in the word, and the game's own
/// next instruction lands on the step. Collision, the pitch limit and the walk's
/// normalisation run on an amount chosen here.
///
///     turn    vel 0x800A0846  Left +, Right -   accel = decay = rate>>2, clamp rate
///     pitch   vel 0x800A0848  R2 +, L2 -        accel 3, decay 2, clamp 10
///     walk    vel 0x800A0842  Up +, Down -      accel speed>>2, decay speed>>3, clamp speed
///     strafe  vel 0x800A0840  R1 +, L1 -        accel = decay = speed>>2, clamp speed
///
/// The buttons are fixed in this game's code (no config table, unlike Verdite2's).
/// Sticks centred, the hook returns before touching anything, so the D-pad and the
/// keyboard behave as they do with this off.
/// </summary>
public static class Analog
{
    // ---- stage A's state ----
    const uint Condition = 0x800A07AA;  // u16; bit 3 slows the player
    const uint StrafeVel = 0x800A0840;
    const uint FwdVel = 0x800A0842;
    const uint TurnVel = 0x800A0846;
    const uint PitchVel = 0x800A0848;

    // The libetc bits stage A tests, named for what they do to the velocity.
    const ushort TurnInc = 0x8000, TurnDec = 0x2000;     // Left, Right
    const ushort PitchInc = 0x0002, PitchDec = 0x0001;   // R2, L2
    const ushort FwdInc = 0x1000, FwdDec = 0x4000;       // Up, Down
    const ushort StrafeInc = 0x0008, StrafeDec = 0x0004; // R1, L1

    const int PitchAccel = 3, PitchDecay = 2, PitchVelMax = 10;

    // How far past the game's own limit the camera may be driven (a sensitivity
    // above 1, or the look ramp): the no-button branch applies a velocity unclamped.
    const int OverspeedCap = 4;

    public const string OnKey = "kf1.analog.on";
    public const string LookKey = "kf1.analog.look";
    public const string MoveKey = "kf1.analog.move";
    public const string StopKey = "kf1.analog.instantstop";
    public const string AccelKey = "kf1.analog.accel";
    public const string EaseKey = "kf1.analog.ease";
    public const string AccelMaxKey = "kf1.analog.accelmax";
    public const string AccelTimeKey = "kf1.analog.acceltime";
    public const string TurnSensKey = "kf1.analog.turn";
    public const string PitchSensKey = "kf1.analog.pitch";
    public const string MoveSensKey = "kf1.analog.movesens";
    public const string LookDeadKey = "kf1.analog.lookdeadzone";
    public const string MoveDeadKey = "kf1.analog.movedeadzone";
    public const string LookCurveKey = "kf1.analog.lookcurve";
    public const string MoveCurveKey = "kf1.analog.movecurve";
    public const string InvertPitchKey = "kf1.analog.invertpitch";
    public const string InvertTurnKey = "kf1.analog.invertturn";
    public const string InvertStrafeKey = "kf1.analog.invertstrafe";
    public const string InvertFwdKey = "kf1.analog.invertforward";
    public const string ControlsKey = "kf1.controls";

    // Plain fields: each is a widget's `ref`.
    public static bool Enabled = true;
    public static bool AnalogLook = true;
    public static bool AnalogMove = true;
    public static bool Tank;

    // Off: it turns the view past the D-pad's rate, and the fights are built on how
    // fast the player can turn. Faster is something a player asks for.
    public static bool LookAccel;
    public static float LookAccelMax = 2.2f;
    public static float LookAccelTime = 0.5f;
    const float LookAccelThreshold = 0.8f;
    static float _accelT;
    static long _accelTick;

    public static float LookDeadzone = 0.15f;
    public static float MoveDeadzone = 0.15f;
    public static float LookCurve = 1.35f;
    public static float MoveCurve = 1.0f;
    // 3D reads each axis of the left stick on its own deadzone, so a thumb a little
    // off forward walks straight and a sideways push turns in place.
    const float TankDeadzone = 0.3f;
    const float TankTurnCurve = 2f;
    public static float TurnSens = 1.0f;
    public static float PitchSens = 1.0f;
    public static float MoveSens = 1.0f;

    public static bool InvertTurn;
    public static bool InvertPitch;
    public static bool InvertStrafe;
    public static bool InvertForward;

    // Off: the original's ease out is how its turning feels. Movement is never
    // stopped dead: its ramp-down is the walking momentum the game has always had.
    public static bool CameraInstantStop;

    // The right stick eased toward what it asks by the game's own step, as a held
    // D-pad and L2/R2 are. Off, it is direct, still capped at their rates. 2D only.
    public static bool StickEase = true;

    public static bool ProbeOn;

    static bool _ownedTurn, _ownedPitch;
    static bool _mouseTurn, _mousePitch;
    static float _turnCarry, _pitchCarry, _fwdCarry, _strafeCarry;
    static float _rampTurn, _rampPitch;
    static bool _rampTurnLive, _rampPitchLive;

    static readonly HashSet<string> _fromEnv = new(StringComparer.Ordinal);

    public static void Configure()
    {
        Env("KF1_ANALOG", OnKey, ref Enabled);
        Env("KF1_ANALOG_LOOK", LookKey, ref AnalogLook);
        Env("KF1_ANALOG_MOVE_ENABLE", MoveKey, ref AnalogMove);
        Env("KF1_TANK", ControlsKey, ref Tank);
        Env("KF1_ANALOG_INVERTY", InvertPitchKey, ref InvertPitch);
        Env("KF1_ANALOG_INVERTTURN", InvertTurnKey, ref InvertTurn);
        Env("KF1_ANALOG_INVERTSTRAFE", InvertStrafeKey, ref InvertStrafe);
        Env("KF1_ANALOG_INVERTFWD", InvertFwdKey, ref InvertForward);
        // One deadzone variable for both sticks; MOVEDEADZONE sets the left's apart.
        Env("KF1_ANALOG_DEADZONE", LookDeadKey, ref LookDeadzone);
        if (_fromEnv.Contains(LookDeadKey)) { MoveDeadzone = LookDeadzone; _fromEnv.Add(MoveDeadKey); }
        Env("KF1_ANALOG_MOVEDEADZONE", MoveDeadKey, ref MoveDeadzone);
        Env("KF1_ANALOG_CURVE", LookCurveKey, ref LookCurve);
        Env("KF1_ANALOG_MOVECURVE", MoveCurveKey, ref MoveCurve);
        Env("KF1_ANALOG_TURN", TurnSensKey, ref TurnSens);
        Env("KF1_ANALOG_PITCH", PitchSensKey, ref PitchSens);
        Env("KF1_ANALOG_MOVE", MoveSensKey, ref MoveSens);
        Env("KF1_ANALOG_INSTANTSTOP", StopKey, ref CameraInstantStop);
        Env("KF1_ANALOG_ACCEL", AccelKey, ref LookAccel);
        Env("KF1_ANALOG_EASE", EaseKey, ref StickEase);
        Env("KF1_ANALOG_ACCELMAX", AccelMaxKey, ref LookAccelMax);
        Env("KF1_ANALOG_ACCELTIME", AccelTimeKey, ref LookAccelTime);
        if (AnalogProbe.Trace) ProbeOn = true;
        else Env("KF1_ANALOG_PROBE", "", ref ProbeOn);
    }

    /// <summary>The kept values, once the config has loaded (TestingSection.Ready).</summary>
    public static void Ready()
    {
        Saved(OnKey, ref Enabled);
        Saved(LookKey, ref AnalogLook);
        Saved(MoveKey, ref AnalogMove);
        Saved(ControlsKey, ref Tank);
        Saved(StopKey, ref CameraInstantStop);
        Saved(AccelKey, ref LookAccel);
        Saved(EaseKey, ref StickEase);
        Saved(AccelMaxKey, ref LookAccelMax);
        Saved(AccelTimeKey, ref LookAccelTime);
        Saved(TurnSensKey, ref TurnSens);
        Saved(PitchSensKey, ref PitchSens);
        Saved(MoveSensKey, ref MoveSens);
        Saved(LookDeadKey, ref LookDeadzone);
        Saved(MoveDeadKey, ref MoveDeadzone);
        Saved(LookCurveKey, ref LookCurve);
        Saved(MoveCurveKey, ref MoveCurve);
        Saved(InvertPitchKey, ref InvertPitch);
        Saved(InvertTurnKey, ref InvertTurn);
        Saved(InvertStrafeKey, ref InvertStrafe);
        Saved(InvertFwdKey, ref InvertForward);
        Console.WriteLine($"[KF1] analog: {(Enabled ? "on" : "off")}, " +
                          (Tank ? "3D: the left stick walks and turns" : "2D: the left stick walks and strafes, the right turns and looks") +
                          $" (turn x{TurnSens:0.##}, move x{MoveSens:0.##}, ease {(StickEase ? "on" : "off")})");
    }

    /// <summary>This tick's walk speed and turn rate. Stage A writes them to
    /// 0x80057E68 and 0x80057E70 after the pad read, from the condition bit, so the
    /// hook works them out the same way rather than reading last tick's.</summary>
    static (int Speed, int Rate) Rates(IMemory m) =>
        (m.ReadU16(Condition) & 0x0008) != 0 ? (0x24, 5) : (0xB4, 0x1C);

    /// <summary>Stage A's pad word, from MouseLook's hook: the sticks and the mouse
    /// spent into it and the four velocities.</summary>
    public static ushort Tick(IMemory m, ushort pad)
    {
        var (speed, rate) = Rates(m);
        pad = Look(m, pad, rate);
        pad = Move(m, pad, speed);
        AnalogProbe.Tick(m);
        return pad;
    }

    // Turning and looking, and the mouse with them: two devices asking for the same
    // step, decided in one place so they never write the word twice in a tick.
    static ushort Look(IMemory m, ushort pad, int rate)
    {
        var (mTurn, mPitch) = Mouse.TakeLook();
        if (!(Mouse.Enabled && Mouse.Captured)) mTurn = mPitch = 0f;

        bool right = Enabled && AnalogLook && !Tank;
        bool left = Enabled && AnalogMove && (Tank || AnalogLook);
        var (x, y) = right ? Shape(Controller.RightX, Controller.RightY, LookDeadzone, LookCurve) : (0f, 0f);
        (float X, float Y) tankLeft = Tank ? TankLeft : (0f, 0f);

        // A held button takes the axis from a mouse that has stopped, as MouseLook
        // always did: no zeroing tick to eat the press.
        if (mTurn == 0f && (pad & (TurnInc | TurnDec)) != 0) _mouseTurn = false;
        if (mPitch == 0f && (pad & (PitchInc | PitchDec)) != 0) _mousePitch = false;

        if (!right && !left && mTurn == 0f && mPitch == 0f && !_mouseTurn && !_mousePitch)
        {
            _rampTurnLive = _rampPitchLive = false;
            return pad;
        }

        // The runtime binds the left stick to the D-pad, and the D-pad's Left/Right
        // turn: owning the turn with a zero step takes that leak away.
        var (lx, ly) = !left ? (0f, 0f)
                     : Tank ? tankLeft
                     : Shape(Controller.LeftX, Controller.LeftY, MoveDeadzone, MoveCurve);
        bool leftActive = left && (lx != 0f || ly != 0f);

        bool direct = !StickEase && !Tank;
        bool stopDead = CameraInstantStop || direct;
        bool releaseTurn = (stopDead && _ownedTurn) || _mouseTurn;
        bool releasePitch = (stopDead && _ownedPitch) || _mousePitch;

        if (x == 0f && y == 0f && !leftActive && mTurn == 0f && mPitch == 0f && !releaseTurn && !releasePitch)
        {
            _accelT = 0f;
            _accelTick = 0;
            // The game decays a released velocity itself, by the same step the ease
            // would, so the next push starts from wherever that has got to.
            _rampTurnLive = _rampPitchLive = false;
            return pad;
        }

        float mult = right ? Accelerate(RawMag(Controller.RightX, Controller.RightY, LookDeadzone)) : 1f;

        // Full deflection is the D-pad's rate and L2/R2's limit, times the sensitivity.
        // Stick right turns right, the decreasing branch.
        float turnWant = -x * rate * TurnSens * mult * (InvertTurn ? -1f : 1f);
        float pitchWant = y * PitchVelMax * PitchSens * mult * (InvertPitch ? -1f : 1f);
        bool tankTurn = Tank && leftActive;
        if (tankTurn) turnWant = -lx * rate * TurnSens * (InvertTurn ? -1f : 1f);

        int turnStep = rate >> 2;
        float stickTurn = direct ? (x != 0f ? turnWant : 0f)
            : x != 0f || tankTurn || _rampTurnLive
            ? Ramp(turnWant, ref _rampTurn, ref _rampTurnLive, m, TurnVel, turnStep, turnStep, _mouseTurn)
            : 0f;
        float stickPitch = direct ? (y != 0f ? pitchWant : 0f)
            : y != 0f || _rampPitchLive
            ? Ramp(pitchWant, ref _rampPitch, ref _rampPitchLive, m, PitchVel, PitchAccel, PitchDecay, _mousePitch)
            : 0f;
        if (direct) _rampTurnLive = _rampPitchLive = false;
        if (CameraInstantStop && x == 0f && !Tank) { stickTurn = 0f; _rampTurnLive = false; }
        if (CameraInstantStop && y == 0f) { stickPitch = 0f; _rampPitchLive = false; }
        Mouse.NoteSpent(m, mTurn, stickTurn, mPitch, stickPitch);

        if (x != 0f || leftActive || mTurn != 0f || releaseTurn || _rampTurnLive)
        {
            int step = Step(stickTurn + mTurn, ref _turnCarry, Ceiling(rate * OverspeedCap, mTurn));
            pad = Drive(m, pad, TurnVel, step, turnStep, turnStep, rate, TurnInc, TurnDec);
            // 3D's release is the D-pad's: the stick back in its deadzone leaves the
            // velocity to the game's decay.
            _ownedTurn = step != 0 && !Tank;
            _mouseTurn = mTurn != 0f;
        }

        if (y != 0f || mPitch != 0f || releasePitch || _rampPitchLive)
        {
            int step = Step(stickPitch + mPitch, ref _pitchCarry, Ceiling(PitchVelMax * OverspeedCap, mPitch));
            pad = Drive(m, pad, PitchVel, step, PitchAccel, PitchDecay, PitchVelMax, PitchInc, PitchDec);
            _ownedPitch = step != 0;
            _mousePitch = mPitch != 0f;
        }

        AnalogProbe.NoteLook(x, y, lx, turnWant, stickTurn, rate, mTurn, mPitch);
        return pad;
    }

    /// <summary>A stick asks for a rate, a mouse for an amount: a flick arrives as a
    /// few very large ticks, which the stick's ceiling would slow down.</summary>
    static int Ceiling(int stick, float mouse) => mouse == 0f ? stick : Math.Max(stick, Mouse.StepCap);

    // Walking and strafing, off this tick's walk speed. Not eased: the stick asks
    // for a speed and gets it, and a released stick leaves the game's own decay.
    static ushort Move(IMemory m, ushort pad, int speed)
    {
        if (!Enabled || !AnalogMove) return pad;
        // 3D: the sideways deflection turns (Look), and L1/R1 still strafe.
        var (x, y) = Tank ? (0f, TankLeft.Y) : Shape(Controller.LeftX, Controller.LeftY, MoveDeadzone, MoveCurve);
        if (x == 0f && y == 0f) return pad;

        int accel = speed >> 2;
        if (y != 0f)
        {
            int step = Step(-y * speed * MoveSens * (InvertForward ? -1f : 1f), ref _fwdCarry, speed);
            pad = Drive(m, pad, FwdVel, step, accel, speed >> 3, speed, FwdInc, FwdDec);
        }
        if (x != 0f)
        {
            int step = Step(x * speed * MoveSens * (InvertStrafe ? -1f : 1f), ref _strafeCarry, speed);
            pad = Drive(m, pad, StrafeVel, step, accel, accel, speed, StrafeInc, StrafeDec);
        }
        AnalogProbe.NoteMove(x, y, speed);
        return pad;
    }

    /// <summary>One tick of the game's own ease toward a velocity: by the accel while
    /// the stick asks, by the decay once it is released, as the button branches and
    /// the released branch move it. It carries on from the game's velocity when it
    /// takes over from a D-pad turn, not from a step the mouse drove.</summary>
    static float Ramp(float want, ref float state, ref bool live, IMemory m, uint velAddr,
                      int accel, int decay, bool wasMouse)
    {
        float from = live ? state : wasMouse ? 0f : (short)m.ReadU16(velAddr);
        int by = want == 0f ? decay : accel;
        state = Math.Clamp(want, from - by, from + by);
        live = state != 0f || want != 0f;
        return state;
    }

    /// <summary>Pre-load a velocity so the game's own branch lands on the step. Inside
    /// the clamp, assert the button and let its accel carry it; past it, assert
    /// nothing and let the unclamped decay land on it. A zero step clears both buttons
    /// and zeroes the velocity: the stick is the authority while it is deflected.</summary>
    static ushort Drive(IMemory m, ushort pad, uint velAddr, int step, int accel, int decay, int clamp,
                        ushort inc, ushort dec)
    {
        pad = (ushort)(pad & ~(inc | dec));
        if (step == 0) { m.WriteU16(velAddr, 0); return pad; }
        if (Math.Abs(step) <= clamp)
        {
            m.WriteU16(velAddr, (ushort)(short)(step > 0 ? step - accel : step + accel));
            pad |= step > 0 ? inc : dec;
        }
        else m.WriteU16(velAddr, (ushort)(short)(step > 0 ? step + decay : step - decay));
        return pad;
    }

    /// <summary>The look ramp's multiplier: 1 at rest, rising to LookAccelMax over
    /// LookAccelTime while the stick is held past the threshold, falling three times
    /// faster.</summary>
    static float Accelerate(float mag)
    {
        if (!LookAccel) { _accelT = 0f; return 1f; }
        long now = Environment.TickCount64;
        float dt = _accelTick == 0 ? 0f : Math.Clamp((now - _accelTick) / 1000f, 0f, 0.1f);
        _accelTick = now;
        _accelT = Math.Clamp(mag >= LookAccelThreshold ? _accelT + dt : _accelT - dt * 3f, 0f, LookAccelTime);
        float t = LookAccelTime <= 0f ? 1f : _accelT / LookAccelTime;
        return 1f + (LookAccelMax - 1f) * t;
    }

    static float RawMag(byte bx, byte by, float deadzone)
    {
        float x = (bx - 128) / 127f, y = (by - 128) / 127f;
        float mag = MathF.Sqrt(x * x + y * y);
        return mag <= deadzone ? 0f : Math.Clamp((mag - deadzone) / (1f - deadzone), 0f, 1f);
    }

    /// <summary>The whole part of the wanted step, the fraction carried to the next
    /// tick: at 15 Hz a small deflection would otherwise round to nothing.</summary>
    static int Step(float want, ref float carry, int limit)
    {
        float total = want + carry;
        int step = (int)MathF.Truncate(total);
        carry = total - step;
        return Math.Clamp(step, -limit, limit);
    }

    /// <summary>One stick, radially deadzoned and curved; the runtime's bytes, 0x80 centre.</summary>
    static (float X, float Y) Shape(byte bx, byte by, float deadzone, float curve)
    {
        float x = (bx - 128) / 127f, y = (by - 128) / 127f;
        float mag = MathF.Sqrt(x * x + y * y);
        if (mag <= deadzone || mag <= 0f) return (0f, 0f);
        float unit = Math.Clamp((mag - deadzone) / (1f - deadzone), 0f, 1f);
        float scaled = MathF.Pow(unit, curve) / mag;
        return (x * scaled, y * scaled);
    }

    /// <summary>The left stick for 3D, as (turn, walk), each on its own deadzone.</summary>
    static (float X, float Y) TankLeft => (
        Axis((Controller.LeftX - 128) / 127f, TankDeadzone, TankTurnCurve),
        Axis((Controller.LeftY - 128) / 127f, TankDeadzone, MoveCurve));

    static float Axis(float v, float deadzone, float curve)
    {
        float a = MathF.Abs(v);
        if (a <= deadzone) return 0f;
        return MathF.Sign(v) * MathF.Pow(Math.Clamp((a - deadzone) / (1f - deadzone), 0f, 1f), curve);
    }

    /// <summary>The two sticks raw, for the Testing tab's readout.</summary>
    public static (float Lx, float Ly, float Rx, float Ry) Sticks => (
        (Controller.LeftX - 128) / 127f, (Controller.LeftY - 128) / 127f,
        (Controller.RightX - 128) / 127f, (Controller.RightY - 128) / 127f);

    // ---- the variable first, then interface.ini; switches kept as 0/1, as the Testing tab keeps them ----

    static void Env(string name, string key, ref bool value) => Kept.Env(name, key, ref value, _fromEnv);
    static void Env(string name, string key, ref float value) => Kept.Env(name, key, ref value, _fromEnv);

    static void Saved(string key, ref bool value)
    {
        if (!_fromEnv.Contains(key)) value = Rt.View.GetInt(key, value ? 1 : 0) != 0;
    }

    static void Saved(string key, ref float value) => Kept.Saved(key, ref value, _fromEnv);
}
