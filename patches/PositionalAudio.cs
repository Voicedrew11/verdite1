using System.Diagnostics;
using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// Positional sound effects: a creature, a door or a spell is heard from where it
/// is for as long as it plays, and on headphones from behind as well as in front.
/// The DSP is the runtime's <c>SpuSpatialVoice</c>; this finds the voices and says
/// where they are. See "Positional audio" in docs/AUDIO.md.
///
///     KF2_POSAUDIO=off|speakers|headphones
///     KF2_POSAUDIO_PROBE=1
///
/// ## What the game does
///
/// <c>func_80013D08(se | flags, &amp;pos, vol, maxDist, fade, noteAdj)</c> is the
/// game's 3D sound: distance against the listener stage 9 stores at
/// <see cref="ListenerPos"/>, a linear fade to <c>fade</c>, halved when the source
/// is on the other half of a stacked map, and an equal-power pan from the angle to
/// the listener's yaw -- **folded**, so a sound behind is panned exactly as the same
/// sound in front. The pan is computed once, into the <c>SsUtKeyOn</c> volumes, and
/// never again: turn round while a sound plays and it stays where it started.
///
/// ## What this does
///
/// Pre <c>func_80013D08</c> keeps the source; pre and post <c>SsUtKeyOn</c> inside
/// it learn the voice, and tag that voice's *next key-on* (it may be written inside
/// the call or at the next sequencer flush -- either way it is the one after the
/// count read before the call). Post stage 9, every live tag is re-aimed from the
/// listener that frame, with the game's own distance formula. A voice re-keyed by
/// anything else loses its tag, so music is never touched.
///
/// The level stays the game's: the mixer scales every target by the registers'
/// magnitude over what the game asked for at key-on, which carries the VAB's own
/// volumes without the port having to know them.
/// </summary>
public static class PositionalAudio
{
    public enum Mode { Off, Speakers, Headphones }

    public const string ModeKey = "kf2.audio.positional";

    const uint Sound3D = 0x80013D08;
    const uint SsUtKeyOn = 0x8005520C;
    const uint ListenerStage = 0x800140AC;

    // Written by stage 9 (func_800140AC): eye position, and the angle block whose
    // second word is the yaw func_80013D08 reads.
    const uint ListenerPos = 0x80198584;
    const uint ListenerYaw = 0x80198598;

    // func_8002B6B4 leaves the half of a stacked map a position is on here; stage 9
    // copies the listener's to ListenerHalf, func_80013D08 compares the source's.
    const uint HalfNow = 0x801E9C8E;
    const uint ListenerHalf = 0x80198594;

    // The game's pan: sin/cos of the angle over 0xD48, so centre is 0.852 per ear.
    const float PanScale = 4096f / 3400f;
    static readonly float CentreLevel = MathF.Sin(MathF.PI / 4) * PanScale;

    // Headphones. Spherical head (Brown & Duda): radius 8.75 cm, alpha_min 0.1 at 150 degrees.
    static readonly float ItdSamples = 0.0875f / 343f * 44100f;
    const float Ild = 0.5f;          // broadband: +/- 4.8 dB at the side, constant power
    const float RearDepth = 0.5f;    // how much of the rear low-pass a source straight behind gets
    const float Near = 0x400;        // closer than half a tile the direction fades to straight ahead

    static Mode? _env;
    public static Mode Current { get; private set; }
    public static bool Probe;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.positionalaudio",
        Name = "Positional audio",
        Version = "1.0",
        Description = "Keeps a sound effect at its source while it plays, and renders it for speakers or headphones.",
    };

    sealed class Tag
    {
        public bool Live;
        public int Serial;
        public int X, Y, Z, Vol, Fade;
        public bool Halve;
        public float Sum;
    }

    static readonly Tag[] _tags = Enumerable.Range(0, 24).Select(_ => new Tag()).ToArray();
    static readonly int[] _serials = new int[24];
    static readonly int[] _before = new int[24];

    // The call in progress. Hooks run on the game thread, and func_80013D08 does not nest.
    static bool _inSound, _inKeyOn;
    static int _sx, _sy, _sz, _sVol, _sFade, _sSe, _sFlags, _gameL, _gameR;

    static long _tagged, _keyedInCall, _deferred, _noVoice, _dropped;
    static int _details;
    static long _lastProbe = Stopwatch.GetTimestamp();

    public static void Configure(string? mode, string? probe)
    {
        if (!string.IsNullOrWhiteSpace(mode))
        {
            _env = mode.Trim().ToLowerInvariant() switch
            {
                "0" or "off" => Mode.Off,
                "1" or "speakers" or "stereo" => Mode.Speakers,
                "2" or "headphones" or "binaural" => Mode.Headphones,
                _ => null
            };
            if (_env == null)
                Console.Error.WriteLine($"[KF2] positional audio: KF2_POSAUDIO=\"{mode}\" is not off, speakers or headphones");
        }

        Probe = probe is "1";
    }

    /// <summary>Whether Program.cs installed it; the Audio page hides the control
    /// otherwise. It hooks King's Field II's sound routines and is not ported.</summary>
    public static bool Installed { get; private set; }

    public static void Install()
    {
        Installed = true;
        Current = _env ?? Mode.Headphones;
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            Set(_env ?? (Mode)Math.Clamp(RecompOne.Runtime.Runtime.View.GetInt(ModeKey, 2), 0, 2));
            Console.WriteLine($"[KF2] positional audio: {Current}");
        });

        HookAttach.OnOverlayLoad("positional audio", Attach);
    }

    public static void Set(Mode mode)
    {
        Current = mode;
        if (mode != Mode.Off) return;
        var spu = RecompOne.Runtime.Runtime.Spu;
        for (var v = 0; v < 24; v++)
        {
            if (!_tags[v].Live) continue;
            _tags[v].Live = false;
            spu?.SetSpatial(v, -1, 0, 0, 0, 0, 0, 1, 1, 0);
        }
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var sound = SymbolRegistry.Resolve("game", null, Sound3D);
        var keyOn = SymbolRegistry.Resolve("game", null, SsUtKeyOn);
        var stage = SymbolRegistry.Resolve("game", null, ListenerStage);
        if (sound == null || keyOn == null || stage == null)
        {
            Console.Error.WriteLine("[KF2] positional audio: func_80013D08, SsUtKeyOn or func_800140AC did not resolve");
            return false;
        }

        MethodInfo M(string name) => typeof(PositionalAudio).GetMethod(name, BindingFlags.Public | BindingFlags.Static)!;
        HookManager.AddPre(_self, sound, M(nameof(BeforeSound)));
        HookManager.AddPost(_self, sound, M(nameof(AfterSound)));
        HookManager.AddPre(_self, keyOn, M(nameof(BeforeKeyOn)));
        HookManager.AddPost(_self, keyOn, M(nameof(AfterKeyOn)));
        HookManager.AddPost(_self, stage, M(nameof(AfterListener)));
        HookManager.Commit();

        var ok = HookAttach.Installed(sound) && HookAttach.Installed(keyOn) && HookAttach.Installed(stage);
        Console.WriteLine($"[KF2] positional audio: hooks {(ok ? "attached" : "INCOMPLETE")}" + (Probe ? ", probe on" : ""));
        return ok;
    }

    public static void BeforeSound(CpuContext c, IMemory m)
    {
        if (Current == Mode.Off) return;
        _inSound = true;
        _sSe = (int)(c.A0 & 0xFFF);
        _sFlags = (int)(c.A0 & 0x8000);
        _sx = (int)m.ReadU32(c.A1);
        _sy = (int)m.ReadU32(c.A1 + 4);
        _sz = (int)m.ReadU32(c.A1 + 8);
        _sVol = (int)c.A2;
        _sFade = (int)m.ReadU32(c.SP + 0x10);
    }

    public static void AfterSound(CpuContext c, IMemory m)
    {
        _inSound = false;
        _inKeyOn = false;
    }

    public static void BeforeKeyOn(CpuContext c, IMemory m)
    {
        if (!_inSound || RecompOne.Runtime.Runtime.Spu is not { } spu) return;
        _inKeyOn = true;
        spu.CopyKeyOnSerials(_before);
        _gameL = (short)m.ReadU32(c.SP + 0x14);
        _gameR = (short)m.ReadU32(c.SP + 0x18);
    }

    public static void AfterKeyOn(CpuContext c, IMemory m)
    {
        if (!_inKeyOn) return;
        _inKeyOn = false;
        if (RecompOne.Runtime.Runtime.Spu is not { } spu) return;

        var v = (int)c.V0;
        if ((uint)v >= 24 || _gameL + _gameR <= 0)
        {
            _noVoice++;
            return;
        }

        spu.CopyKeyOnSerials(_serials);
        if (_serials[v] != _before[v]) _keyedInCall++;
        else _deferred++;

        var t = _tags[v];
        t.Live = true;
        t.Serial = _before[v] + 1;
        t.X = _sx;
        t.Y = _sy;
        t.Z = _sz;
        t.Vol = _sVol;
        t.Fade = _sFade;
        t.Halve = m.ReadU16(HalfNow) != m.ReadU16(ListenerHalf);
        t.Sum = _gameL + _gameR;
        _tagged++;

        var (left, fwd, dist) = Aim(t, spu, m, v);
        if (Probe && _details < 24)
        {
            _details++;
            Console.WriteLine($"[KF2] positional audio: se 0x{_sSe:X3}{(_sFlags != 0 ? " flag 8000" : "")} voice {v} " +
                              $"dist {dist:F0} fade {_sFade} vol {_sVol}{(t.Halve ? " halved" : "")} -- " +
                              $"game L {_gameL} R {_gameR}, port left {Signed(left)} ahead {Signed(fwd)}");
        }
    }

    public static void AfterListener(CpuContext c, IMemory m)
    {
        if (RecompOne.Runtime.Runtime.Spu is not { } spu) return;

        if (Current != Mode.Off)
        {
            spu.CopyKeyOnSerials(_serials);
            for (var v = 0; v < 24; v++)
            {
                var t = _tags[v];
                if (!t.Live) continue;
                if (_serials[v] > t.Serial)
                {
                    t.Live = false;
                    _dropped++;
                    continue;
                }

                Aim(t, spu, m, v);
            }
        }

        if (Probe) Report(spu);
    }

    /// <summary>Re-aim one tag from this frame's listener, and hand the mixer its targets.
    /// Returns the direction the probe prints.</summary>
    static (float left, float ahead, float dist) Aim(Tag t, Spu spu, IMemory m, int voice)
    {
        float dx = t.X - (int)m.ReadU32(ListenerPos);
        float dy = t.Y - (int)m.ReadU32(ListenerPos + 4);
        float dz = t.Z - (int)m.ReadU32(ListenerPos + 8);
        var yaw = (short)m.ReadU16(ListenerYaw);

        var h = MathF.Sqrt(dx * dx + dz * dz);
        var d = MathF.Sqrt(h * h + dy * dy);

        // The game's distance law, evaluated every frame instead of once.
        var gain = t.Fade > 0 ? Math.Max(0f, t.Fade - d) * 128f / t.Fade : 0f;
        gain = Math.Min(127f, gain * t.Vol / (t.Halve ? 256f : 128f));

        // func_80015394 is atan2(-dx, dz) in 4096ths of a turn; relative to the yaw,
        // 0 is ahead and a quarter turn is the side the game pans left.
        var rel = MathF.Atan2(-dx, dz) - yaw * (MathF.Tau / 4096f);
        float left = 0, ahead = 1, up = 0;
        if (d >= 1)
        {
            left = MathF.Sin(rel) * h / d;
            ahead = MathF.Cos(rel) * h / d;
            up = -dy / d;
        }

        var w = Math.Clamp(d / Near, 0f, 1f);
        left *= w;
        up *= w;
        ahead = ahead * w + (1 - w);

        var rear = RearDepth * Math.Max(0f, -ahead);

        if (Current == Mode.Speakers)
        {
            // The game's own equal-power law, unfolded from behind and kept up to date.
            var az = MathF.Atan2(left, MathF.Sqrt(ahead * ahead + up * up));
            var p = MathF.PI / 4 + az / 2;
            var l = Math.Min(127f, gain * MathF.Sin(p) * PanScale);
            var r = Math.Min(127f, gain * MathF.Cos(p) * PanScale);
            spu.SetSpatial(voice, t.Serial, t.Sum, l, r, 0, 0, 1, 1, rear);
        }
        else
        {
            var s = Math.Clamp(left, -1f, 1f);
            var level = gain * CentreLevel;
            float thetaL = MathF.Acos(s), thetaR = MathF.Acos(-s);
            spu.SetSpatial(voice, t.Serial, t.Sum,
                level * MathF.Sqrt(1 + Ild * s), level * MathF.Sqrt(1 - Ild * s),
                Itd(thetaL), Itd(thetaR), Shelf(thetaL), Shelf(thetaR), rear);
        }

        return (left, ahead, d);
    }

    // Angle from the ear's own axis: 0 is the source beside that ear.
    static float Shelf(float theta) => 1.05f + 0.95f * MathF.Cos(theta * 1.2f);

    static float Itd(float theta) => ItdSamples * (theta < MathF.PI / 2 ? 1 - MathF.Cos(theta) : theta - MathF.PI / 2 + 1);

    static string Signed(float x) => MathF.Round(x, 2) is var r && r > 0 ? $"+{r:F2}" : r < 0 ? $"{r:F2}" : "0.00";

    static void Report(Spu spu)
    {
        var now = Stopwatch.GetTimestamp();
        if (now - _lastProbe < Stopwatch.Frequency) return;
        _lastProbe = now;

        var live = _tags.Count(t => t.Live);
        var peak = spu.Stats.SpatialVoicesPeak;
        spu.Stats.SpatialVoicesPeak = 0;
        if (_tagged + _noVoice + _dropped + live + peak == 0) return;
        Console.WriteLine($"[KF2] positional audio: {Current}, tagged {_tagged} (keyed in call {_keyedInCall}, " +
                          $"deferred {_deferred}, no voice {_noVoice}), dropped {_dropped}, live {live}, mixer peak {peak}");
        _tagged = _keyedInCall = _deferred = _noVoice = _dropped = 0;
    }
}
