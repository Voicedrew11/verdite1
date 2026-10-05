using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// Creatures, objects and missiles carried between world ticks, with their clip
/// times and the first-person arm's swing:
///
///     KF1_SMOOTH_MODELS=1        on whenever pacing is; 0 to compare
///     KF1_SMOOTH_MODELS_PROBE=1  a line a second: records carried, poses carried, snaps
///
/// Around the renderer's main-loop call, each record live and of the same model at
/// the last two ticks gets its position and angles interpolated at the pacing
/// clock's fraction, and the game's values back afterwards. A clip time is carried
/// without writing it: the pose routine func_800205D4 is handed the tick the
/// carried time lies in, and its one blend weight (gteMIMefunc, return address
/// 0x8002092C) the fraction's share of that segment on top. Verdite3's
/// ModelSmoothing and MoPose on this game's tables. See "Creatures, objects and
/// their clips" in docs/SMOOTHING.md.
/// </summary>
public static class ModelSmoothing
{
    const uint Renderer = 0x8001FDE4;
    const uint MainSite = 0x800148C0 + 8;
    const uint Pose = 0x800205D4;          // (pose, model, clip, time, verts)
    const uint GteMime = 0x8004C860;       // gteMIMefunc(dst, src, n, weight)
    const uint MimeSite = 0x8002092C;      // the pose routine's blend call
    const uint AnimHeaders = 0x80090FCC;   // u32 per model: its animation header

    /// <summary>A step in one tick past which a record is drawn where it is.</summary>
    public const int SnapUnits = 2000;

    public static bool Enabled { get; set; } = true;
    public static bool Active => Enabled && FramePacing.Enabled;
    public static bool ProbeOn { get => _probe; set => _probe = value; }

    sealed record Table(string Name, uint Base, int Stride, uint Pos, uint Rot, int Angles,
                        int Clip, int Time, Func<IMemory, uint, bool> Live, Func<IMemory, uint, int> Model,
                        Func<IMemory, int> Count);

    static readonly Table[] Tables =
    [
        // Creatures: live while +0 and the model +3 are not 0xFF; SVECTOR at +0x1C.
        new("creature", 0x8009D040, 60, 0x0C, 0x1C, 3, 4, 8,
            (m, r) => m.ReadU8(r) != 0xFF && m.ReadU8(r + 3) != 0xFF, (m, r) => m.ReadU8(r + 3), _ => 48),
        // Objects: live while +6 is 1; two angles at +0x2C and +0x2E.
        new("object", 0x8006C4B8, 72, 0x1C, 0x2C, 2, 0xA, 0x12,
            (m, r) => m.ReadU8(r + 6) == 1, (m, r) => m.ReadU8(r + 1), _ => 128),
        // The counted list the model walk draws third (u16 count at 0x80095090).
        new("missile", 0x80095098, 24, 0x04, 0, 0, -1, -1,
            (_, _) => true, (m, r) => m.ReadU16(r), m => Math.Min(m.ReadU16(0x80095090), (ushort)64)),
    ];

    struct Sample
    {
        public bool Live;
        public int Model, X, Y, Z, Clip, Time;
        public short A0, A1, A2;
    }

    static readonly Sample[][] _prev = Tables.Select(t => new Sample[t.Name == "object" ? 128 : 64]).ToArray();
    static readonly Sample[][] _cur = Tables.Select(t => new Sample[t.Name == "object" ? 128 : 64]).ToArray();
    static readonly Sample[][] _saved = Tables.Select(t => new Sample[t.Name == "object" ? 128 : 64]).ToArray();
    static readonly bool[][] _written = Tables.Select(t => new bool[t.Name == "object" ? 128 : 64]).ToArray();

    // The first-person arm: func_8001F798 poses model 0x14, clip 0, at the s16 time
    // 0x800A07F0 (-1 while it is not swinging) into the pose buffer 0x800A07F4.
    const uint ArmTime = 0x800A07F0, ArmPose = 0x800A07F4;
    const int ArmModel = 0x14;
    static int _armPrev = -1, _armCur = -1;
    static long _armCarried;

    // Pose buffer (record + 0x34) -> the carried clip time, for the frame being drawn.
    static readonly Dictionary<uint, double> _carriedTime = [];
    static bool _primed, _restore;
    static long _tick = -1;

    // The pose call in flight: what the blend weight is for.
    static int _poseModel = -1, _poseClip, _poseTime;
    static double _poseFrac;

    static readonly ModInfo _self = new() { Id = "kf1.modelsmoothing", Name = "Model smoothing", Version = "1.0" };

    static bool _probe;
    static double _probeAt = -1.0;
    static long _frames, _carried, _posed, _weighted, _snaps;

    public static void Configure(string? mode, string? probe)
    {
        Enabled = mode?.Trim() is not ("0" or "off");
        _probe = probe?.Trim() == "1";
    }

    public static void Install()
    {
        Event.AddListener<OverlayLoadedEvent>(_ => _primed = false);
        HookAttach.OnOverlayLoad("model smoothing", Attach);
        Console.WriteLine($"[KF1] model smoothing: {(Enabled ? "on" : "off")}");
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var r = SymbolRegistry.Resolve("game", null, Renderer);
        var p = SymbolRegistry.Resolve("game", null, Pose);
        var g = SymbolRegistry.Resolve("game", null, GteMime);
        if (r == null || p == null || g == null) return false;
        var self = typeof(ModelSmoothing);
        MethodInfo M(string n) => self.GetMethod(n, BindingFlags.Public | BindingFlags.Static)!;
        HookManager.AddPre(_self, r, M(nameof(BeforeRenderer)));
        HookManager.AddPost(_self, r, M(nameof(AfterRenderer)));
        HookManager.AddPre(_self, p, M(nameof(BeforePose)));
        HookManager.AddPost(_self, p, M(nameof(AfterPose)));
        HookManager.AddPre(_self, g, M(nameof(BeforeMime)));
        HookManager.Commit();
        return HookAttach.Installed(r) && HookAttach.Installed(p) && HookAttach.Installed(g);
    }

    public static void BeforeRenderer(CpuContext c, IMemory m)
    {
        _restore = false;
        _carriedTime.Clear();
        if (c.RA != MainSite || !Active) { _primed = false; return; }

        long tick = FramePacing.Ticks;
        if (!_primed || (tick != _tick && FramePacing.IterationTicked))
        {
            for (int t = 0; t < Tables.Length; t++)
            {
                (_prev[t], _cur[t]) = (_cur[t], _prev[t]);
                Read(m, Tables[t], _cur[t]);
                if (!_primed) Array.Copy(_cur[t], _prev[t], _cur[t].Length);
            }
            _armPrev = _primed ? _armCur : -1;
            _armCur = (short)m.ReadU16(ArmTime);
            _primed = true;
            _tick = tick;
        }

        double frac = FramePacing.TickFraction;
        _frameId++;
        for (int t = 0; t < Tables.Length; t++) Carry(m, Tables[t], t, frac);
        if (_armPrev >= 0 && _armCur >= 0 && (short)m.ReadU16(ArmTime) == _armCur
            && ClipTime(m, ArmModel, 0, _armPrev, _armCur, frac) is { } arm)
        {
            _carriedTime[ArmPose] = arm;
            _armCarried++;
        }
        _restore = true;
        if (_probe) Probe();
    }

    static void Read(IMemory m, Table t, Sample[] into)
    {
        int n = Math.Min(t.Count(m), into.Length);
        for (int i = 0; i < into.Length; i++)
        {
            ref var s = ref into[i];
            uint r = t.Base + (uint)(i * t.Stride);
            s.Live = i < n && t.Live(m, r);
            if (!s.Live) continue;
            s.Model = t.Model(m, r);
            s.X = (int)m.ReadU32(r + t.Pos); s.Y = (int)m.ReadU32(r + t.Pos + 4); s.Z = (int)m.ReadU32(r + t.Pos + 8);
            s.A0 = t.Angles > 0 ? (short)m.ReadU16(r + t.Rot) : (short)0;
            s.A1 = t.Angles > 1 ? (short)m.ReadU16(r + t.Rot + 2) : (short)0;
            s.A2 = t.Angles > 2 ? (short)m.ReadU16(r + t.Rot + 4) : (short)0;
            s.Clip = t.Clip >= 0 ? m.ReadU8(r + (uint)t.Clip) : 0xFF;
            s.Time = t.Time >= 0 ? m.ReadU16(r + (uint)t.Time) : 0;
        }
    }

    static void Carry(IMemory m, Table t, int ti, double frac)
    {
        var prev = _prev[ti]; var cur = _cur[ti]; var saved = _saved[ti]; var written = _written[ti];
        int n = Math.Min(t.Count(m), cur.Length);
        for (int i = 0; i < cur.Length; i++)
        {
            written[i] = false;
            ref var a = ref prev[i];
            ref var b = ref cur[i];
            if (i >= n || !a.Live || !b.Live || a.Model != b.Model) continue;
            uint r = t.Base + (uint)(i * t.Stride);
            // The record must still be what the tick left: nothing between ticks moves it.
            if ((int)m.ReadU32(r + t.Pos) != b.X || (int)m.ReadU32(r + t.Pos + 8) != b.Z) continue;
            if (Math.Abs((long)b.X - a.X) > SnapUnits || Math.Abs((long)b.Y - a.Y) > SnapUnits
                || Math.Abs((long)b.Z - a.Z) > SnapUnits) { _snaps++; continue; }

            saved[i] = b;
            written[i] = true;
            m.WriteU32(r + t.Pos, (uint)ViewSmoothing.Mix(a.X, b.X, frac));
            m.WriteU32(r + t.Pos + 4, (uint)ViewSmoothing.Mix(a.Y, b.Y, frac));
            m.WriteU32(r + t.Pos + 8, (uint)ViewSmoothing.Mix(a.Z, b.Z, frac));
            if (t.Angles > 0) m.WriteU16(r + t.Rot, (ushort)ViewSmoothing.Turn(a.A0, b.A0, frac));
            if (t.Angles > 1) m.WriteU16(r + t.Rot + 2, (ushort)ViewSmoothing.Turn(a.A1, b.A1, frac));
            if (t.Angles > 2) m.WriteU16(r + t.Rot + 4, (ushort)ViewSmoothing.Turn(a.A2, b.A2, frac));
            if (a.X != b.X || a.Y != b.Y || a.Z != b.Z || a.A0 != b.A0 || a.A1 != b.A1 || a.A2 != b.A2) _carried++;

            if (t.Clip >= 0 && a.Clip == b.Clip && b.Clip != 0xFF
                && ClipTime(m, AnimModel(m, t, b.Model), b.Clip, a.Time, b.Time, frac) is { } time)
                _carriedTime[r + 0x34] = time;
        }
    }

    /// <summary>The model the pose routine is handed: a creature's +3 plus 0x1E, an
    /// object's from its type record (0x8006BD99 + type * 152, low nibble).</summary>
    static int AnimModel(IMemory m, Table t, int model) =>
        t.Name == "creature" ? model + 0x1E : m.ReadU8(0x8006BD99 + (uint)model * 152) & 0xF;

    /// <summary>The clip time a frame at <paramref name="frac"/> between two ticks stands
    /// for, unwrapped over the clip's length when it looped; null when it jumped.</summary>
    static double? ClipTime(IMemory m, int model, int clip, int t0, int t1, double frac)
    {
        // Clips run to a length of their own (4096 for the ones read so far), and a
        // step of more than half of it in one tick is a jump, not play.
        int length = ClipLength(m, model, clip);
        if (length <= 0) return null;
        int dt = t1 - t0;
        if (dt < 0) dt += length;
        if (dt <= 0 || dt > length / 2) return null;
        double t = t0 + dt * frac;
        return t >= length ? t - length : t;
    }

    // The clip as the pose routine reads it: header + *(header + 0x10) is a table of
    // u32 offsets, a clip is a u16 count then u32 offsets to segments, and a segment
    // is a u16 flag and a u16 duration.
    static uint ClipAddress(IMemory m, int model, int clip)
    {
        uint header = m.ReadU32(AnimHeaders + (uint)model * 4);
        if ((header & 0xFF000000) != 0x80000000) return 0;
        uint table = header + m.ReadU32(header + 0x10);
        return header + m.ReadU32(table + (uint)clip * 4);
    }

    static int ClipLength(IMemory m, int model, int clip)
    {
        uint c = ClipAddress(m, model, clip);
        if (c == 0) return 0;
        uint header = m.ReadU32(AnimHeaders + (uint)model * 4);
        int count = m.ReadU16(c), length = 0;
        for (int k = 0; k < count && k < 256; k++) length += m.ReadU16(header + m.ReadU32(c + 4 + (uint)k * 4) + 2);
        return length;
    }

    /// <summary>The segment holding <paramref name="time"/>: its duration and flag, or null.</summary>
    static (int Duration, bool Flag)? Segment(IMemory m, int model, int clip, int time)
    {
        uint c = ClipAddress(m, model, clip);
        if (c == 0) return null;
        uint header = m.ReadU32(AnimHeaders + (uint)model * 4);
        int count = m.ReadU16(c), end = 0;
        for (int k = 0; k < count && k < 256; k++)
        {
            uint seg = header + m.ReadU32(c + 4 + (uint)k * 4);
            int dur = m.ReadU16(seg + 2);
            end += dur;
            if (time < end) return dur == 0 ? null : (dur, m.ReadU16(seg) != 0);
        }
        return null;
    }

    static readonly Dictionary<uint, (double T, long Frame)> _lastPosed = [];
    static long _backward, _repeated, _frameId;

    public static void BeforePose(CpuContext c, IMemory m)
    {
        _poseModel = -1;
        if (!_carriedTime.TryGetValue(c.A0, out double t)) return;
        // Against the frame before only: a frame drawn at the game's own time (a
        // clip restarted, a jump) breaks the run.
        if (_probe && _lastPosed.TryGetValue(c.A0, out var last) && last.Frame == _frameId - 1)
        {
            if (t == last.T) _repeated++;
            else if (t < last.T && last.T - t < 2048) _backward++;
        }
        _lastPosed[c.A0] = (t, _frameId);
        int whole = (int)Math.Floor(t);
        _poseModel = (int)(c.A1 & 0xFFFF);
        _poseClip = (int)(c.A2 & 0xFFFF);
        _poseTime = whole;
        _poseFrac = t - whole;
        c.A3 = (uint)whole;
        _posed++;
    }

    public static void AfterPose(CpuContext c, IMemory m) => _poseModel = -1;

    public static void BeforeMime(CpuContext c, IMemory m)
    {
        if (_poseModel < 0 || c.RA != MimeSite || _poseFrac <= 0.0) return;
        if (Segment(m, _poseModel, _poseClip, _poseTime) is not { } seg) return;
        double share = _poseFrac * 4096.0 / seg.Duration;
        int w = (int)(c.A3 & 0xFFFF) + (int)Math.Round(seg.Flag ? -share : share);
        c.A3 = (uint)Math.Clamp(w, 0, 4096);
        _weighted++;
    }

    public static void AfterRenderer(CpuContext c, IMemory m)
    {
        _carriedTime.Clear();
        if (!_restore) return;
        _restore = false;
        for (int ti = 0; ti < Tables.Length; ti++)
        {
            var t = Tables[ti];
            for (int i = 0; i < _written[ti].Length; i++)
            {
                if (!_written[ti][i]) continue;
                _written[ti][i] = false;
                ref var s = ref _saved[ti][i];
                uint r = t.Base + (uint)(i * t.Stride);
                m.WriteU32(r + t.Pos, (uint)s.X); m.WriteU32(r + t.Pos + 4, (uint)s.Y); m.WriteU32(r + t.Pos + 8, (uint)s.Z);
                if (t.Angles > 0) m.WriteU16(r + t.Rot, (ushort)s.A0);
                if (t.Angles > 1) m.WriteU16(r + t.Rot + 2, (ushort)s.A1);
                if (t.Angles > 2) m.WriteU16(r + t.Rot + 4, (ushort)s.A2);
            }
        }
    }

    static void Probe()
    {
        _frames++;
        double now = Environment.TickCount64 / 1000.0;
        if (_probeAt < 0.0) _probeAt = now;
        double dt = now - _probeAt;
        if (dt < 1.0) return;
        Console.WriteLine($"[KF1] model smoothing: {_frames / dt:0.0} frame(s)/s, {_carried / dt:0.0} moving record(s) carried/s, " +
                          $"{_posed / dt:0.0} pose(s) carried/s ({_armCarried / dt:0.0} of the arm), {_weighted / dt:0.0} weight(s) moved/s, {_repeated} repeated and {_backward} backward pose(s), {_snaps} snap(s)");
        Console.Out.Flush();
        _probeAt = now;
        _frames = _carried = _posed = _weighted = _snaps = _backward = _repeated = _armCarried = 0;
    }
}
