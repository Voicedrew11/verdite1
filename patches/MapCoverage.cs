using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// KF1_MAPCOVERAGE=1: how the GTE vertex address map covers the frame, by the
/// routine that wrote each packet. Pre/post hooks on the packet writers record the
/// range of the primitive buffer each call bumps (the record at 0x80070EB8, cursor
/// at +8). A pre on the frame head walks the table about to be flipped and asks
/// <see cref="GteVertexMap.Peek"/> about each polygon's vertex words, without moving
/// the map's counters. One line a row every 2 s. The map has to be on: KF1_ZBUFFER,
/// KF1_PERSPECTIVE or KF1_SUBPIXEL. See "Unit 3" in docs/PICTURE.md.
/// </summary>
public static class MapCoverage
{
    // The frame's packet record and the ordering table it points at (see docs/GPU_RENDERER.md).
    const uint RecordPointer = 0x80070EB8;
    const uint OtPointer = 0x80090EBC;
    const int OtWords = 0x4000;

    // FrameHead: flips the buffer index, so the table it is about to flip is the frame just drawn.
    const uint FrameHead = 0x8001BFB8;

    enum Owner { Map, Objects, Submit, Arm, Compass, Sprite, HudSprite, Other, Count }

    static readonly string[] _names =
        ["map", "objects", "submit", "arm", "compass", "sprite", "hud sprite", "other"];

    // A writer bumps the cursor; a flag counts the calls it is inside, so the submitter
    // (func_8001C7F8) is named by the arm or the compass that called it.
    sealed class Target
    {
        public uint Addr;
        public Owner Row;
        public bool Flag;    // not a writer: only tells the submitter who called it
        public bool Split;   // the submitter: the arm or the compass picks the row
    }

    static readonly Target[] _targets =
    [
        new() { Addr = 0x8001DE18, Row = Owner.Map },          // map writer, reached from E83C
        new() { Addr = 0x8001D730, Row = Owner.Objects },      // object writer, from E9A4
        new() { Addr = 0x8001C7F8, Row = Owner.Submit, Split = true },
        new() { Addr = 0x8001E230, Row = Owner.Sprite },       // sprites, creatures, HUD models
        new() { Addr = 0x8001E480, Row = Owner.HudSprite },
        new() { Addr = 0x8001F798, Row = Owner.Arm, Flag = true },
        new() { Addr = 0x8001F8B0, Row = Owner.Compass, Flag = true },
    ];

    // One writer's range: [Lo, Hi) of the primitive buffer, physical.
    struct Span { public uint Lo, Hi; public Owner Row; }

    static readonly List<Span> _spans = [];
    static readonly uint[] _start = new uint[_targets.Length];
    static readonly bool[] _have = new bool[_targets.Length];
    static readonly Owner[] _pending = new Owner[_targets.Length];
    static readonly int[] _inFlag = new int[(int)Owner.Count];

    static readonly long[] _packets = new long[(int)Owner.Count];
    static readonly long[] _vertices = new long[(int)Owner.Count];
    static readonly long[] _hits = new long[(int)Owner.Count];

    static bool _on, _warned;
    static long _windowStart = -1;

    static readonly ModInfo _self = new()
    {
        Id = "kf1.mapcov",
        Name = "Address map coverage",
        Version = "1.0",
        Description = "The GTE vertex map's coverage, by packet-writing routine.",
    };

    public static void Configure(string? on) => _on = on?.Trim() == "1";

    public static void Install()
    {
        if (!_on) return;
        HookAttach.OnOverlayLoad("mapcov", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var targets = new List<MethodInfo>();

        for (int i = 0; i < _targets.Length; i++)
        {
            var t = SymbolRegistry.Resolve("game", null, _targets[i].Addr);
            if (t == null) { Console.Error.WriteLine($"[KF1] mapcov: no game/0x{_targets[i].Addr:X8}"); continue; }
            var slot = typeof(Slot<>).MakeGenericType(Markers[i]);
            slot.GetField(nameof(Slot<int>.Id))!.SetValue(null, i);
            HookManager.AddPre(_self, t, slot.GetMethod(nameof(Slot<int>.Pre))!);
            HookManager.AddPost(_self, t, slot.GetMethod(nameof(Slot<int>.Post))!);
            targets.Add(t);
        }

        var head = SymbolRegistry.Resolve("game", null, FrameHead);
        if (head != null)
        {
            HookManager.AddPre(_self, head, typeof(MapCoverage).GetMethod(nameof(BeforeFrameHead), BindingFlags.Public | BindingFlags.Static)!);
            targets.Add(head);
        }
        else Console.Error.WriteLine($"[KF1] mapcov: no game/0x{FrameHead:X8}");

        HookManager.Commit();
        int n = targets.Count(HookAttach.Installed);
        Console.WriteLine($"[KF1] mapcov: {n}/{_targets.Length + 1} function(s) hooked");
        return n == _targets.Length + 1;
    }

    static uint Mask => RecompOne.Runtime.Runtime.RamWordMask;

    // The cursor of this frame's buffer: the record points at the frame struct, +8 is the cursor.
    static uint Cursor(IMemory m)
    {
        uint rec = m.ReadU32(RecordPointer);
        return rec < 0x80000000u ? 0u : m.ReadU32(rec + 8u) & Mask;
    }

    // ---- the packet writers and flags ----------------------------------------

    public static void Pre(int target, CpuContext c, IMemory m)
    {
        var t = _targets[target];
        if (t.Flag) { _inFlag[(int)t.Row]++; return; }
        _start[target] = Cursor(m);
        _pending[target] = RowFor(t);
        _have[target] = true;
    }

    public static void Post(int target, CpuContext c, IMemory m)
    {
        var t = _targets[target];
        if (t.Flag) { if (_inFlag[(int)t.Row] > 0) _inFlag[(int)t.Row]--; return; }
        if (!_have[target]) return;
        uint end = Cursor(m);
        if (end > _start[target]) _spans.Add(new Span { Lo = _start[target], Hi = end, Row = _pending[target] });
        _have[target] = false;
    }

    static Owner RowFor(Target t)
    {
        if (!t.Split) return t.Row;
        return _inFlag[(int)Owner.Arm] > 0 ? Owner.Arm
             : _inFlag[(int)Owner.Compass] > 0 ? Owner.Compass
             : Owner.Submit;
    }

    // ---- the frame boundary and the walk -------------------------------------

    public static void BeforeFrameHead(CpuContext c, IMemory m)
    {
        if (!_on) return;
        Walk(m);
        _spans.Clear();
        Array.Clear(_have);
        Array.Clear(_inFlag);
        Report();
    }

    // Every packet hanging from the table, from its last entry down the chain (an empty
    // slot links to the one below it), as GeometryProbe walks it.
    static void Walk(IMemory m)
    {
        uint ot = m.ReadU32(OtPointer);
        if (ot < 0x80000000u || ot - 0x80000000u + OtWords * 4u > 0x200000u) return;
        uint lo = ot, hi = ot + OtWords * 4u;
        uint p = hi - 4u;
        for (int guard = 0; guard < 1 << 17; guard++)
        {
            uint tag = m.ReadU32(p);
            if ((p < lo || p >= hi) && (tag >> 24) != 0) Count(m, p & Mask);
            uint next = tag & 0xFFFFFFu;
            if (next == 0xFFFFFFu) break;
            p = 0x80000000u | next;
        }
    }

    // A polygon's vertex words: the xy word of each corner, after the command word.
    // bit 26 textured (a uv word per corner), bit 28 shaded (a colour word per corner).
    static void Count(IMemory m, uint pkt)
    {
        uint body = pkt + 4u;
        uint cmd = m.ReadU32(body) >> 24;
        if (cmd < 0x20u || cmd > 0x3Fu) return;

        int row = (int)OwnerOf(pkt);
        _packets[row]++;

        int n = (cmd & 0x08u) != 0 ? 4 : 3;
        bool gouraud = (cmd & 0x10u) != 0;
        bool tex = (cmd & 0x04u) != 0;
        uint off = 4u;
        for (int k = 0; k < n; k++)
        {
            if (k > 0) off += 4u + (tex ? 4u : 0u) + (gouraud ? 4u : 0u);
            uint at = (body + off) & Mask;
            uint word = m.ReadU32(at);
            _vertices[row]++;
            if (GteVertexMap.Peek(at, word, out _)) _hits[row]++;
        }
    }

    // The innermost writer whose range holds the packet; the table's own entries are skipped
    // by the walk, and a packet no writer wrote this frame is Other.
    static Owner OwnerOf(uint pkt)
    {
        Owner best = Owner.Other;
        uint bestLo = 0;
        bool found = false;
        foreach (var s in _spans)
        {
            if (pkt < s.Lo || pkt >= s.Hi) continue;
            if (!found || s.Lo > bestLo) { best = s.Row; bestLo = s.Lo; found = true; }
        }
        return best;
    }

    static void Report()
    {
        long now = Environment.TickCount64;
        if (_windowStart < 0) { _windowStart = now; return; }
        double s = (now - _windowStart) / 1000.0;
        if (s < 2.0) return;

        if (!_warned && !GteVertexMap.Active)
        {
            _warned = true;
            Console.Error.WriteLine("[KF1] mapcov: the address map is off -- enable KF1_ZBUFFER, KF1_PERSPECTIVE or KF1_SUBPIXEL");
        }

        for (int i = 0; i < (int)Owner.Count; i++)
        {
            double v = _vertices[i];
            double hit = v == 0.0 ? 0.0 : 100.0 * _hits[i] / v;
            Console.WriteLine($"[KF1] mapcov {_names[i]}: {_packets[i] / s:F0}/s, {v / s:F0}/s, {hit:F1}%.");
            _packets[i] = _vertices[i] = _hits[i] = 0;
        }
        Console.Out.Flush();
        _windowStart = now;
    }

    // One distinct pair of methods per target, so the same delegate is not shared.
    struct M0; struct M1; struct M2; struct M3; struct M4; struct M5; struct M6;

    static readonly Type[] Markers =
    [
        typeof(M0), typeof(M1), typeof(M2), typeof(M3), typeof(M4), typeof(M5), typeof(M6),
    ];

    static class Slot<T>
    {
        public static int Id;
        public static void Pre(CpuContext c, IMemory m) => MapCoverage.Pre(Id, c, m);
        public static void Post(CpuContext c, IMemory m) => MapCoverage.Post(Id, c, m);
    }
}
