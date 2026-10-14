using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hardware;
using RecompOne.Runtime.Memory;

namespace Verdite.Core;

/// <summary>
/// Rumble the pad as a bow is drawn and loosed: two-motor rumble on any pad that
/// has it, HD rumble on a Switch Pro Controller or a single Joy-Con (runtime 0104),
/// the actuators of a DualSense over USB (runtime 0105).
///
///     {Tag}_RUMBLE=0          no rumble (on by default)
///     {Tag}_RUMBLE_HD=0       two-motor rumble on a Switch pad and a DualSense too
///                             (HD on by default)
///     {Tag}_RUMBLE_PROBE=1    a line a second: the phase, the tension, waves asked, HD;
///                             2 also a line each tick the bow's state changes (the
///                             port's Bow reader)
///     {Tag}_RUMBLE_TEST=1     from 3 s after start-up, the bow three times over without
///                             one: nock, a 1.5 s draw, 1 s at full draw, the loose
///
/// The bow's state is the port's, handed in as a <see cref="BowReads"/>. The waves:
/// a click as the arrow is nocked; a creak while it is drawn, low and grained,
/// rising in pitch and strength with the tension; at full draw a steady strain with
/// a slow tremble; and on the loose a twang that dies away in a fifth of a second,
/// as strong as the draw was. A two-motor pad takes the amplitudes, the low band on
/// the large motor. See "Rumble" in docs/INPUT.md.
/// </summary>
public static class Rumble
{
    const double NockMs = 45, TwangMs = 200, TwangDecayMs = 50;

    /// <summary>The world has stopped (a menu, a cutscene) when the tick routine has
    /// not run for this long; a held draw's rumble stops with it.</summary>
    const long StillMs = 150;

    /// <summary>How long one grain of the creak lasts.</summary>
    const double GrainMs = 28;

    public static string OnKey => Game.Id + ".rumble.on";
    public static string HdKey => Game.Id + ".rumble.hd";

    public static bool Enabled = true;
    public static bool Hd = true;

    static readonly HashSet<string> _fromEnv = [];
    static readonly System.Diagnostics.Stopwatch _clock = System.Diagnostics.Stopwatch.StartNew();
    static readonly Random _grain = new(0x271D0);

    static BowReads? _bow;
    static bool _drawing;
    static float _tension, _power;
    static double _drawnMs, _loosedMs = double.NegativeInfinity, _grainMs;
    static float _grainLevel = 1f;

    static bool _test;
    static double _testAt = -1;
    const double TestCycleMs = 3500, TestDrawnMs = 1500, TestLooseMs = 2500;

    static bool _probe;
    static double _probeAt;
    static long _waves;
    static string _phase = "idle";

    public static void Configure(BowReads bow)
    {
        _bow = bow;
        Kept.Env(Game.EnvPrefix + "RUMBLE", OnKey, ref Enabled, _fromEnv);
        Kept.Env(Game.EnvPrefix + "RUMBLE_HD", HdKey, ref Hd, _fromEnv);
        string? probe = Game.Env("RUMBLE_PROBE")?.Trim();
        _probe = probe is "1" or "2" or "on" or "true";
        _test = Game.Env("RUMBLE_TEST")?.Trim() is "1" or "on" or "true";
        _probe |= _test;
    }

    public static void Install()
    {
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            Kept.Saved(OnKey, ref Enabled, _fromEnv);
            Kept.Saved(HdKey, ref Hd, _fromEnv);
            SetEnabled(Enabled);
        });
        Event.AddListener<VSyncEvent>(_ => Step());
        _bow?.Attach?.Invoke();
    }

    public static void SetEnabled(bool on)
    {
        Enabled = on;
        if (!on) Controller.Rumble = null;
        SetHd(Hd);
    }

    public static void SetHd(bool on)
    {
        Hd = on;
        Controller.WantHdRumble = Enabled && on;
    }

    static void Step()
    {
        double now = _clock.Elapsed.TotalMilliseconds;
        if (_probe) Probe(now);

        if (Enabled && _test && Test(now)) return;

        var bow = _bow;
        var m = bow?.Memory();
        if (!Enabled || bow == null || m == null || Environment.TickCount64 - bow.TickMs() > StillMs)
        {
            _drawing = false;
            Ask(null, "idle");
            return;
        }

        bool bowInHand = bow.InHand(m);
        bool drawing = bow.Drawn(m, out int clock);
        int clip = m.ReadU8(bow.ClipByte);
        if (drawing && !_drawing) _drawnMs = now;
        if (!drawing && _drawing && bowInHand && clip == 1 && clock >= 0)
        {
            _loosedMs = now;
            _power = 0.35f + 0.65f * _tension;
        }
        _drawing = drawing;

        if (drawing)
        {
            _tension = Math.Clamp(clock / (float)BowReads.FullDraw, 0f, 1f);
            if (now - _drawnMs < NockMs) Ask(Nock, "nock");
            else if (clock >= BowReads.FullDraw) Ask(Strain(now), "full draw");
            else Ask(Creak(now, _tension), "drawing");
            return;
        }

        double since = now - _loosedMs;
        if (since < TwangMs)
        {
            Ask(Twang(since, _power), "loosed");
            return;
        }

        Ask(null, "idle");
    }

    static readonly Controller.RumbleWave Nock = new(120f, 0.2f, 400f, 0.55f);

    /// <summary>{Tag}_RUMBLE_TEST: the bow's waves on a clock of their own, three times,
    /// for a pad on the desk. False before it starts, so the game's own run goes on.</summary>
    static bool Test(double now)
    {
        if (_testAt < 0) _testAt = now + 3000;
        double t = now - _testAt;
        if (t < 0) return false;
        if (t >= 3 * TestCycleMs)
        {
            _test = false;
            Ask(null, "idle");
            return true;
        }

        double u = t % TestCycleMs;
        if (u < NockMs) Ask(Nock, "test: nock");
        else if (u < TestDrawnMs) Ask(Creak(now, _tension = (float)((u - NockMs) / (TestDrawnMs - NockMs))), "test: drawing");
        else if (u < TestLooseMs) Ask(Strain(now), "test: full draw");
        else if (u < TestLooseMs + TwangMs) Ask(Twang(u - TestLooseMs, 1f), "test: loosed");
        else Ask(null, "test: rest");
        return true;
    }

    /// <summary>The loose: a twang dying away, as strong as the draw was.</summary>
    static Controller.RumbleWave Twang(double since, float power)
    {
        float e = (float)Math.Exp(-since / TwangDecayMs) * power;
        return new(90f, 0.75f * e, 520f, 0.95f * e);
    }

    /// <summary>The string drawn back: a low creak broken into grains, its pitch and
    /// strength rising with the tension.</summary>
    static Controller.RumbleWave Creak(double now, float t)
    {
        if (now - _grainMs >= GrainMs)
        {
            _grainMs = now;
            _grainLevel = 0.7f + 0.6f * (float)_grain.NextDouble();
        }
        return new(70f + 80f * t, (0.12f + 0.3f * t) * _grainLevel, 170f + 180f * t, 0.04f + 0.14f * t);
    }

    /// <summary>Held at full draw: a steady strain with a slow tremble in it.</summary>
    static Controller.RumbleWave Strain(double now)
    {
        float tremble = MathF.Sin((float)(now / 1000.0 * 2.0 * Math.PI * 7.0));
        return new(150f, 0.3f + 0.07f * tremble, 320f, 0.1f);
    }

    static void Ask(Controller.RumbleWave? wave, string phase)
    {
        _phase = phase;
        // An unchanged wave keeps its record, so the host resends it on its own clock
        // instead of taking it as a new moment.
        if (wave == Controller.Rumble) return;
        Controller.Rumble = wave;
        if (wave != null) _waves++;
    }

    static void Probe(double now)
    {
        if (now - _probeAt < 1000) return;
        _probeAt = now;
        Console.WriteLine($"[{Game.Tag}] rumble: {(Enabled ? "on" : "off")}, HD {(Controller.HdRumble ? "on" : Hd ? "asked, pad cannot" : "off")}, " +
                          $"{_phase}, tension {_tension:0.00}, waves asked {_waves}");
        _waves = 0;
    }
}
