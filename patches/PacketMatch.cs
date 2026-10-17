using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// Can a primitive be recognised in the previous tick's display list?
///
///     KF1_PACKETMATCH=1   the match census, once a second
///     KF1_PACKETMATCH=2   also list the worst-matching contexts
///
/// A read-only pre-hook on <c>DrawOTag</c> walks the ordering table and tries to
/// match each primitive to one in the previous logic tick's table. Nothing is written
/// to game memory and nothing is drawn differently; it prints lines, like the other
/// probes. Verdite2's PacketMatch, with KF1's addresses.
///
/// Contexts (what a packet is attributed to) come from the arena spans recorded at
/// the five model submitters (the model walk <c>func_8001F218</c> hands each its
/// record in <c>a0</c>) and the map cell <c>func_8001E5EC</c> (its <c>a0</c>/<c>a1</c>
/// are the cell's column and row). Packets written by anything else are "other".
/// The arena is the frame's packet record at <c>0x80070EB8</c> (start, end, cursor).
///
/// Three keys, measured side by side: K1 ordinal (context, index in context), K2
/// lerpable (K1 and the same shape), K3 intrinsic (context, opcode, texture words).
/// Vertex colours are not in K3: they are this frame's shading. Untextured primitives
/// have no K3 key and are reported as such. The <c>surviving</c> columns count only
/// primitives whose context was drawn on the previous tick too: that is the number
/// that decides whether a packet smoother can work.
///
/// The <c>pose</c> column is the part of a context's motion left after its mean is
/// taken out, measured only on ticks where the camera stood still. The <c>yaw</c> and
/// <c>move</c> labels say whether the window was taken standing, walking or turning.
///
/// Reads: <c>0x80070EB8</c> (the packet record), <c>0x80090EBC</c> (the table),
/// <c>0x800A0824</c> (position, s32 x/y/z) and <c>0x800A083A</c> (yaw, 0x1000 a turn).
/// See "The renderer in C#" in docs/GAME_INTERNALS.md and <see cref="FramePacing"/>
/// for the tick flag.
/// </summary>
public static class PacketMatch
{
    // libgpu DrawOTag, per overlay, as bound in config/kf1.json.
    static readonly (string Overlay, uint Addr)[] DrawOTagSites =
        [("open", 0x800309D4), ("game", 0x80050CF0)];

    // The frame's packet record: a pointer to { start, end, cursor }.
    const uint Record = 0x80070EB8;

    // The five submitters the model walk calls, each handed its record in a0, and the
    // map cell (a0 = column, a1 = row).
    static readonly uint[] ModelSubmitters = [0x8001E9A4, 0x8001EBB8, 0x8001ED90, 0x8001EEDC, 0x8001F0C4];
    const uint MapCell = 0x8001E5EC;

    // The player's position (s32 x, y, z) and yaw (u16, 0x1000 a turn), as ViewSmoothing reads them.
    const uint PosX = 0x800A0824, PosZ = 0x800A082C;
    const uint Yaw = 0x800A083A;

    /// <summary>One turn, in the game's angle units.</summary>
    const int AngleMod = 0x1000;

    const int KindModel = 0, KindTile = 1, KindOther = 2;
    static readonly string[] KindNames = ["models", "map cells", "other (HUD, arm)"];

    static bool _measure, _verbose;

    static readonly ModInfo _self = new()
    {
        Id = "kf1.packetmatch",
        Name = "Packet match probe",
        Version = "1.0",
        Description = "How much of a frame can be matched to the previous tick's frame.",
    };

    public static void Configure(string? probe)
    {
        if (string.IsNullOrWhiteSpace(probe) || probe.Trim() == "0") return;
        _measure = true;
        _verbose = probe.Trim() == "2";
    }

    public static void Install()
    {
        if (!_measure) return;
        _windowStart = Now;
        // Every overlay load is an area change as far as this is concerned: the
        // context ids are new and the player has moved, so nothing carries.
        Event.AddListener<OverlayLoadedEvent>(_ => Forget());
        HookAttach.OnOverlayLoad("packetmatch", Attach);
    }

    // Hooks are added once each, as their overlay loads, so a site that resolves late
    // is not lost. The walk's pre goes on each DrawOTag; the contexts get pre and post.
    static readonly HashSet<uint> _hooked = new();
    static int _targets;

    static bool Attach()
    {
        SymbolRegistry.Build();
        var self = typeof(PacketMatch);
        var walk = self.GetMethod(nameof(BeforeDrawOTag), BindingFlags.Public | BindingFlags.Static)!;

        foreach (var (overlay, addr) in DrawOTagSites)
        {
            if (_hooked.Contains(addr) || SymbolRegistry.Resolve(overlay, null, addr) is not { } t) continue;
            if (HookManager.AddPre(_self, t, walk)) _hooked.Add(addr);
        }

        // The contexts: one slot per routine, so each pre knows what it is entering.
        var sites = ModelSubmitters.Select(a => (Addr: a, Kind: KindModel))
            .Append((Addr: MapCell, Kind: KindTile)).ToArray();
        for (int i = 0; i < sites.Length; i++)
        {
            var (addr, kind) = sites[i];
            if (_hooked.Contains(addr) || SymbolRegistry.Resolve("game", null, addr) is not { } t) continue;
            var slot = typeof(Slot<>).MakeGenericType(Markers[i]);
            slot.GetField(nameof(Slot<int>.Id))!.SetValue(null, i);
            slot.GetField(nameof(Slot<int>.KindOf))!.SetValue(null, kind);
            // Post before pre: an orphan post is harmless, an orphan pre is not.
            if (HookManager.AddPost(_self, t, slot.GetMethod(nameof(Slot<int>.Post))!) &&
                HookManager.AddPre(_self, t, slot.GetMethod(nameof(Slot<int>.Pre))!)) _hooked.Add(addr);
        }

        HookManager.Commit();
        _targets = DrawOTagSites.Length + sites.Length;
        int contexts = sites.Count(s => _hooked.Contains(s.Addr));
        Console.WriteLine($"[KF1] packetmatch: {_hooked.Count} of {_targets} site(s) hooked, " +
                          $"{contexts} of {sites.Length} context(s) attributed" + (_verbose ? ", verbose" : ""));
        return _hooked.Count == _targets;
    }

    // ---------------------------------------------------------------- contexts

    /// <summary>A span of the primitive arena and what was drawing into it.</summary>
    struct Span { public uint Lo, Hi; public int Kind; public uint Id; }

    struct Open { public int Kind; public uint Id; public uint Desc; public uint Entry; }

    static readonly List<Span> _spans = new(1024);
    static readonly Open[] _open = new Open[16];
    static int _depth;

    /// <summary>Per-routine entry and exit, the id read from the call's registers.</summary>
    static class Slot<T>
    {
        public static int Id;
        public static int KindOf;
        public static void Pre(CpuContext c, IMemory m) => Enter(KindOf, KeyOf(Id, c), m);
        public static void Post(CpuContext c, IMemory m) => Leave(m);
    }

    // The id of a context: a submitter's record address, or the map cell's column and row.
    static uint KeyOf(int slot, CpuContext c) => slot < ModelSubmitters.Length ? c.A0 : (c.A1 << 8) | (c.A0 & 0xFFu);

    /// <summary>This frame's packet record and its cursor, or zero if there is no arena yet.</summary>
    static (uint Desc, uint Cur) Arena(IMemory m)
    {
        uint desc = m.ReadU32(Record);
        if (desc < 0x80000000u) return (0u, 0u);
        uint start = m.ReadU32(desc), end = m.ReadU32(desc + 4u);
        return start == 0 || end <= start ? (0u, 0u) : (desc, m.ReadU32(desc + 8u));
    }

    static void Enter(int kind, uint id, IMemory m)
    {
        if (!_measure) return;
        if (_depth >= _open.Length) { _depth++; return; }
        var a = Arena(m);
        _open[_depth++] = new Open { Kind = kind, Id = id, Desc = a.Desc, Entry = a.Cur };
    }

    static void Leave(IMemory m)
    {
        if (_depth <= 0) return;
        if (_depth > _open.Length) { _depth--; return; }

        var f = _open[--_depth];
        var a = Arena(m);
        // A swapped record means the arena changed under the call: drop it, do not guess.
        if (a.Cur == 0 || a.Desc != f.Desc || a.Cur <= f.Entry) return;
        _spans.Add(new Span { Lo = f.Entry & RecompOne.Runtime.Runtime.RamWordMask, Hi = a.Cur & RecompOne.Runtime.Runtime.RamWordMask, Kind = f.Kind, Id = f.Id });
    }

    // ------------------------------------------------------------------- walk

    struct Prim
    {
        public uint Addr;
        public int Kind;
        public uint Id;
        public int Ordinal;
        public int Shape;
        public ulong Hash;
        public bool Keyed;
        public float Cx, Cy;
    }

    struct Seen { public int Shape; public ulong Hash; public float Cx, Cy; }

    /// <summary>One matched primitive's tick displacement, kept until its context's mean is known.</summary>
    struct Move { public int Kind; public uint Id; public float Dx, Dy; }

    static readonly List<Move> _moves = new(2048);
    static readonly List<Prim> _prims = new(2048);
    static Dictionary<(int, uint, int), Seen> _curOrd = new(2048), _prevOrd = new(2048);
    static Dictionary<(int, uint, ulong), Seen> _curInt = new(2048), _prevInt = new(2048);
    static bool _primed;

    static readonly Comparison<Span> ByAddress = (a, b) => a.Lo.CompareTo(b.Lo);

    static readonly Comparison<Prim> ByContextThenAddress = (a, b) =>
        a.Kind != b.Kind ? a.Kind - b.Kind :
        a.Id != b.Id ? a.Id.CompareTo(b.Id) :
        a.Addr.CompareTo(b.Addr);

    /// <summary>
    /// The read-only walk of the table <c>DrawOTag</c> is about to draw. Only tick frames
    /// are sampled (<see cref="FramePacing.TickedThisFrame"/>), so the comparison is always
    /// between two consecutive states of the world.
    /// </summary>
    public static void BeforeDrawOTag(CpuContext c, IMemory m)
    {
        if (!_measure) return;
        try
        {
            if (!FramePacing.TickedThisFrame) return;
            Walk(c.A0, m);
            Label(m);     // before the compare: the pose split needs to know whether the camera held still
            Compare();
            Report();
        }
        finally
        {
            // The record is rewound at the frame head, so the spans belong to the frame just finished.
            _spans.Clear();
            _depth = 0;
        }
    }

    static void Walk(uint head, IMemory m)
    {
        _prims.Clear();
        _spans.Sort(ByAddress);

        uint mask = RecompOne.Runtime.Runtime.RamWordMask;
        uint addr = head & mask;
        for (int guard = 0; guard < 0x100000; guard++)
        {
            uint header = m.ReadU32(addr);
            uint count = header >> 24;
            if (count > 0) Decode(addr, (int)count, m);

            uint next = header & 0xFFFFFFu;
            if (next == 0xFFFFFFu || (next & 0x800000u) != 0) break;
            addr = next & mask;
        }

        // The table is sorted by depth, not submission order. Within a context, address
        // order is submission order (the arena is a bump allocator), so the ordinal uses it.
        _prims.Sort(ByContextThenAddress);
        int ord = 0;
        for (int i = 0; i < _prims.Count; i++)
        {
            if (i > 0 && (_prims[i].Kind != _prims[i - 1].Kind || _prims[i].Id != _prims[i - 1].Id)) ord = 0;
            var p = _prims[i];
            p.Ordinal = ord++;
            _prims[i] = p;
        }
    }

    /// <summary>Which routine built the packet at this address, by the recorded arena spans.</summary>
    static (int Kind, uint Id) Owner(uint addr)
    {
        int lo = 0, hi = _spans.Count - 1;
        while (lo <= hi)
        {
            int mid = (lo + hi) >> 1;
            var s = _spans[mid];
            if (addr < s.Lo) hi = mid - 1;
            else if (addr >= s.Hi) lo = mid + 1;
            else return (s.Kind, s.Id);
        }
        return (KindOther, 0u);
    }

    static int CoordX(uint w) { int x = (int)(w & 0x7FF); return (x & 0x400) != 0 ? x - 0x800 : x; }
    static int CoordY(uint w) { int y = (int)((w >> 16) & 0x7FF); return (y & 0x400) != 0 ? y - 0x800 : y; }

    static ulong Mix(ulong h, uint v) { h ^= v; return h * 0x100000001B3ul; }

    static long _skipped;

    /// <summary>
    /// Unpack one ordering-table node the way the GPU's command stream is laid out: bit 28
    /// gouraud, 27 quad, 26 textured, 25 semi-transparent, 24 raw, then per vertex an
    /// optional colour word, a position word and an optional UV word. Lines, texpage and
    /// environment words are counted and skipped.
    /// </summary>
    static void Decode(uint node, int count, IMemory m)
    {
        uint W(int i) => m.ReadU32(node + 4u + (uint)i * 4u);

        uint cmd = W(0);
        int op = (int)(cmd >> 24);
        var (kind, id) = Owner(node);
        // The opcode and its flag bits, not the colour: the colour is this frame's shading.
        ulong h = Mix(0xCBF29CE484222325ul, cmd >> 24);

        if (op >= 0x20 && op <= 0x3F)
        {
            bool gouraud = (cmd & (1u << 28)) != 0, quad = (cmd & (1u << 27)) != 0;
            bool tex = (cmd & (1u << 26)) != 0, semi = (cmd & (1u << 25)) != 0, raw = (cmd & (1u << 24)) != 0;
            int n = quad ? 4 : 3;

            int idx = 1;
            long sx = 0, sy = 0;
            for (int i = 0; i < n; i++)
            {
                if (gouraud && i > 0)
                {
                    if (idx >= count) { _skipped++; return; }
                    idx++;   // a colour word, skipped
                }
                if (idx >= count) { _skipped++; return; }
                uint vw = W(idx++);
                sx += CoordX(vw); sy += CoordY(vw);
                if (tex)
                {
                    if (idx >= count) { _skipped++; return; }
                    uint uvw = W(idx++);
                    // Only the low half is a texture coordinate: the high half is the CLUT on
                    // vertex 0, the texpage on vertex 1, and PSY-Q's stale pad on 2 and 3.
                    h = Mix(h, uvw & 0xFFFFu);
                    if (i <= 1) h = Mix(h, uvw >> 16);
                }
            }

            _prims.Add(new Prim
            {
                Addr = node, Kind = kind, Id = id,
                Shape = n | (tex ? 16 : 0) | (gouraud ? 32 : 0) | (semi ? 64 : 0) | (raw ? 128 : 0),
                Hash = h, Keyed = tex, Cx = (float)sx / n, Cy = (float)sy / n,
            });
            return;
        }

        if (op >= 0x60 && op <= 0x7F)
        {
            bool tex = (cmd & (1u << 26)) != 0, semi = (cmd & (1u << 25)) != 0, raw = (cmd & (1u << 24)) != 0;
            int size = (int)((cmd >> 27) & 3u);

            int idx = 1;
            if (idx >= count) { _skipped++; return; }
            uint xy = W(idx++);
            if (tex)
            {
                if (idx >= count) { _skipped++; return; }
                h = Mix(h, W(idx++));
            }
            if (size == 0)
            {
                if (idx >= count) { _skipped++; return; }
                h = Mix(h, W(idx++));   // width and height: the sprite's own size
            }
            else h = Mix(h, (uint)size);

            _prims.Add(new Prim
            {
                Addr = node, Kind = kind, Id = id,
                Shape = 2 | (tex ? 16 : 0) | (semi ? 64 : 0) | (raw ? 128 : 0),
                Hash = h, Keyed = tex, Cx = CoordX(xy), Cy = CoordY(xy),
            });
            return;
        }

        _skipped++;
    }

    // ---------------------------------------------------------------- compare

    // Per kind: primitives, K1 matched, K2 (matched and same shape), K3 matched,
    // K3 keys that were not unique, and the summed displacement of the matched.
    static readonly long[] _tot = new long[3], _k1 = new long[3], _k2 = new long[3];
    static readonly long[] _k3 = new long[3], _coll = new long[3], _keyed = new long[3];

    // The same census restricted to primitives whose context was drawn last tick too.
    static readonly long[] _sTot = new long[3], _sK1 = new long[3], _sK3 = new long[3], _sKeyed = new long[3];

    // The pose split: how much of a matched primitive's motion is left once its context's mean is taken out.
    static readonly double[] _poseSum = new double[3], _meanSum = new double[3];
    static readonly long[] _poseN = new long[3], _ctxN = new long[3], _ctxPosed = new long[3];
    static long _poseTicks;

    // Of the primitives K1 matched, how many matched a face with the same texture coordinates.
    static readonly long[] _k1Right = new long[3], _k1Keyed = new long[3];
    static readonly double[] _disp1 = new double[3], _disp3 = new double[3];
    static long _ticks, _contexts, _contextsMatched;

    static readonly Dictionary<(int, uint), int> _ctxSeen = new(512);

    /// <summary>Which contexts drew anything last tick, so "this object is new" can be told from "the key broke".</summary>
    static HashSet<(int, uint)> _curCtx = new(512), _prevCtx = new(512);

    /// <summary>KF1_PACKETMATCH=2 only: per context, what each key matched.</summary>
    struct CtxTally { public long Tot, Keyed, K1, K3, Moved; public double Disp; }
    static readonly Dictionary<(int, uint), CtxTally> _ctxTally = new(512);

    static void Compare()
    {
        _curOrd.Clear();
        _curInt.Clear();
        _ctxSeen.Clear();
        _curCtx.Clear();
        _moves.Clear();

        foreach (var p in _prims)
        {
            var seen = new Seen { Shape = p.Shape, Hash = p.Hash, Cx = p.Cx, Cy = p.Cy };
            var kOrd = (p.Kind, p.Id, p.Ordinal);
            var kInt = (p.Kind, p.Id, p.Hash);

            _curOrd[kOrd] = seen;
            if (p.Keyed && !_curInt.TryAdd(kInt, seen)) _coll[p.Kind]++;

            if (!_primed) continue;

            bool survived = _prevCtx.Contains((p.Kind, p.Id));

            _tot[p.Kind]++;
            if (p.Keyed) _keyed[p.Kind]++;
            if (survived) { _sTot[p.Kind]++; if (p.Keyed) _sKeyed[p.Kind]++; }

            double moved = 0.0;
            if (_prevOrd.TryGetValue(kOrd, out var was))
            {
                _k1[p.Kind]++;
                if (survived) _sK1[p.Kind]++;
                // Matching is not the same as matching the right face: an ordinal survives a
                // cull that drops a polygon while naming a different triangle.
                if (p.Keyed) { _k1Keyed[p.Kind]++; if (was.Hash == p.Hash) _k1Right[p.Kind]++; }
                if (was.Shape == p.Shape) _k2[p.Kind]++;
                moved = Math.Sqrt((p.Cx - was.Cx) * (p.Cx - was.Cx) + (p.Cy - was.Cy) * (p.Cy - was.Cy));
                _disp1[p.Kind] += moved;
                _moves.Add(new Move { Kind = p.Kind, Id = p.Id, Dx = p.Cx - was.Cx, Dy = p.Cy - was.Cy });
            }
            if (p.Keyed && _prevInt.TryGetValue(kInt, out var wasI))
            {
                _k3[p.Kind]++;
                if (survived) _sK3[p.Kind]++;
                _disp3[p.Kind] += Math.Sqrt((p.Cx - wasI.Cx) * (p.Cx - wasI.Cx) + (p.Cy - wasI.Cy) * (p.Cy - wasI.Cy));
            }

            var ctx = (p.Kind, p.Id);
            if (_ctxSeen.TryAdd(ctx, 0))
            {
                _contexts++;
                if (_prevOrd.ContainsKey((p.Kind, p.Id, 0))) _contextsMatched++;
            }

            if (_verbose)
            {
                _ctxTally.TryGetValue(ctx, out var t);
                t.Tot++;
                if (_prevOrd.ContainsKey(kOrd)) { t.K1++; t.Moved++; t.Disp += moved; }
                if (p.Keyed) { t.Keyed++; if (_prevInt.ContainsKey(kInt)) t.K3++; }
                _ctxTally[ctx] = t;
            }
        }

        PoseSplit();
        foreach (var p in _prims) _curCtx.Add((p.Kind, p.Id));

        if (_primed) _ticks++;
        (_prevOrd, _curOrd) = (_curOrd, _prevOrd);
        (_prevInt, _curInt) = (_curInt, _prevInt);
        (_prevCtx, _curCtx) = (_curCtx, _prevCtx);
        _primed = true;
    }

    /// <summary>
    /// Take each context's mean displacement out of its primitives and measure what is left.
    /// Reported only for contexts whose mean is small, so a model sweeping across the screen
    /// does not report its own translation as pose.
    /// </summary>
    static void PoseSplit()
    {
        if (_moves.Count == 0 || !_camStill) return;
        _poseTicks++;
        _moves.Sort(static (a, b) => a.Kind != b.Kind ? a.Kind - b.Kind : a.Id.CompareTo(b.Id));

        int i = 0;
        while (i < _moves.Count)
        {
            int j = i;
            double sx = 0, sy = 0;
            while (j < _moves.Count && _moves[j].Kind == _moves[i].Kind && _moves[j].Id == _moves[i].Id)
            {
                sx += _moves[j].Dx; sy += _moves[j].Dy; j++;
            }
            int n = j - i;
            int kind = _moves[i].Kind;
            double mx = sx / n, my = sy / n;
            double mean = Math.Sqrt(mx * mx + my * my);

            _ctxN[kind]++;
            _meanSum[kind] += mean;

            // The camera held still, so what is left after the mean is pose. The bound keeps
            // a slot that was re-placed rather than moved out of the average.
            if (mean <= 32.0)
            {
                double dev = 0;
                for (int k = i; k < j; k++)
                {
                    double ex = _moves[k].Dx - mx, ey = _moves[k].Dy - my;
                    dev += Math.Sqrt(ex * ex + ey * ey);
                }
                _poseSum[kind] += dev;
                _poseN[kind] += n;
                if (dev / n > 0.5) _ctxPosed[kind]++;
            }
            i = j;
        }
    }

    // ------------------------------------------------------------ self-labels

    static int _lastYaw = -1;
    static long _lastX, _lastZ;
    static bool _haveLast;
    static double _yawSum, _moveSum;

    /// <summary>The camera did not move on this tick. Only then is a primitive's deviation from
    /// its context's mean unambiguously animation: with the eye translating, near and far faces
    /// move by different amounts and that parallax would be counted as pose.</summary>
    static bool _camStill;

    /// <summary>The turn rate and walking speed over the window, so a line says what the player was doing.</summary>
    static void Label(IMemory m)
    {
        int yaw = m.ReadU16(Yaw) & (AngleMod - 1);
        long x = (int)m.ReadU32(PosX), z = (int)m.ReadU32(PosZ);

        _camStill = false;
        if (_haveLast)
        {
            int d = (yaw - _lastYaw) & (AngleMod - 1);
            if (d > AngleMod / 2) d -= AngleMod;
            _yawSum += Math.Abs(d);
            double dx = x - _lastX, dz = z - _lastZ;
            _moveSum += Math.Sqrt(dx * dx + dz * dz);
            _camStill = d == 0 && dx == 0 && dz == 0;
        }
        _lastYaw = yaw; _lastX = x; _lastZ = z; _haveLast = true;
    }

    /// <summary>Cleared on an area change: every context id is new and a window straddling it would report a collapse.</summary>
    static void Forget()
    {
        _primed = false;
        _haveLast = false;
        _prevOrd.Clear();
        _prevInt.Clear();
        _prevCtx.Clear();
    }

    // ----------------------------------------------------------------- report

    static double _windowStart;
    static double Now => Environment.TickCount64 / 1000.0;

    static void Report()
    {
        double now = Now;
        if (now - _windowStart < 1.0) return;
        double secs = now - _windowStart;

        long tot = _tot[0] + _tot[1] + _tot[2];
        if (_ticks > 0 && tot > 0)
        {
            double deg = _yawSum / secs * 360.0 / AngleMod;
            Console.WriteLine($"[KF1] packetmatch: {_ticks} tick frame(s) in {secs:0.0}s, " +
                              $"{(double)tot / _ticks:0} prim/frame, " +
                              $"yaw {deg:0} deg/s, move {_moveSum / secs:0} u/s, " +
                              $"{_contexts / (double)_ticks:0.0} context/frame " +
                              $"({Pct(_contextsMatched, _contexts)} carried over)");

            for (int k = 0; k < 3; k++)
            {
                if (_tot[k] == 0) continue;
                Console.WriteLine($"[KF1]   {KindNames[k],-16} " +
                                  $"{(double)_tot[k] / _ticks,6:0} prim  " +
                                  $"K1 ordinal {Pct(_k1[k], _tot[k])}  " +
                                  $"K2 lerpable {Pct(_k2[k], _tot[k])}  " +
                                  $"K3 intrinsic {Pct(_k3[k], _keyed[k])}  " +
                                  $"collisions {Pct(_coll[k], _keyed[k])}  " +
                                  $"disp {Div(_disp1[k], _k1[k]):0.0}/{Div(_disp3[k], _k3[k]):0.0} px");
                Console.WriteLine($"[KF1]   {"K1 correctness",-16} " +
                                  $"{Pct(_k1Right[k], _k1Keyed[k])} of ordinal matches were the same face " +
                                  $"(by texture coordinates)");
                Console.WriteLine($"[KF1]   {"pose split",-16} " +
                                  $"{Div(_poseSum[k], _poseN[k]),6:0.00} px/tick left after the context mean " +
                                  $"({Div(_meanSum[k], _ctxN[k]):0.0} px), " +
                                  $"{Pct(_ctxPosed[k], _ctxN[k])} of contexts show any" +
                                  (_poseTicks == 0 ? "  [no still tick this window]" : $"  [{_poseTicks} still tick(s)]"));
                Console.WriteLine($"[KF1]   {"in surviving ctx",-16} " +
                                  $"{(double)_sTot[k] / _ticks,6:0} prim  " +
                                  $"K1 ordinal {Pct(_sK1[k], _sTot[k])}  " +
                                  $"{Pct(_tot[k] - _sTot[k], _tot[k])} of the row was newly on screen  " +
                                  $"K3 intrinsic {Pct(_sK3[k], _sKeyed[k])}");
            }

            if (_skipped > 0)
                Console.WriteLine($"[KF1]   {(double)_skipped / _ticks:0.0} non-drawing node(s) a frame, not counted");

            if (_verbose && _ctxTally.Count > 0)
            {
                // The contexts dragging the average down, worst first. A context that appeared
                // for one frame of the window will sit here legitimately: something walked into view.
                var worst = _ctxTally
                    .Where(e => e.Value.Tot >= 8)
                    .OrderBy(e => (double)e.Value.K1 / e.Value.Tot)
                    .Take(5);
                foreach (var (ctx, t) in worst)
                    Console.WriteLine($"[KF1]     worst {KindNames[ctx.Item1],-16} 0x{ctx.Item2:X8}  " +
                                      $"{t.Tot,5} prim  K1 {Pct(t.K1, t.Tot)}  K3 {Pct(t.K3, t.Keyed)}");

                // What moved. Read with the camera standing still, this names the thing that is animating.
                var movers = _ctxTally
                    .Where(e => e.Value.Moved >= 4 && e.Value.Disp / e.Value.Moved > 0.5)
                    .OrderByDescending(e => e.Value.Disp / e.Value.Moved)
                    .Take(5);
                foreach (var (ctx, t) in movers)
                    Console.WriteLine($"[KF1]     moved {KindNames[ctx.Item1],-16} 0x{ctx.Item2:X8}  " +
                                      $"{t.Tot,5} prim  {t.Disp / t.Moved,6:0.0} px/tick  K1 {Pct(t.K1, t.Tot)}");
            }
        }

        Array.Clear(_tot); Array.Clear(_k1); Array.Clear(_k2);
        Array.Clear(_k3); Array.Clear(_coll); Array.Clear(_keyed);
        Array.Clear(_sTot); Array.Clear(_sK1); Array.Clear(_sK3); Array.Clear(_sKeyed);
        Array.Clear(_poseSum); Array.Clear(_meanSum);
        Array.Clear(_poseN); Array.Clear(_ctxN); Array.Clear(_ctxPosed);
        _poseTicks = 0;
        Array.Clear(_k1Right); Array.Clear(_k1Keyed);
        Array.Clear(_disp1); Array.Clear(_disp3);
        _ctxTally.Clear();
        _ticks = 0; _contexts = 0; _contextsMatched = 0; _skipped = 0;
        _yawSum = 0; _moveSum = 0;
        _windowStart = now;
    }

    static string Pct(long hit, long of) => of == 0 ? "  n/a" : $"{100.0 * hit / of,5:0.0}%";
    static double Div(double a, long b) => b == 0 ? 0.0 : a / b;

    // One distinct type per routine, so each pre/post pair is its own delegate.
    struct M0; struct M1; struct M2; struct M3; struct M4; struct M5;
    static readonly Type[] Markers = [typeof(M0), typeof(M1), typeof(M2), typeof(M3), typeof(M4), typeof(M5)];
}
