using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// Creatures and objects, carried between ticks with the view.
///
/// King's Field's model walk (<c>func_8001F218</c>) runs five tables through a
/// visible-cell test and hands each live entry to its submitter. Two hold things
/// that move, in world units:
///
/// | table | base | entries | stride | live | position (int32 x,y,z) | rotation (s16) | submitter |
/// |---|---|---|---|---|---|---|---|
/// | creatures | <c>0x8009D040</c> | 48 | 60 | <c>+0 != 0xFF</c>, <c>+3 != 0xFF</c> | <c>+0x0C</c> | <c>+0x1C</c> | <c>func_8001EEDC</c> |
/// | objects | <c>0x8006C4B8</c> | 128 | 72 | <c>+6 == 1</c> | <c>+0x1C</c> | <c>+0x2C</c> | <c>func_8001E9A4</c> |
///
/// The submitters build each model's matrix from those fields relative to the
/// renderer's camera store, so writing <c>lerp(prev, cur, phase)</c> into the
/// entries for the length of the walk moves the models between ticks exactly as
/// <see cref="ViewCarry"/> moves the view, and the true values go back before the
/// renderer returns. Samples are taken on each tick's first picture; a slot whose
/// model changed, that was not live at the last tick, or that moved further than
/// <see cref="WarpUnits"/> in one tick is drawn where it is. See "Creatures and
/// objects" in docs/KF1.md.
///
///     KF2_SMOOTH_OBJECTS=0         leave them at the tick
///     KF2_SMOOTH_OBJECTS_PROBE=1   a line a second: live slots and carries
/// </summary>
public static class ObjectCarry
{
    public const uint ModelWalk = 0x8001F218;
    const int WarpUnits = 2000;

    sealed record Table(string Name, uint Base, int Count, int Stride, int Pos, int Rot, Func<IMemory, uint, bool> Live, int Kind);

    static readonly Table[] Tables =
    [
        new("creatures", 0x8009D040, 48, 60, 0x0C, 0x1C,
            (m, e) => m.ReadU8(e) != 0xFF && m.ReadU8(e + 3) != 0xFF, 3),
        new("objects", 0x8006C4B8, 128, 72, 0x1C, 0x2C,
            (m, e) => m.ReadU8(e + 6) == 1, 0),
    ];

    struct Pose
    {
        public int X, Y, Z;
        public short Rx, Ry, Rz;
        public byte Kind;
        public bool Live;
        public byte Clip;       // creatures: +4, the animation (0xFF: none)
        public ushort Time;     // creatures: +8, the animation's time in ticks
    }

    static readonly Pose[][] Prev = Tables.Select(t => new Pose[t.Count]).ToArray();
    static readonly Pose[][] Cur = Tables.Select(t => new Pose[t.Count]).ToArray();
    static readonly Pose[][] Truth = Tables.Select(t => new Pose[t.Count]).ToArray();
    static readonly bool[][] Written = Tables.Select(t => new bool[t.Count]).ToArray();

    static readonly ModInfo _self = new()
    {
        Id = "kf1.objectcarry",
        Name = "Object smoothing",
        Version = "1.0",
        Description = "Moves creatures and objects between ticks for frames drawn between them.",
    };

    static bool? _forced;
    static bool _probe, _probe2;
    public static bool Enabled { get; private set; } = true;
    public const string OnKey = "kf1.smooth.objects";

    static long _walks, _carried, _live, _warps, _dbg;
    static double _windowStart;

    public static void Configure(string? on, string? probe)
    {
        if (!string.IsNullOrWhiteSpace(on)) _forced = on.Trim() != "0";
        _probe = !string.IsNullOrWhiteSpace(probe) && probe.Trim() != "0";
        _probe2 = probe?.Trim() == "2";
    }

    public static void Install()
    {
        if (_forced is { } f) Enabled = f;
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            Enabled = _forced ?? RecompOne.Runtime.Runtime.View.GetBool(OnKey, true);
            Console.WriteLine($"[KF1] object smoothing: {(Enabled ? "on" : "off")}");
        });
        Event.AddListener<OverlayLoadedEvent>(_ => Forget());
        HookAttach.OnOverlayLoad("object carry", Attach);
    }

    public static void SetEnabled(bool on)
    {
        if (on && !Enabled) Forget();
        Enabled = on;
    }

    static void Forget()
    {
        foreach (var a in Prev) Array.Clear(a);
        foreach (var a in Cur) Array.Clear(a);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var target = SymbolRegistry.Resolve("game", null, ModelWalk);
        if (target == null) return false;
        const BindingFlags f = BindingFlags.Public | BindingFlags.Static;
        HookManager.AddPre(_self, target, typeof(ObjectCarry).GetMethod(nameof(Before), f)!);
        HookManager.AddPost(_self, target, typeof(ObjectCarry).GetMethod(nameof(After), f)!);
        HookManager.Commit();
        return HookAttach.Installed(target);
    }

    static Pose Read(IMemory m, Table t, uint e) => new()
    {
        X = (int)m.ReadU32(e + (uint)t.Pos),
        Y = (int)m.ReadU32(e + (uint)t.Pos + 4),
        Z = (int)m.ReadU32(e + (uint)t.Pos + 8),
        Rx = (short)m.ReadU16(e + (uint)t.Rot),
        Ry = (short)m.ReadU16(e + (uint)t.Rot + 2),
        Rz = (short)m.ReadU16(e + (uint)t.Rot + 4),
        Kind = m.ReadU8(e + (uint)t.Kind),
        Live = t.Live(m, e),
        Clip = t.Name == "creatures" ? m.ReadU8(e + 4) : (byte)0xFF,
        Time = t.Name == "creatures" ? m.ReadU16(e + 8) : (ushort)0,
    };

    static void Write(IMemory m, Table t, uint e, in Pose p)
    {
        m.WriteU32(e + (uint)t.Pos, (uint)p.X);
        m.WriteU32(e + (uint)t.Pos + 4, (uint)p.Y);
        m.WriteU32(e + (uint)t.Pos + 8, (uint)p.Z);
        m.WriteU16(e + (uint)t.Rot, (ushort)p.Rx);
        m.WriteU16(e + (uint)t.Rot + 2, (ushort)p.Ry);
        m.WriteU16(e + (uint)t.Rot + 4, (ushort)p.Rz);
    }

    static int AngleStep(short from, short to) => ((to - from + 2048) & 4095) - 2048;
    static short LerpAngle(short a, short b, double t) => (short)(ushort)(a + (int)Math.Round(AngleStep(a, b) * t));
    static int Lerp(int a, int b, double t) => a + (int)Math.Round((b - a) * t);

    public static void Before(CpuContext c, IMemory m)
    {
        for (int ti = 0; ti < Tables.Length; ti++) Array.Clear(Written[ti]);
        AnimCarry.Clear();
        if (!Enabled || !ViewCarry.Carrying) return;
        _walks++;
        double phase = ViewCarry.Phase;
        bool tick = ViewCarry.TickRender;

        for (int ti = 0; ti < Tables.Length; ti++)
        {
            var t = Tables[ti];
            for (int i = 0; i < t.Count; i++)
            {
                uint e = t.Base + (uint)(i * t.Stride);
                var now = Read(m, t, e);
                if (tick)
                {
                    Prev[ti][i] = Cur[ti][i];
                    Cur[ti][i] = now;
                }
                if (!now.Live) continue;
                _live++;
                if (_probe2 && tick && t.Name == "creatures" && _dbg++ % 20 == 0)
                    Console.WriteLine($"[KF1] creature {i}: kind {now.Kind} clip {now.Clip} time {Prev[ti][i].Time}->{now.Time} pos {now.X},{now.Y},{now.Z}");

                ref var p = ref Prev[ti][i];
                ref var q = ref Cur[ti][i];
                // Carry only a slot that was this same live thing at both ticks,
                // and that the game has not moved since (a redraw between ticks
                // sees the tick's values; anything else is a write we did not see).
                if (!p.Live || !q.Live || p.Kind != q.Kind) continue;
                if (now.X != q.X || now.Y != q.Y || now.Z != q.Z || now.Ry != q.Ry) continue;
                // The pose's time, for AnimCarry: the same clip at both ticks and
                // moving forward by a little.
                if (q.Clip != 0xFF && p.Clip == q.Clip && now.Time == q.Time && q.Time > p.Time && q.Time - p.Time <= 8)
                    AnimCarry.Note(e, q.Time - (q.Time - p.Time) * (1.0 - phase));
                if (Math.Abs((long)q.X - p.X) > WarpUnits || Math.Abs((long)q.Y - p.Y) > WarpUnits ||
                    Math.Abs((long)q.Z - p.Z) > WarpUnits) { _warps++; continue; }
                if (p.X == q.X && p.Y == q.Y && p.Z == q.Z && p.Rx == q.Rx && p.Ry == q.Ry && p.Rz == q.Rz) continue;

                var carried = new Pose
                {
                    X = Lerp(p.X, q.X, phase), Y = Lerp(p.Y, q.Y, phase), Z = Lerp(p.Z, q.Z, phase),
                    Rx = LerpAngle(p.Rx, q.Rx, phase), Ry = LerpAngle(p.Ry, q.Ry, phase), Rz = LerpAngle(p.Rz, q.Rz, phase),
                };
                // Keep a yaw the game holds in 0..4095 there, as ViewCarry does.
                if (p.Ry >= 0 && q.Ry >= 0 && p.Ry < 4096 && q.Ry < 4096) carried.Ry = (short)(carried.Ry & 0xFFF);
                Truth[ti][i] = now;
                Write(m, t, e, carried);
                Written[ti][i] = true;
                _carried++;
            }
        }
    }

    public static void After(CpuContext c, IMemory m)
    {
        for (int ti = 0; ti < Tables.Length; ti++)
        {
            var t = Tables[ti];
            for (int i = 0; i < t.Count; i++)
                if (Written[ti][i]) Write(m, t, t.Base + (uint)(i * t.Stride), Truth[ti][i]);
        }
        if (_probe) Report();
    }

    static void Report()
    {
        double now = Environment.TickCount64 / 1000.0;
        if (_windowStart == 0) { _windowStart = now; return; }
        double secs = now - _windowStart;
        if (secs < 1.0) return;
        Console.WriteLine($"[KF1] objects: {_walks / secs:0.0} walks/s carried, " +
                          $"{(_walks == 0 ? 0 : (double)_live / _walks):0.0} live slots, " +
                          $"{(_walks == 0 ? 0 : (double)_carried / _walks):0.0} carried a walk, {_warps} warps");
        _walks = _carried = _live = _warps = 0;
        _windowStart = now;
    }
}
