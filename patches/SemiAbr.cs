using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// KF1_SEMI_ABR=1: the blend mode (ABR) each untextured semi-transparent model face's
/// packet draws with. The packets are tagged by the polygon writer (<see cref="Emit"/>,
/// keyed by the mesh that built them), and <c>DrawOTag</c> walks the table before the
/// runtime does, in the order <c>LibGpu.WalkOTag</c> walks it, following the GP0 state:
/// E1 sets the mode, and a textured polygon sets it from its texpage (the second
/// vertex's UV word). The end state is checked against the GPU's own field at the
/// next walk. A line every 2 s. See "Semi-transparent model faces" in
/// <c>docs/GAME_INTERNALS.md</c>.
/// </summary>
public static class SemiAbr
{
    static bool _on;
    public static bool On => _on;
    public static void Configure(string? on) => _on = on?.Trim() == "1";

    /// <summary>The semi mesh whose writer call is running, or null (set by RetainedModels.Submit).</summary>
    public static (uint Header, ulong Hash)? Current;

    static readonly Dictionary<uint, (uint H, ulong K)> Tagged = new();
    static readonly Dictionary<(int Area, uint H, ulong K), long[]> Stats = new();
    static readonly FieldInfo? ModeField =
        typeof(RecompOne.Runtime.Gpu).GetField("_blendMode", BindingFlags.NonPublic | BindingFlags.Instance);
    static int _expect = -1;
    static long _walks, _endMismatch, _e1, _tex, _tagged, _found, _tagIn;
    static int _sample;
    static double _last;

    static readonly ModInfo _self = new()
    {
        Id = "kf1.semiabr",
        Name = "Semi-transparent ABR probe",
        Version = "1.0",
    };

    public static void Install()
    {
        if (!_on) return;
        HookAttach.OnOverlayLoad("semi abr", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var target = SymbolRegistry.Resolve("game", null, 0x80050CF0);
        if (target == null) { Console.Error.WriteLine("[KF1] semi abr: no game/0x80050CF0"); return false; }
        var pre = typeof(SemiAbr).GetMethod(nameof(Before), BindingFlags.Public | BindingFlags.Static)!;
        bool ok = HookManager.AddPre(_self, target, pre);
        HookManager.Commit();
        Console.WriteLine($"[KF1] semi abr: DrawOTag hooked={ok}, blend order active={BlendOrder.Active}, " +
                          $"gpu field={(ModeField == null ? "missing" : "ok")}");
        return ok;
    }

    static uint Norm(uint a) => (a & 0x1FFFFFu) | 0x80000000u;

    // The blended-face keys (KF1_GPU_CHECK): the slots the retained path expects for the
    // semi mesh being written, and the packets the writer links for it, in the same order.
    static readonly Queue<int> _keys = new();
    static bool _keyActive;
    public static long KeyChecked, KeyMismatch, KeyExtra, KeyMissing;

    /// <summary>At each model's submit: the previous object's expectations not met are missing.</summary>
    public static void Begin()
    {
        Current = null;
        KeyMissing += _keys.Count;
        _keys.Clear();
        _keyActive = false;
    }

    /// <summary>The slots (table index) a semi mesh's writer links, in its prim order.</summary>
    public static void Expect(List<int> slots)
    {
        if (!GpuWorld.Check) return;
        foreach (int s in slots) _keys.Enqueue(s);
        _keyActive = true;
    }

    /// <summary>The polygon writer, at each packet it links: a semi mesh's untextured face is
    /// tagged for the probe, and under the check its linked slot is compared with the expected one.
    /// <paramref name="linked"/> is the OT entry address the writer links it at.</summary>
    public static void Emit(uint pkt, uint primWord, uint linked, IMemory m)
    {
        uint code = primWord >> 24;
        if (code is < 0x20 or > 0x3F || (code & 4) != 0 || (code & 2) == 0) return;
        if (_on && Current is { } cur)
        {
            Tagged[Norm(pkt)] = (cur.Header, cur.Hash);
            _tagged++;
        }
        if (!_keyActive) return;
        uint ot = m.ReadU32(0x80090EBCu);
        int index = (int)(((linked - ot) >> 2) & 0x3FFF);
        KeyChecked++;
        if (_keys.Count == 0) { KeyExtra++; return; }
        int want = _keys.Dequeue();
        if (want != index)
        {
            KeyMismatch++;
            if (KeyMismatch <= 8) Console.WriteLine($"[KF1-KEY] expected {want} linked {index} raw {linked:X8} ot {ot:X8}");
        }
    }

    public static string KeyReport() =>
        $"semi keys {KeyChecked}: slot mismatch {KeyMismatch}, extra packets {KeyExtra}, missing {KeyMissing + _keys.Count}";

    /// <summary>DrawOTag's entry: walks the table as the runtime will, with the GPU's state.</summary>
    public static void Before(CpuContext c, IMemory m)
    {
        if (!_on) return;
        var gpu = RecompOne.Runtime.Runtime.Gpu;
        if (gpu == null || ModeField == null) return;
        int mode = (int)ModeField.GetValue(gpu)!;
        if (_expect >= 0 && mode != _expect) _endMismatch++;
        _walks++;
        int area = m.ReadU8(0x800A078Au);

        uint addr = Norm(c.A0);
        for (int guard = 0; guard < 0x100000; guard++)
        {
            uint header = m.ReadU32(addr);
            int count = (int)(header >> 24);
            if (count > 0)
            {
                if (Tagged.TryGetValue(addr, out var tag))
                {
                    Record(area, tag, mode);
                    _found++;
                }
                mode = Apply(m, addr, mode);
            }
            uint next = header & 0xFFFFFFu;
            if (next == 0xFFFFFFu || (next & 0x800000u) != 0) break;
            addr = Norm(next);
        }
        _tagIn += Tagged.Count;
        Tagged.Clear();
        _expect = mode;

        double now = Environment.TickCount64 / 1000.0;
        if (now - _last >= 2.0) { _last = now; Report(); }
    }

    /// <summary>The GP0 word k of a packet (k = 0 is the command).</summary>
    static uint W(IMemory m, uint addr, uint k) => m.ReadU32(addr + 4u + 4u * k);

    /// <summary>The mode after a packet's command, as the runtime's GP0 parse sets it.</summary>
    static int Apply(IMemory m, uint addr, int mode)
    {
        uint cmd = W(m, addr, 0);
        uint code = cmd >> 24;
        if (code == 0xE1)
        {
            _e1++;
            return (int)((cmd >> 5) & 3);
        }
        if (code is < 0x20 or > 0x3F || (code & 4) == 0) return mode;
        bool gouraud = (code & 0x10) != 0;
        int n = (code & 8) != 0 ? 4 : 3;
        uint idx = 1;
        for (int i = 0; i < n; i++)
        {
            if (gouraud && i > 0) idx++;
            idx++;
            uint uvw = W(m, addr, idx);
            idx++;
            if (i == 1)
            {
                _tex++;
                uint tp = (uvw >> 16) & 0xFFFFu;
                return (int)((tp >> 5) & 3);
            }
        }
        return mode;
    }

    static void Record(int area, (uint H, ulong K) tag, int mode)
    {
        var key = (area, tag.H, tag.K);
        if (!Stats.TryGetValue(key, out var s)) Stats[key] = s = new long[4];
        s[mode & 3]++;
    }

    static void Report()
    {
        Console.WriteLine($"[KF1-ABR] walks {_walks}, end-state mismatches {_endMismatch}, E1 {_e1}, " +
                          $"texpage sets {_tex}, tagged packets {_tagged}, tags entering walks {_tagIn}, found {_found}");
        foreach (var kv in Stats.OrderBy(k => k.Key.Area).ThenBy(k => k.Key.H))
        {
            var s = kv.Value;
            Console.WriteLine($"[KF1-ABR]   area {kv.Key.Area} mesh {kv.Key.H:X8}/{kv.Key.K:X16}: " +
                              $"abr0 {s[0]} abr1 {s[1]} abr2 {s[2]} abr3 {s[3]}");
        }
    }
}
