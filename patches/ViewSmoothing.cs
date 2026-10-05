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

    /// <summary>Set by another carrier before the renderer runs: the view to draw instead.</summary>
    public static Func<Camera, double, Camera>? Lead;

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
            if (Lead == null) return;
            var led = Lead(handed, 1.0);
            if (led == handed) return;
            Draw(m, handed, led);
            return;
        }

        long tick = FramePacing.Ticks;
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
            if (Jump(_prev, _cur)) { _prev = _cur; _snaps++; }
        }
        else if (handed != _cur)
        {
            // Moved without a tick of the world: no pair to carry.
            _prev = _cur = handed;
            _snaps++;
        }

        double frac = FramePacing.TickFraction;
        var view = Lerp(_prev, _cur, frac);
        if (Lead != null) view = Lead(view, frac);
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
