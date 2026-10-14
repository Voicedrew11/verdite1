using System.Diagnostics;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hardware;

namespace Verdite.Core;

/// <summary>
/// Aim a drawn bow with the pad's gyroscope: while a bow is in hand and being drawn,
/// turning and tipping the pad turns and tips the view, one to one.
///
///     {Tag}_GYROAIM=0         off (on by default)
///     {Tag}_GYROAIM_PROBE=1   a line a second: the weapon, the clock, the clip, the
///                             attack button, vblanks gated in, the units handed to
///                             the look
///
/// **Drawn** is <see cref="BowReads.Drawn"/>, the port's bow read once a tick. The game
/// holds full draw while attack is held, so the aim lasts as long as the hold.
///
/// The rate is integrated every vblank while the gate holds, and the look routine's
/// tick spends the sum through <c>Analog.BeforeLook</c> beside the right stick, as an
/// amount the way the mouse's is. A sum the look routine has not spent in
/// <see cref="StaleMs"/> (a menu, a cutscene) is dropped. See "Gyro aim with a drawn
/// bow" in docs/INPUT.md.
/// </summary>
public static class GyroAim
{
    /// <summary>A gyro rate below this, in radians a second, is a hand at rest: it
    /// keeps a pad's drift from creeping the aim.</summary>
    const float GyroRest = 0.05f;

    const float Units = 4096f / (2f * MathF.PI);     // angle units a radian

    /// <summary>A sum older than this is not the hand's any more.</summary>
    const double StaleMs = 250;

    /// <summary>The world has stopped when the tick routine has not run for this long
    /// (<see cref="Rumble"/>'s rule).</summary>
    const long StillMs = 150;

    public static string OnKey => Game.Id + ".gyroaim.on";

    public static bool Enabled = true;

    static readonly HashSet<string> _fromEnv = [];
    static readonly Stopwatch _clock = Stopwatch.StartNew();
    static BowReads? _bow;
    static double _lastMs, _takenMs;
    static float _turn, _pitch;

    static bool _probe;
    static double _probeAt;
    static long _gated, _ticks;
    static float _spentTurn, _spentPitch;

    public static void Configure(BowReads bow)
    {
        _bow = bow;
        Kept.Env(Game.EnvPrefix + "GYROAIM", OnKey, ref Enabled, _fromEnv);
        _probe = Game.Env("GYROAIM_PROBE")?.Trim() is "1" or "on" or "true";
    }

    public static void Install()
    {
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            Kept.Saved(OnKey, ref Enabled, _fromEnv);
            SetEnabled(Enabled);
        });
        Event.AddListener<VSyncEvent>(_ => Integrate());
        _bow?.Attach?.Invoke();
    }

    /// <summary>The pad streams its gyroscope (a larger report, runtime 0102) while
    /// either reader wants it.</summary>
    public static void SetEnabled(bool on)
    {
        Enabled = on;
        WantGyro();
        if (!on) _turn = _pitch = 0f;
    }

    /// <summary>The pad's gyroscope is on while this reader or the port's other one would
    /// use it: a pad streams a larger report once it is (runtime 0102), and one flag serves both.</summary>
    public static void WantGyro() => Controller.WantGyro = Enabled || (_bow?.GyroWanted?.Invoke() ?? false);

    static void Integrate()
    {
        double now = _clock.Elapsed.TotalMilliseconds;
        float dt = (float)Math.Clamp((now - _lastMs) / 1000.0, 0.0, 0.1);
        _lastMs = now;
        if (_probe) Probe(now);

        var bow = _bow;
        var m = bow?.Memory();
        if (!Enabled || !Controller.Gyro || bow == null || m == null ||
            Environment.TickCount64 - bow.TickMs() > StillMs || (bow.Busy?.Invoke() ?? false) || !bow.Drawn(m, out _))
        {
            _turn = _pitch = 0f;
            return;
        }
        if (now - _takenMs > StaleMs) _turn = _pitch = 0f;
        if (_probe) _gated++;

        // SDL's axes for a pad held in front of you: x across it, y up, z toward you,
        // anticlockwise positive. Turning the pad left (+y) turns the view left, which
        // is yaw increasing (the game's Left increases it). Tipping the far edge up
        // (+x) looks up, which is pitch decreasing (the way the mouse's pitch goes when
        // it is pushed away), so it takes the mouse's inversion with it.
        float gx = Rest(Controller.GyroX), gy = Rest(Controller.GyroY);
        _turn += gy * Units * dt;
        _pitch -= gx * Units * dt * (Mouse.InvertY ? -1f : 1f);
    }

    static float Rest(float rate) => MathF.Abs(rate) < GyroRest ? 0f : rate;

    /// <summary>What the gyro asks the look routine for this tick, in angle units,
    /// emptied as it is taken. Called once a tick from the port's look.</summary>
    internal static (float Turn, float Pitch) Take()
    {
        _takenMs = _clock.Elapsed.TotalMilliseconds;
        var taken = (_turn, _pitch);
        _turn = _pitch = 0f;
        if (_probe && (taken._turn != 0f || taken._pitch != 0f))
        {
            _ticks++;
            _spentTurn += taken._turn;
            _spentPitch += taken._pitch;
        }
        return taken;
    }

    static void Probe(double now)
    {
        if (now - _probeAt < 1000) return;
        _probeAt = now;
        var bow = _bow;
        var m = bow?.Memory();
        if (bow == null || m == null) return;
        Console.WriteLine($"[{Game.Tag}] gyro aim: {(Enabled ? "on" : "off")}, pad gyro {(Controller.Gyro ? "on" : "off")}, " +
                          $"weapon {m.ReadU8(bow.WeaponSlot)}, clock {(short)m.ReadU16(bow.SwingClock)}, clip {m.ReadU8(bow.ClipByte)}, " +
                          $"attack {(bow.AttackHeld(m) ? "held" : "up")}, " +
                          $"vblanks drawn {_gated}, ticks spent {_ticks}, turn {_spentTurn:0} pitch {_spentPitch:0}");
        _gated = _ticks = 0;
        _spentTurn = _spentPitch = 0f;
    }
}
