using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// The view, carried between ticks: what makes <see cref="GateRedraw"/>'s extra
/// frames worth drawing.
///
/// King's Field's renderer (<c>func_8001FDE4</c>) takes a camera position (a
/// VECTOR) in <c>a0</c> and a rotation (an SVECTOR, pitch/yaw/roll, 12-bit) in
/// <c>a1</c>, and its camera block <c>func_8001C184</c> copies a non-null one into
/// its own store -- position at <c>0x80095744</c> (x, y, z), rotation at
/// <c>0x80095754</c> -- with the map cell the position lies in (x/2000, z/2000) at
/// <c>0x8009575C</c>/<c>5E</c>, and builds its matrices from the store. A null
/// argument keeps what is stored. The main loop passes <c>0x800650A0</c> and
/// <c>0x80065098</c>, which stage B fills from the player (<c>0x800A0824</c>,
/// <c>0x800A0838</c>).
///
/// Around each main-loop render and each redraw, this writes
/// <c>lerp(prev, cur, phase)</c> into the store and calls the renderer with null
/// arguments, then puts back what the game's own call would have left there. It
/// interpolates between the last two ticks the game produced, as King's Field
/// II's FrameSmoothing does and for the same reason ("The view has to be carried
/// between ticks" in docs/PATCHES_AND_MODS.md): an extrapolated turn overshoots
/// when the game damps it, and snaps back. The cost is one tick of display
/// latency (50 ms). The carried view lives for one renderer call and nothing
/// else reads the store, so it cannot reach the logic, the collision or a save.
///
///     KF2_SMOOTH=0         leave the view at the tick
///     KF2_SMOOTH_PROBE=1   a line a second: frames carried, mean phase, step sizes
/// </summary>
public static class ViewCarry
{
    const uint StorePos = 0x80095744, StoreRot = 0x80095754, StoreCell = 0x8009575C;
    const uint MainLoopRenderReturn = 0x800148C8;
    const int CellSize = 2000;

    /// <summary>A step longer than this on any axis in one tick is a warp (a
    /// door, a load, a teleport), not a walk, and is not interpolated.</summary>
    const int WarpUnits = 4000;

    /// <summary>A gap this long between two main-loop renders means the loop was
    /// away (a menu, a load); the sample before it is not the previous tick.</summary>
    const double StaleMs = 250.0;

    public const string OnKey = "kf1.smooth.on";

    static readonly ModInfo _self = new()
    {
        Id = "kf1.viewcarry",
        Name = "View smoothing",
        Version = "1.0",
        Description = "Moves the camera between ticks for frames drawn between them.",
    };

    static bool? _forced;
    static bool _probe;
    public static bool Enabled { get; private set; } = true;

    struct Cam
    {
        public int X, Y, Z;
        public short Pitch, Yaw, Roll;
    }

    static Cam _prev, _cur;
    static bool _have;
    static double _lastSampleMs = double.NegativeInfinity;

    /// <summary>For the length of a renderer call: whether this picture is being
    /// carried, whether it is a tick's first, and how far across the tick it is.
    /// <see cref="ObjectCarry"/> reads these inside the call.</summary>
    public static bool Carrying { get; private set; }
    public static bool TickRender { get; private set; }
    public static double Phase { get; private set; }

    // What the store must hold when the renderer returns.
    static bool _restore;
    static Cam _true;

    // probe
    static long _carried, _renders;
    static double _phaseSum, _yawStepSum, _posStepSum;
    static double _windowStart;

    public static void Configure(string? on, string? probe)
    {
        if (!string.IsNullOrWhiteSpace(on)) _forced = on.Trim() != "0";
        _probe = !string.IsNullOrWhiteSpace(probe) && probe.Trim() != "0";
    }

    public static void Install()
    {
        if (_forced is { } f) Enabled = f;
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            Enabled = _forced ?? RecompOne.Runtime.Runtime.View.GetBool(OnKey, true);
            Console.WriteLine($"[KF1] view smoothing: {(Enabled ? "on" : "off")}");
        });
        Event.AddListener<OverlayLoadedEvent>(_ => _have = false);
        HookAttach.OnOverlayLoad("view carry", Attach);
    }

    public static void SetEnabled(bool on)
    {
        if (on && !Enabled) _have = false;
        Enabled = on;
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var target = SymbolRegistry.Resolve("game", null, GateRedraw.Renderer);
        if (target == null) return false;
        const BindingFlags f = BindingFlags.Public | BindingFlags.Static;
        HookManager.AddPre(_self, target, typeof(ViewCarry).GetMethod(nameof(Before), f)!);
        HookManager.AddPost(_self, target, typeof(ViewCarry).GetMethod(nameof(After), f)!);
        HookManager.Commit();
        return HookAttach.Installed(target);
    }

    static Cam Read(IMemory m, uint pos, uint rot) => new()
    {
        X = (int)m.ReadU32(pos), Y = (int)m.ReadU32(pos + 4), Z = (int)m.ReadU32(pos + 8),
        Pitch = (short)m.ReadU16(rot), Yaw = (short)m.ReadU16(rot + 2), Roll = (short)m.ReadU16(rot + 4),
    };

    static void Write(IMemory m, in Cam v)
    {
        m.WriteU32(StorePos, (uint)v.X);
        m.WriteU32(StorePos + 4, (uint)v.Y);
        m.WriteU32(StorePos + 8, (uint)v.Z);
        m.WriteU16(StoreRot, (ushort)v.Pitch);
        m.WriteU16(StoreRot + 2, (ushort)v.Yaw);
        m.WriteU16(StoreRot + 4, (ushort)v.Roll);
        // The cell the camera block derives from the position, the way it does:
        // MIPS div truncates towards zero, as C# does.
        m.WriteU16(StoreCell, (ushort)(short)(v.X / CellSize));
        m.WriteU16(StoreCell + 2, (ushort)(short)(v.Z / CellSize));
    }

    /// <summary>The shortest way round, in the angle's own 12-bit circle.</summary>
    static int AngleStep(short from, short to) => ((to - from + 2048) & 4095) - 2048;

    static short LerpAngle(short from, short to, double t) =>
        (short)(ushort)(from + (int)Math.Round(AngleStep(from, to) * t));

    static int Lerp(int a, int b, double t) => a + (int)Math.Round((b - a) * t);

    static bool Warped(in Cam a, in Cam b) =>
        Math.Abs((long)b.X - a.X) > WarpUnits || Math.Abs((long)b.Y - a.Y) > WarpUnits ||
        Math.Abs((long)b.Z - a.Z) > WarpUnits;

    public static void Before(CpuContext c, IMemory m)
    {
        bool mainLoop = c.RA == MainLoopRenderReturn;
        GateRedraw.NoteRender(mainLoop);
        _restore = false;
        Carrying = false;
        if (!Enabled || !GateRedraw.Redrawing) return;
        if (!mainLoop && !GateRedraw.InRedraw) return;

        // The camera this call would draw from, had it been left alone.
        uint pos = c.A0 != 0 ? c.A0 : StorePos;
        uint rot = c.A1 != 0 ? c.A1 : StoreRot;
        var truth = Read(m, pos, rot);

        double now = Interrupts.ClockMs;
        if (!GateRedraw.InRedraw)
        {
            // A tick: the world has advanced, and this is its first picture.
            bool fresh = !_have || now - _lastSampleMs > StaleMs || Warped(_cur, truth);
            _prev = fresh ? truth : _cur;
            _cur = truth;
            _have = true;
            _lastSampleMs = now;
        }
        if (!_have) return;

        double phase = Math.Clamp((now - GateRedraw.TickStartMs) / GateRedraw.TickMs, 0.0, 1.0);
        var carried = new Cam
        {
            X = Lerp(_prev.X, _cur.X, phase),
            Y = Lerp(_prev.Y, _cur.Y, phase),
            Z = Lerp(_prev.Z, _cur.Z, phase),
            Pitch = LerpAngle(_prev.Pitch, _cur.Pitch, phase),
            Yaw = LerpAngle(_prev.Yaw, _cur.Yaw, phase),
            Roll = LerpAngle(_prev.Roll, _cur.Roll, phase),
        };

        Write(m, carried);
        c.A0 = 0;
        c.A1 = 0;
        _true = truth;
        _restore = true;
        Carrying = true;
        TickRender = !GateRedraw.InRedraw;
        Phase = phase;

        if (_probe)
        {
            _carried++;
            _phaseSum += phase;
            _yawStepSum += Math.Abs(AngleStep(_prev.Yaw, _cur.Yaw));
            _posStepSum += Math.Sqrt(Math.Pow(_cur.X - _prev.X, 2) + Math.Pow(_cur.Z - _prev.Z, 2));
        }
    }

    public static void After(CpuContext c, IMemory m)
    {
        _renders++;
        Carrying = false;
        if (_restore)
        {
            // What the game's own call leaves in the store: its camera, not ours.
            Write(m, _true);
            _restore = false;
        }
        if (_probe) Report();
    }

    static void Report()
    {
        double now = Environment.TickCount64 / 1000.0;
        if (_windowStart == 0) { _windowStart = now; return; }
        double secs = now - _windowStart;
        if (secs < 1.0) return;
        Console.WriteLine($"[KF1] smooth: {_carried}/{_renders} renders carried, " +
                          $"mean phase {(_carried == 0 ? 0 : _phaseSum / _carried):0.00}, " +
                          $"yaw step {(_carried == 0 ? 0 : _yawStepSum / _carried):0.0} u/tick, " +
                          $"move {(_carried == 0 ? 0 : _posStepSum / _carried):0} u/tick");
        _carried = _renders = 0;
        _phaseSum = _yawStepSum = _posStepSum = 0;
        _windowStart = now;
    }
}
