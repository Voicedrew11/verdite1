using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>A camera: a VECTOR position and an SVECTOR rotation, 0x1000 a turn.</summary>
public readonly record struct Camera(int X, int Y, int Z, short Pitch, short Yaw, short Roll)
{
    public static Camera Read(IMemory m, uint pos, uint rot) => new(
        (int)m.ReadU32(pos), (int)m.ReadU32(pos + 4u), (int)m.ReadU32(pos + 8u),
        (short)m.ReadU16(rot), (short)m.ReadU16(rot + 2u), (short)m.ReadU16(rot + 4u));

    public void Write(IMemory m, uint pos, uint rot)
    {
        m.WriteU32(pos, (uint)X); m.WriteU32(pos + 4u, (uint)Y); m.WriteU32(pos + 8u, (uint)Z);
        m.WriteU16(rot, (ushort)Pitch); m.WriteU16(rot + 2u, (ushort)Yaw); m.WriteU16(rot + 4u, (ushort)Roll);
    }
}

/// <summary>
/// The camera carried between world ticks: each frame the main loop draws, the
/// renderer is handed the camera interpolated between the last two ticks at the
/// clock's fraction, and the tick's camera is put back after it.
///
///     KF1_SMOOTH=1         on whenever pacing is; 0 to compare
///     KF1_SMOOTH_PROBE=1   a line a second: frames drawn, distinct cameras drawn, ticks, snaps
///
/// The renderer (stage I) takes the camera as two pointers, the main loop's camera
/// blocks 0x800650A0 and 0x80065098, which only the main loop names; only renderer
/// code reads the copy it keeps. Interpolates, never extrapolates; a jump larger
/// than <see cref="SnapUnits"/> or <see cref="SnapAngle"/> in one tick snaps, and an
/// overlay load re-primes. Verdite3's ViewSmoothing on this game's renderer. See
/// "The camera carried" in docs/SMOOTHING.md.
/// </summary>
public static class ViewSmoothing
{
    const uint Renderer = 0x8001FDE4;
    const uint MainSite = 0x800148C0 + 8;   // the main loop's call of the renderer
    public const uint CamPos = 0x800650A0;
    public const uint CamRot = 0x80065098;

    /// <summary>A position step in one tick past which the view snaps (a warp, a load).</summary>
    public const int SnapUnits = 4000;

    /// <summary>An angle step in one tick past which the view snaps (a cut), 0x1000 a turn.</summary>
    public const int SnapAngle = 0x300;

    public static bool Enabled { get; set; } = true;

    /// <summary>Carrying now: on, under pacing.</summary>
    public static bool Active => Enabled && FramePacing.Enabled;

    public static bool ProbeOn { get => _probe; set => _probe = value; }

    static readonly ModInfo _self = new() { Id = "kf1.viewsmoothing", Name = "View smoothing", Version = "1.0" };

    static Camera _prev, _cur, _drawn, _handed, _probed;
    static bool _primed, _restore;
    static long _tick = -1;

    static bool _probe;
    static double _probeAt = -1.0;
    static long _frames, _moved, _samples, _snaps;

    /// <summary>The camera the last main-loop frame was drawn with.</summary>
    public static Camera Drawn => _drawn;

    public static void Configure(string? mode, string? probe)
    {
        Enabled = mode?.Trim() is not ("0" or "off");
        _probe = probe?.Trim() == "1";
    }

    public static void Install()
    {
        Event.AddListener<OverlayLoadedEvent>(_ => _primed = false);
        HookAttach.OnOverlayLoad("view smoothing", () =>
        {
            SymbolRegistry.Build();
            if (SymbolRegistry.Resolve("game", null, Renderer) is not { } t) return false;
            var self = typeof(ViewSmoothing);
            HookManager.AddPre(_self, t, self.GetMethod(nameof(BeforeRenderer), BindingFlags.Public | BindingFlags.Static)!);
            HookManager.AddPost(_self, t, self.GetMethod(nameof(AfterRenderer), BindingFlags.Public | BindingFlags.Static)!);
            HookManager.Commit();
            return HookAttach.Installed(t);
        });
        Console.WriteLine($"[KF1] view smoothing: {(Enabled ? "on" : "off")}");
    }

    public static void BeforeRenderer(CpuContext c, IMemory m)
    {
        _restore = false;
        if (c.RA != MainSite || c.A0 != CamPos || c.A1 != CamRot) return;
        var handed = Camera.Read(m, CamPos, CamRot);
        if (!Active)
        {
            _primed = false;
            // Carrying off under pacing, the mouse still leads: the tick's view plus
            // what the hand has moved since.
            if (Leading && Lead(m, handed, 1.0, false) is { } led && led != handed) Draw(m, handed, led);
            return;
        }

        long tick = FramePacing.Ticks;
        bool ticked = false, snapped = false;
        if (!_primed)
        {
            _prev = _cur = handed;
            _primed = true;
            _tick = tick;
        }
        else if (tick != _tick && FramePacing.IterationTicked)
        {
            _tick = tick;
            _prev = _cur;
            _cur = handed;
            _samples++;
            ticked = true;
            if (Jump(_prev, _cur)) { _prev = _cur; _snaps++; snapped = true; }
        }
        else if (handed != _cur)
        {
            // Moved without a tick of the world: no pair to carry.
            _prev = _cur = handed;
            _snaps++;
            snapped = true;
        }

        double frac = FramePacing.TickFraction;
        var view = Lerp(_prev, _cur, frac);
        if (Leading)
        {
            if (snapped) _tickYaw = _tickPitch = 0;
            else if (ticked) TakeSpent(m);
            view = Lead(m, view, frac, true) ?? view;
        }
        Draw(m, handed, view);
        if (_probe) Probe(view);
    }

    static void Draw(IMemory m, in Camera handed, in Camera view)
    {
        _handed = handed;
        _drawn = view;
        view.Write(m, CamPos, CamRot);
        _restore = true;
    }

    public static void AfterRenderer(CpuContext c, IMemory m)
    {
        if (!_restore) return;
        _restore = false;
        _handed.Write(m, CamPos, CamRot);
    }

    // ---- the mouse leads the tick ------------------------------------------------
    //
    // Stage A spends the mouse once a tick, and the lerp reaches that turn only at
    // the next tick: up to two ticks from hand to picture. A mouse asks for a
    // displacement the game adds unchanged (patches/MouseLook.cs), so the view can
    // show it the frame it happens: the lerp's share of the last tick's mouse turn
    // is replaced by all of it, and the motion not yet spent is added on top.
    // Verdite3's ViewSmoothing lead; see "The mouse leads the tick" in docs/INPUT.md.

    static bool Leading => Mouse.Lead && FramePacing.Enabled;
    static int _tickYaw, _tickPitch;

    static void TakeSpent(IMemory m)
    {
        var spent = Mouse.SpentThisFrame(m);
        (_tickYaw, _tickPitch) = spent is { } t ? (t.Yaw, t.Pitch) : (0, 0);
    }

    /// <summary>The view with the mouse's lead added, or null when there is none.
    /// <paramref name="frac"/> is the lerp's phase, 1 when nothing is carried.</summary>
    static Camera? Lead(IMemory m, Camera view, double frac, bool carried)
    {
        Mouse.Poll();
        var (turn, look) = Mouse.Pending;

        double keep = 1.0 - frac;
        double yaw = (carried ? _tickYaw * keep : 0) + turn;

        // Held inside the game's pitch limit, off the angle the next tick adds to.
        int basePitch = (short)m.ReadU16(MouseLook.Game.PitchAddress);
        int ahead = Math.Clamp(basePitch + (int)Math.Round(look), -Mouse.PitchLimit, Mouse.PitchLimit) - basePitch;
        double pitch = (carried ? _tickPitch * keep : 0) + ahead;

        int dy = (int)Math.Round(yaw), dp = (int)Math.Round(pitch);
        if (dy == 0 && dp == 0) return carried ? view : null;
        // Yaw stays in 0..0xFFF; pitch is a signed angle here, not a 12-bit one.
        return view with
        {
            Yaw = (uint)view.Yaw < 0x1000u ? (short)((view.Yaw + dy) & 0xFFF) : (short)(view.Yaw + dy),
            Pitch = (short)Math.Clamp(view.Pitch + dp, -Mouse.PitchLimit, Mouse.PitchLimit),
        };
    }

    static bool Jump(in Camera a, in Camera b) =>
        Math.Abs((long)b.X - a.X) > SnapUnits || Math.Abs((long)b.Y - a.Y) > SnapUnits ||
        Math.Abs((long)b.Z - a.Z) > SnapUnits ||
        Math.Abs(Wrap(b.Pitch - a.Pitch)) > SnapAngle || Math.Abs(Wrap(b.Yaw - a.Yaw)) > SnapAngle ||
        Math.Abs(Wrap(b.Roll - a.Roll)) > SnapAngle;

    internal static int Wrap(int d) => ((d + 0x800) & 0xFFF) - 0x800;

    static Camera Lerp(in Camera a, in Camera b, double t) => new(
        Mix(a.X, b.X, t), Mix(a.Y, b.Y, t), Mix(a.Z, b.Z, t),
        Turn(a.Pitch, b.Pitch, t), Turn(a.Yaw, b.Yaw, t), Turn(a.Roll, b.Roll, t));

    internal static int Mix(int a, int b, double t) => (int)(a + Math.Round(((long)b - a) * t));

    /// <summary>The short way round at 12 bits, kept in 0..0xFFF when both ends are:
    /// the map walk picks its stencil by the yaw's high byte.</summary>
    internal static short Turn(short a, short b, double t)
    {
        int v = a + (int)Math.Round(Wrap(b - a) * t);
        if ((uint)a < 0x1000u && (uint)b < 0x1000u) v &= 0xFFF;
        return (short)v;
    }

    static void Probe(in Camera view)
    {
        _frames++;
        if (view != _probed) _moved++;
        _probed = view;
        double now = Environment.TickCount64 / 1000.0;
        if (_probeAt < 0.0) _probeAt = now;
        double dt = now - _probeAt;
        if (dt < 1.0) return;
        Console.WriteLine($"[KF1] view smoothing: {_frames / dt:0.0} frame(s)/s, {_moved / dt:0.0} with a new camera, " +
                          $"{_samples / dt:0.0} tick sample(s)/s, {_snaps} snap(s); " +
                          $"drawn [{view.X},{view.Y},{view.Z}] yaw {view.Yaw}, handed [{_cur.X},{_cur.Y},{_cur.Z}] yaw {_cur.Yaw}");
        _probeAt = now;
        _frames = _moved = _samples = _snaps = 0;
    }
}
