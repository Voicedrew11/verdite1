using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// Everything the model walk draws, carried between world ticks, with clip times
/// and the first-person arm's swing:
///
///     KF1_SMOOTH_MODELS=1        on whenever pacing is; 0 to compare
///     KF1_SMOOTH_MODELS_PROBE=1  a line a second: drawn and carried by table, snaps, poses, held cels
///
/// Verdite3's seam: the carry happens at each record's own submit, keyed by table
/// and slot with an identity, so whatever the walk draws is carried and nothing
/// else is touched. A pre hook on each of the walk's five submitters samples the
/// record on a tick's first draw, writes it interpolated at the pacing clock's
/// fraction, and the post hook puts the tick's values back. A clip time is carried
/// without writing it: the pose routine func_800205D4 is handed the tick the
/// carried time lies in, and its one blend weight (gteMIMefunc, return address
/// 0x8002092C) the fraction's share of that segment on top. The list's flipbook
/// cel steps once a tick, not once a draw. See "Creatures, objects and their clips"
/// in docs/SMOOTHING.md.
/// </summary>
public static class ModelSmoothing
{
    const uint Renderer = 0x8001FDE4;
    const uint MainSite = 0x800148C0 + 8;
    const uint Pose = 0x800205D4;          // (pose, model, clip, time, verts)
    const uint GteMime = 0x8004C860;       // gteMIMefunc(dst, src, n, weight)
    const uint MimeSite = 0x8002092C;      // the pose routine's blend call
    const uint AnimHeaders = 0x80090FCC;   // u32 per model: its animation header
    const uint ArmSubmit = 0x8001F798;     // the first-person arm, after the walk

    /// <summary>A position step in one tick past which a record snaps (a warp, a spawn).</summary>
    public const int SnapUnits = 2000;

    /// <summary>An angle step in one tick past which a record snaps, 0x1000 a turn.</summary>
    public const int SnapAngle = 0x300;

    public static bool Enabled { get; set; } = true;
    public static bool Active => Enabled && FramePacing.Enabled;
    public static bool ProbeOn { get => _probe; set => _probe = value; }

    /// <summary>One table the walk func_8001F218 draws, through its own submitter.</summary>
    sealed record Table(string Name, uint Submit, uint Base, int Stride, int Count,
                        uint Pos, uint Rot, int Angles, Func<IMemory, uint, uint> Identity);

    static readonly Table[] Tables =
    [
        // Interaction objects (pickups, doors, props): stage F moves their Y and angles.
        new("interaction", 0x8001EBB8, 0x8006EDE0, 44, 190, 0x08, 0x18, 2, (m, r) => m.ReadU8(r)),
        new("object", 0x8001E9A4, 0x8006C4B8, 72, 128, 0x1C, 0x2C, 2, (m, r) => m.ReadU8(r + 1)),
        // The counted list (u16 count at 0x80095090): flipbook sprites, cel at +0x14.
        new("list", 0x8001ED90, 0x80095098, 24, 256, 0x04, 0, 0, (m, r) => m.ReadU16(r) | (uint)m.ReadU8(r + 2) << 16),
        new("creature", 0x8001EEDC, 0x8009D040, 60, 48, 0x0C, 0x1C, 3, (m, r) => m.ReadU8(r + 3) | (uint)m.ReadU8(r + 2) << 8),
        // The people stage H runs: SVECTOR at +0x34 through RotMatrix.
        new("person", 0x8001F0C4, 0x8009DB88, 68, 8, 0x24, 0x34, 3, (m, r) => m.ReadU8(r + 2) | (uint)m.ReadU8(r + 1) << 8),
    ];

    const int ListTable = 2;
    const uint ListCel = 0x14;
    static readonly int[] TableBase = new int[Tables.Length];
    static readonly int Total;
    static readonly int ArmSlot;            // clip state only

    // ---- positions and angles, by flattened slot ----
    static readonly long[] _tick;
    static readonly uint[] _id;
    static readonly bool[] _primed;
    static readonly int[] _px, _py, _pz, _cx, _cy, _cz;
    static readonly short[] _pa, _pb, _pc, _ca, _cb, _cc;

    // ---- clip times, by flattened slot (and the arm) ----
    static readonly long[] _clipTick;
    static readonly int[] _clipModel, _clip, _t0, _t1;
    static readonly bool[] _clipPrimed;

    // ---- the flipbook cel ----
    static readonly long[] _celTick;

    // The submit in progress: what its post hook puts back, and whose clip the pose is.
    static int _inSlot = -1, _inTable = -1;
    static uint _inRec;
    static bool _wrote, _heldCel;
    static int _savedX, _savedY, _savedZ;
    static short _savedA, _savedB, _savedC;
    static byte _savedCel;

    static bool _mainDraw;

    // The pose call in flight: what the blend weight is for.
    static int _poseModel = -1, _poseClip, _poseTime;
    static double _poseFrac;

    static readonly ModInfo _self = new() { Id = "kf1.modelsmoothing", Name = "Model smoothing", Version = "2.0" };

    static bool _probe;
    static double _probeAt = -1.0;
    static long _frames, _posed, _weighted, _snaps, _armPosed, _celsHeld;
    static readonly long[] _drawn = new long[Tables.Length], _carried = new long[Tables.Length];

    static ModelSmoothing()
    {
        int n = 0;
        for (int t = 0; t < Tables.Length; t++) { TableBase[t] = n; n += Tables[t].Count; }
        Total = n;
        ArmSlot = n;
        _tick = new long[Total]; _id = new uint[Total]; _primed = new bool[Total];
        _px = new int[Total]; _py = new int[Total]; _pz = new int[Total];
        _cx = new int[Total]; _cy = new int[Total]; _cz = new int[Total];
        _pa = new short[Total]; _pb = new short[Total]; _pc = new short[Total];
        _ca = new short[Total]; _cb = new short[Total]; _cc = new short[Total];
        _clipTick = new long[Total + 1]; _clipModel = new int[Total + 1]; _clip = new int[Total + 1];
        _t0 = new int[Total + 1]; _t1 = new int[Total + 1]; _clipPrimed = new bool[Total + 1];
        _celTick = new long[Tables[ListTable].Count];
        Reprime();
    }

    public static void Configure(string? mode, string? probe)
    {
        Enabled = mode?.Trim() is not ("0" or "off");
        _probe = probe?.Trim() == "1";
    }

    public static void Install()
    {
        Event.AddListener<OverlayLoadedEvent>(_ => Reprime());
        HookAttach.OnOverlayLoad("model smoothing", Attach);
        Console.WriteLine($"[KF1] model smoothing: {(Enabled ? "on" : "off")}");
    }

    /// <summary>Forget every slot: an overlay load replaces the tables' tenants.</summary>
    static void Reprime()
    {
        Array.Fill(_tick, -1L);
        Array.Fill(_primed, false);
        Array.Fill(_clipTick, -1L);
        Array.Fill(_clipPrimed, false);
        Array.Fill(_celTick, -1L);
        _inSlot = _inTable = -1;
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var self = typeof(ModelSmoothing);
        MethodInfo M(string n) => self.GetMethod(n, BindingFlags.Public | BindingFlags.Static)!;
        var targets = new List<MethodInfo>();
        MethodInfo? Hook(uint addr, string? pre, string? post)
        {
            var f = SymbolRegistry.Resolve("game", null, addr);
            if (f == null) return null;
            if (pre != null) HookManager.AddPre(_self, f, M(pre));
            if (post != null) HookManager.AddPost(_self, f, M(post));
            targets.Add(f);
            return f;
        }
        if (Hook(Renderer, nameof(BeforeRenderer), nameof(AfterRenderer)) == null) return false;
        if (Hook(Pose, nameof(BeforePose), nameof(AfterPose)) == null) return false;
        if (Hook(GteMime, nameof(BeforeMime), null) == null) return false;
        if (Hook(ArmSubmit, nameof(BeforeArm), nameof(AfterSubmit)) == null) return false;
        foreach (var t in Tables)
            if (Hook(t.Submit, nameof(BeforeSubmit), nameof(AfterSubmit)) == null) return false;
        HookManager.Commit();
        return targets.All(HookAttach.Installed);
    }

    public static void BeforeRenderer(CpuContext c, IMemory m)
    {
        _mainDraw = c.RA == MainSite && Active;
        if (_mainDraw && _probe) Probe();
    }

    public static void AfterRenderer(CpuContext c, IMemory m) => _mainDraw = false;

    // ---- the submit seam ----

    public static void BeforeSubmit(CpuContext c, IMemory m)
    {
        _inSlot = -1;
        _wrote = _heldCel = false;
        if (!FramePacing.Enabled) return;
        // The submitter is told by its record: each table's records lie in their own range.
        uint r = c.A0;
        int ti = -1;
        for (int t = 0; t < Tables.Length; t++)
        {
            var tb = Tables[t];
            if (r >= tb.Base && r < tb.Base + (uint)(tb.Count * tb.Stride) && (r - tb.Base) % (uint)tb.Stride == 0) { ti = t; break; }
        }
        if (ti < 0) return;
        var table = Tables[ti];
        int slot = (int)((r - table.Base) / (uint)table.Stride);
        int i = TableBase[ti] + slot;
        _inSlot = i; _inTable = ti; _inRec = r;

        if (ti == ListTable) HoldCel(m, r, slot);
        if (!_mainDraw) return;
        Carry(m, table, ti, i, r);
    }

    public static void BeforeArm(CpuContext c, IMemory m)
    {
        _wrote = _heldCel = false;
        _inSlot = ArmSlot; _inTable = -1;
    }

    public static void AfterSubmit(CpuContext c, IMemory m)
    {
        if (_wrote && _inTable >= 0)
        {
            var t = Tables[_inTable];
            uint r = _inRec;
            m.WriteU32(r + t.Pos, (uint)_savedX); m.WriteU32(r + t.Pos + 4, (uint)_savedY); m.WriteU32(r + t.Pos + 8, (uint)_savedZ);
            if (t.Angles > 0) m.WriteU16(r + t.Rot, (ushort)_savedA);
            if (t.Angles > 1) m.WriteU16(r + t.Rot + 2, (ushort)_savedB);
            if (t.Angles > 2) m.WriteU16(r + t.Rot + 4, (ushort)_savedC);
        }
        if (_heldCel) m.WriteU8(_inRec + ListCel, _savedCel);
        _wrote = _heldCel = false;
        _inSlot = _inTable = -1;
    }

    /// <summary>The list's submitter steps its cel on every draw; under pacing the
    /// step is kept for a tick's first draw of the record only. It draws the cel it
    /// read before stepping, so a held draw shows the tick's cel.</summary>
    static void HoldCel(IMemory m, uint r, int slot)
    {
        if (!FramePacing.Frozen && _celTick[slot] != FramePacing.Ticks) { _celTick[slot] = FramePacing.Ticks; return; }
        _savedCel = m.ReadU8(r + ListCel);
        _heldCel = true;
        _celsHeld++;
    }

    static void Carry(IMemory m, Table t, int ti, int i, uint r)
    {
        _drawn[ti]++;
        int x = (int)m.ReadU32(r + t.Pos), y = (int)m.ReadU32(r + t.Pos + 4), z = (int)m.ReadU32(r + t.Pos + 8);
        short a = t.Angles > 0 ? (short)m.ReadU16(r + t.Rot) : (short)0;
        short b = t.Angles > 1 ? (short)m.ReadU16(r + t.Rot + 2) : (short)0;
        short cc = t.Angles > 2 ? (short)m.ReadU16(r + t.Rot + 4) : (short)0;
        uint id = t.Identity(m, r);

        long ticks = FramePacing.Ticks;
        if (ticks != _tick[i] && FramePacing.IterationTicked)
        {
            // A missed tick (culled) or a new tenant has no pair: prime. Else roll forward.
            if (!_primed[i] || _tick[i] != ticks - 1 || _id[i] != id) Prime(i, x, y, z, a, b, cc);
            else
            {
                _px[i] = _cx[i]; _py[i] = _cy[i]; _pz[i] = _cz[i];
                _pa[i] = _ca[i]; _pb[i] = _cb[i]; _pc[i] = _cc[i];
                _cx[i] = x; _cy[i] = y; _cz[i] = z;
                _ca[i] = a; _cb[i] = b; _cc[i] = cc;
                if (Jump(i)) { Prime(i, x, y, z, a, b, cc); _snaps++; }
            }
            _tick[i] = ticks;
            _id[i] = id;
        }
        else if (_primed[i] && (x != _cx[i] || y != _cy[i] || z != _cz[i] || a != _ca[i] || b != _cb[i] || cc != _cc[i]))
        {
            // Moved without a tick of the world: no pair.
            Prime(i, x, y, z, a, b, cc);
            _snaps++;
        }

        if (!_primed[i] || (_px[i] == _cx[i] && _py[i] == _cy[i] && _pz[i] == _cz[i]
                            && _pa[i] == _ca[i] && _pb[i] == _cb[i] && _pc[i] == _cc[i])) return;

        double f = FramePacing.TickFraction;
        _savedX = x; _savedY = y; _savedZ = z; _savedA = a; _savedB = b; _savedC = cc;
        _wrote = true;
        _carried[ti]++;
        m.WriteU32(r + t.Pos, (uint)ViewSmoothing.Mix(_px[i], _cx[i], f));
        m.WriteU32(r + t.Pos + 4, (uint)ViewSmoothing.Mix(_py[i], _cy[i], f));
        m.WriteU32(r + t.Pos + 8, (uint)ViewSmoothing.Mix(_pz[i], _cz[i], f));
        if (t.Angles > 0) m.WriteU16(r + t.Rot, (ushort)ViewSmoothing.Turn(_pa[i], _ca[i], f));
        if (t.Angles > 1) m.WriteU16(r + t.Rot + 2, (ushort)ViewSmoothing.Turn(_pb[i], _cb[i], f));
        if (t.Angles > 2) m.WriteU16(r + t.Rot + 4, (ushort)ViewSmoothing.Turn(_pc[i], _cc[i], f));
    }

    static void Prime(int i, int x, int y, int z, short a, short b, short c)
    {
        _px[i] = _cx[i] = x; _py[i] = _cy[i] = y; _pz[i] = _cz[i] = z;
        _pa[i] = _ca[i] = a; _pb[i] = _cb[i] = b; _pc[i] = _cc[i] = c;
        _primed[i] = true;
    }

    static bool Jump(int i) =>
        Math.Abs((long)_cx[i] - _px[i]) > SnapUnits || Math.Abs((long)_cy[i] - _py[i]) > SnapUnits
        || Math.Abs((long)_cz[i] - _pz[i]) > SnapUnits || Math.Abs(Wrap(_ca[i] - _pa[i])) > SnapAngle
        || Math.Abs(Wrap(_cb[i] - _pb[i])) > SnapAngle || Math.Abs(Wrap(_cc[i] - _pc[i])) > SnapAngle;

    static int Wrap(int d) => ((d + 0x800) & 0xFFF) - 0x800;

    // ---- the clip time ----

    public static void BeforePose(CpuContext c, IMemory m)
    {
        _poseModel = -1;
        int i = _inSlot;
        if (i < 0 || !_mainDraw) return;
        int model = (int)(c.A1 & 0xFFFF), clip = (int)(c.A2 & 0xFFFF), time = (short)c.A3;

        long ticks = FramePacing.Ticks;
        if (ticks != _clipTick[i] && FramePacing.IterationTicked)
        {
            if (_clipPrimed[i] && _clipTick[i] == ticks - 1 && _clipModel[i] == model && _clip[i] == clip)
            { _t0[i] = _t1[i]; _t1[i] = time; }
            else { _t0[i] = _t1[i] = time; _clipPrimed[i] = true; }
            _clipModel[i] = model; _clip[i] = clip;
            _clipTick[i] = ticks;
        }
        else if (_clipPrimed[i] && (time != _t1[i] || model != _clipModel[i] || clip != _clip[i]))
        {
            _t0[i] = _t1[i] = time; _clipModel[i] = model; _clip[i] = clip;
        }

        if (!_clipPrimed[i] || _t0[i] == _t1[i]) return;
        if (ClipTime(m, model, clip, _t0[i], _t1[i], FramePacing.TickFraction) is not { } t) return;
        int whole = (int)Math.Floor(t);
        _poseModel = model;
        _poseClip = clip;
        _poseTime = whole;
        _poseFrac = t - whole;
        c.A3 = (uint)whole;
        _posed++;
        if (i == ArmSlot) _armPosed++;
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

    /// <summary>The clip time a frame at <paramref name="frac"/> between two ticks stands
    /// for, unwrapped over the clip's length when it looped; null when it jumped.</summary>
    static double? ClipTime(IMemory m, int model, int clip, int t0, int t1, double frac)
    {
        // A step of more than half the clip's length in one tick is a jump, not play.
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
    // is a u16 flag and a u16 duration. A model with *(header + 4) == 0 has no clips
    // (the routine returns before reading them).
    static uint ClipAddress(IMemory m, int model, int clip)
    {
        uint header = m.ReadU32(AnimHeaders + (uint)model * 4);
        if (!InRam(header, 0x14) || m.ReadU32(header + 4) == 0) return 0;
        uint table = header + m.ReadU32(header + 0x10);
        if (!InRam(table + (uint)clip * 4, 4)) return 0;
        uint c = header + m.ReadU32(table + (uint)clip * 4);
        return InRam(c, 4) ? c : 0;
    }

    static bool InRam(uint a, uint size) => a >= 0x80010000 && a + size <= 0x80200000;

    static int ClipLength(IMemory m, int model, int clip)
    {
        uint c = ClipAddress(m, model, clip);
        if (c == 0) return 0;
        uint header = m.ReadU32(AnimHeaders + (uint)model * 4);
        int count = m.ReadU16(c), length = 0;
        for (int k = 0; k < count && k < 256; k++)
        {
            if (!InRam(c + 4 + (uint)k * 4, 4)) return 0;
            uint seg = header + m.ReadU32(c + 4 + (uint)k * 4);
            if (!InRam(seg, 4)) return 0;
            length += m.ReadU16(seg + 2);
        }
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
            if (!InRam(c + 4 + (uint)k * 4, 4)) return null;
            uint seg = header + m.ReadU32(c + 4 + (uint)k * 4);
            if (!InRam(seg, 4)) return null;
            int dur = m.ReadU16(seg + 2);
            end += dur;
            if (time < end) return dur == 0 ? null : (dur, m.ReadU16(seg) != 0);
        }
        return null;
    }

    static void Probe()
    {
        _frames++;
        double now = Environment.TickCount64 / 1000.0;
        if (_probeAt < 0.0) _probeAt = now;
        double dt = now - _probeAt;
        if (dt < 1.0) return;
        var by = string.Join(", ", Tables.Select((t, k) => $"{t.Name} {_drawn[k] / dt:0}/{_carried[k] / dt:0}"));
        Console.WriteLine($"[KF1] model smoothing: {_frames / dt:0.0} frame(s)/s; drawn/carried a second: {by}; " +
                          $"{_posed / dt:0.0} pose(s) carried/s ({_armPosed / dt:0.0} of the arm), {_weighted / dt:0.0} weight(s) moved/s, " +
                          $"{_celsHeld / dt:0.0} cel(s) held/s, {_snaps} snap(s)");
        Console.Out.Flush();
        _probeAt = now;
        _frames = _posed = _weighted = _snaps = _armPosed = _celsHeld = 0;
        Array.Clear(_drawn); Array.Clear(_carried);
    }
}
