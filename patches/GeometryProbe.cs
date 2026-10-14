using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// KF1_GEOPROBE=1: what each of the renderer's drawing calls adds to the ordering
/// table, by GPU packet type. The table is walked before and after each call and the
/// packets new to it counted; one block every five seconds. Only calls made from
/// inside stage I (func_8001FDE4) count. The renderer is also called from modal
/// loops, so its calls are split into main-loop and other frames in the header.
/// See "The renderer in C#" in docs/GAME_INTERNALS.md.
/// </summary>
public static class GeometryProbe
{
    const uint Renderer = 0x8001FDE4;
    const uint MainLoopRenderer = 0x800148C0;   // the jal into stage I from the main loop, plus 8
    const uint OtPointer = 0x80090EBC;
    const int OtWords = 0x4000;
    const int MaxPackets = 1 << 17;

    // The renderer's drawing callees, with the return address each call site leaves in
    // RA (the same values patches/Renderer.cs makes its calls with).
    static readonly (string Label, uint Callee, uint[] Returns)[] Sites =
    [
        ("map walk", 0x8001E83C, [0x8001FE20]),
        ("compass", 0x8001F8B0, [0x80020134]),
        ("HUD sprites", 0x8001F9D4, [0x8002017C]),
        ("selected item", 0x8001FAFC, [0x80020194]),
        ("HUD models", 0x8001E230, [0x80020238, 0x8002025C, 0x800202A8]),
        ("models", 0x8001F218, [0x800202C8]),
        ("arm", 0x8001F798, [0x800202D0]),
    ];

    sealed class Row
    {
        public string Label = "";
        public long Calls, Words, Removed;
        public readonly Dictionary<string, long> Types = new();
    }

    static readonly ModInfo _self = new()
    {
        Id = "kf1.geoprobe",
        Name = "Geometry probe",
        Version = "1.0",
        Description = "Counts the packets each renderer draw call adds to the ordering table.",
    };

    static bool _on, _queued;
    static readonly List<Row> _rows = new();
    static readonly Dictionary<uint, int> _rowByReturn = new();
    static readonly Stack<(int Row, HashSet<uint>? Before)> _calls = new();
    static readonly Dictionary<string, long> _nonPoly = new();
    static int _depth;
    static long _mainFrames, _otherFrames;
    static long _windowStart = -1;

    public static void Configure(string? on) => _on = on?.Trim() == "1";

    public static void Install()
    {
        if (!_on) return;
        for (int i = 0; i < Sites.Length; i++)
        {
            _rows.Add(new Row { Label = Sites[i].Label });
            foreach (uint r in Sites[i].Returns) _rowByReturn[r] = i;
        }
        HookAttach.OnOverlayLoad("geoprobe", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var self = typeof(GeometryProbe);
        var pre = self.GetMethod(nameof(Pre), BindingFlags.Public | BindingFlags.Static)!;
        var post = self.GetMethod(nameof(Post), BindingFlags.Public | BindingFlags.Static)!;
        var rpre = self.GetMethod(nameof(RendererPre), BindingFlags.Public | BindingFlags.Static)!;
        var rpost = self.GetMethod(nameof(RendererPost), BindingFlags.Public | BindingFlags.Static)!;

        var renderer = SymbolRegistry.Resolve("game", null, Renderer);
        if (renderer == null) { Console.Error.WriteLine($"[KF1] geoprobe: no game/0x{Renderer:X8}"); return false; }

        // E230 is one function with three call sites; one pair of hooks serves them all.
        var callees = Sites.Select(s => s.Callee).Distinct().ToList();
        var targets = new List<MethodInfo>();
        foreach (uint a in callees)
        {
            var t = SymbolRegistry.Resolve("game", null, a);
            if (t == null) { Console.Error.WriteLine($"[KF1] geoprobe: no game/0x{a:X8}"); return false; }
            targets.Add(t);
        }
        if (!_queued)
        {
            HookManager.AddPre(_self, renderer, rpre);
            HookManager.AddPost(_self, renderer, rpost);
            foreach (var t in targets)
            {
                HookManager.AddPre(_self, t, pre);
                HookManager.AddPost(_self, t, post);
            }
            _queued = true;
        }
        HookManager.Commit();
        int n = targets.Count(HookAttach.Installed) + (HookAttach.Installed(renderer) ? 1 : 0);
        Console.WriteLine($"[KF1] geoprobe: {n}/{targets.Count + 1} functions hooked");
        return n == targets.Count + 1;
    }

    // stage I brackets the calls. Its own pre and post only count, and report.
    public static void RendererPre(CpuContext c, IMemory m)
    {
        _depth++;
        if (c.RA - 8u == MainLoopRenderer) _mainFrames++; else _otherFrames++;
    }

    public static void RendererPost(CpuContext c, IMemory m)
    {
        if (_depth > 0) _depth--;
        Report();
    }

    // A callee's hooks: the call site is its return address, so the same function
    // called from elsewhere in the renderer (the submitters' E230) is not counted.
    public static void Pre(CpuContext c, IMemory m)
    {
        int row = _depth > 0 && _rowByReturn.TryGetValue(c.RA, out int r) ? r : -1;
        HashSet<uint>? before = null;
        if (row >= 0)
        {
            before = new HashSet<uint>();
            EachPacket(m, (p, _) => before.Add(p));
        }
        _calls.Push((row, before));
    }

    public static void Post(CpuContext c, IMemory m)
    {
        if (_calls.Count == 0) return;
        var (row, before) = _calls.Pop();
        if (row < 0 || before == null) return;
        var r = _rows[row];
        r.Calls++;
        int kept = 0;
        EachPacket(m, (p, tag) =>
        {
            if (before.Contains(p)) { kept++; return; }
            Classify(m, p, tag, r);
        });
        r.Removed += before.Count - kept;
    }

    // Counts one new packet: its words, and its type, by the first word after the header.
    static void Classify(IMemory m, uint p, uint tag, Row r)
    {
        int len = (int)(tag >> 24);
        r.Words += len;
        string kind;
        bool poly = false;
        if (len == 0) kind = "empty";
        else
        {
            uint code = m.ReadU32(p + 4) >> 24;
            poly = code is >= 0x20 and <= 0x3F;
            kind = Kind(code);
        }
        r.Types[kind] = r.Types.GetValueOrDefault(kind) + 1;
        if (!poly) _nonPoly[kind] = _nonPoly.GetValueOrDefault(kind) + 1;
    }

    // Polygons by F/G, T, 3/4 corners. Semi-transparency is read as Verdite3's
    // RetainedAssets does: bit 1, on textured polygons only (S suffix). GP0 sets bit 1
    // on untextured semi-transparent polygons too, which this does not flag.
    static string Kind(uint code) => code switch
    {
        >= 0x20 and <= 0x3F => $"{((code & 0x10) != 0 ? "G" : "F")}{((code & 0x04) != 0 ? "T" : "")}" +
                               $"{((code & 0x08) != 0 ? "4" : "3")}{((code & 0x04) != 0 && (code & 0x02) != 0 ? "S" : "")}",
        >= 0x40 and <= 0x5F => "line",
        >= 0x60 and <= 0x7F => "sprite",
        >= 0xE1 and <= 0xE6 => "E1-6",
        _ => $"other {code:X2}",
    };

    // Every packet hanging from the table, from its last entry down the chain to the
    // terminator (an empty slot links to the one below it). Table entries are skipped.
    static void EachPacket(IMemory m, Action<uint, uint> each)
    {
        uint ot = m.ReadU32(OtPointer);
        if (ot < 0x80000000u || ot - 0x80000000u + OtWords * 4u > 0x200000u) return;
        uint lo = ot, hi = ot + OtWords * 4u;
        uint p = hi - 4u;
        for (int guard = 0; guard < MaxPackets; guard++)
        {
            uint tag = m.ReadU32(p);
            if (p < lo || p >= hi) each(p, tag);
            uint next = tag & 0xFFFFFFu;
            if (next == 0xFFFFFFu) break;
            p = 0x80000000u | next;
        }
    }

    static void Report()
    {
        long now = Environment.TickCount64;
        if (_windowStart < 0) { _windowStart = now; return; }
        double s = (now - _windowStart) / 1000.0;
        if (s < 5.0) return;
        string np = _nonPoly.Count == 0 ? "none"
            : string.Join(" ", _nonPoly.OrderByDescending(kv => kv.Value).Select(kv => $"{kv.Key} {kv.Value}"));
        Console.WriteLine($"[KF1] geoprobe: {(_mainFrames + _otherFrames) / s:0.0} renderer calls/s " +
                          $"({_mainFrames / s:0.0} main loop, {_otherFrames / s:0.0} modal or other); " +
                          $"non-polygon packets new in the window: {np}");
        foreach (var r in _rows)
        {
            double calls = Math.Max(1, r.Calls);
            string types = string.Join(" ", r.Types.OrderByDescending(kv => kv.Value).Take(8)
                .Select(kv => $"{kv.Key} {kv.Value / calls:0.0}"));
            Console.WriteLine($"[KF1] geoprobe:   {r.Label} {r.Calls / s:0.0}/s: {(types.Length == 0 ? "no packets" : types)}; " +
                              $"{r.Words / calls:0} words/call, {r.Words} total" +
                              (r.Removed > 0 ? $", {r.Removed} removed" : ""));
            r.Calls = r.Words = r.Removed = 0;
            r.Types.Clear();
        }
        _nonPoly.Clear();
        _mainFrames = _otherFrames = 0;
        _windowStart = now;
    }
}
