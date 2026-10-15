using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// One routine of the draw path transcribed literally into C#: the same reads and
/// stores in the same order, each callee called directly so its hooks still fire.
///
///     KF1_LITERAL=1|0|verify   every routine below at once (default 1)
///     KF1_&lt;NAME&gt;=1|0|verify    one routine, over KF1_LITERAL
///
/// Verify runs the recompiled routine, then the transcription from the same state,
/// and compares all of RAM outside a stack window under the call, the scratchpad,
/// every register with LO and HI, the GTE and the interrupt poll's countdown (so a
/// loop that polls a different number of times shows). A call in which either run
/// reached a slow poll is counted, not compared: its handlers read the clock. **The
/// recompiled result stands.** Step 1 of docs/RENDERER_PLAN.md; see "The draw path
/// in C#" in docs/GPU_RENDERER.md.
/// </summary>
public sealed class Literal
{
    enum Mode { Off, On, Verify }

    public static readonly List<Literal> All = [];

    public string Name { get; }
    public string Env { get; }
    public uint Address { get; }

    readonly Type _owner;
    readonly Action<CpuContext, PSMemory> _body;
    readonly string _tip;
    Mode _mode = Mode.On;
    bool _queued, _hooked;

    /// <summary>How many times the transcription has run in place of the routine.</summary>
    public long Runs;

    readonly ModInfo _self;

    /// <param name="owner">The class holding a static <c>Replace(orig, c, m)</c> that forwards here.</param>
    public Literal(Type owner, string name, string env, uint address, Action<CpuContext, PSMemory> body, string tip)
    {
        _owner = owner;
        Name = name;
        Env = env;
        Address = address;
        _body = body;
        _tip = tip;
        _self = new ModInfo
        {
            Id = "kf1.literal." + env.ToLowerInvariant(),
            Name = name,
            Version = "1.0",
            Description = $"func_{address:X8} in C#.",
        };
        All.Add(this);
    }

    public string Tip => _tip;

    /// <summary>The mode as the Testing tab sets it: 0 recompiled, 1 C#, 2 verify.</summary>
    public int Setting { get => (int)_mode; set => _mode = (Mode)Math.Clamp(value, 0, 2); }

    public bool InCSharp => _mode == Mode.On;

    static Mode Parse(string? s, Mode fallback) => s?.Trim().ToLowerInvariant() switch
    {
        "0" or "off" => Mode.Off,
        "1" or "on" => Mode.On,
        "verify" => Mode.Verify,
        _ => fallback,
    };

    /// <summary>Read KF1_LITERAL and each routine's own variable, and attach them all.</summary>
    public static void InstallAll()
    {
        // Touch every routine's class so its Literal registers itself.
        foreach (var t in typeof(Literal).Assembly.GetTypes())
            if (t.Namespace == "Kf1" && t.GetField("R", BindingFlags.Public | BindingFlags.Static)?.FieldType == typeof(Literal))
                System.Runtime.CompilerServices.RuntimeHelpers.RunClassConstructor(t.TypeHandle);
        All.Sort((a, b) => a.Address.CompareTo(b.Address));

        var all = Parse(Environment.GetEnvironmentVariable("KF1_LITERAL"), Mode.On);
        foreach (var r in All) r._mode = Parse(Environment.GetEnvironmentVariable("KF1_" + r.Env), all);
        HookAttach.OnOverlayLoad("literal routines", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        int n = 0;
        foreach (var r in All)
        {
            var target = SymbolRegistry.Resolve("game", null, r.Address);
            if (target == null) continue;
            if (!r._queued)
            {
                var impl = r._owner.GetMethod("Replace", BindingFlags.NonPublic | BindingFlags.Public | BindingFlags.Static);
                if (impl == null) { Console.Error.WriteLine($"[KF1] literal {r.Name}: no Replace"); continue; }
                r._queued = HookManager.AddReplace(r._self, target, impl);
            }
        }
        HookManager.Commit();
        foreach (var r in All)
        {
            var target = SymbolRegistry.Resolve("game", null, r.Address);
            r._hooked = target != null && HookAttach.Installed(target);
            if (r._hooked) n++;
        }
        var modes = All.GroupBy(r => r._mode).Select(g => $"{g.Count()} {g.Key.ToString().ToLowerInvariant()}");
        Console.WriteLine($"[KF1] literal routines: {n} of {All.Count} installed ({string.Join(", ", modes)})");
        return n == All.Count;
    }

    /// <summary>The owner's replace hook forwards here.</summary>
    public void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        // PGXP's RAM shadow is kept by the recompiled stores, which C# stores skip.
        if (_mode == Mode.Off || RecompOne.Runtime.Pgxp.Pgxp.CpuTracking || m is not PSMemory mem)
        {
            orig(c, m);
            return;
        }
        if (_mode == Mode.Verify) Verify(orig, c, mem);
        else
        {
            Runs++;
            _body(c, mem);
        }
    }

    // ---- verify --------------------------------------------------------------------

    const uint StackWindow = 0x2000;
    static readonly FieldInfo? Countdown =
        typeof(Interrupts).GetField("_countdown", BindingFlags.NonPublic | BindingFlags.Static);

    byte[] _before = [], _theirs = [];
    readonly uint[] _padBefore = new uint[Differential.PadWords], _padTheirs = new uint[Differential.PadWords],
                    _padOurs = new uint[Differential.PadWords];
    readonly Gte.State _gteEntry = new(), _gteTheirs = new(), _gteOurs = new();
    long _calls, _bad, _interrupted;
    readonly List<string> _samples = [];
    double _reportAt;

    void Verify(Action<CpuContext, IMemory> orig, CpuContext c, PSMemory mem)
    {
        var ram = Differential.Ram(mem);
        if (_before.Length != ram.Length)
        {
            _before = new byte[ram.Length];
            _theirs = new byte[ram.Length];
        }

        uint a0 = c.A0, a1 = c.A1, ra = c.RA;
        ram.CopyTo(_before);
        Differential.SavePad(mem, _padBefore);
        var entry = c.Snapshot();
        Gte.Save(_gteEntry);
        int countdown = (int?)Countdown?.GetValue(null) ?? 0;
        uint slow = Interrupts.SlowPolls;

        orig(c, mem);
        ram.CopyTo(_theirs);
        Differential.SavePad(mem, _padTheirs);
        var theirs = c.Snapshot();
        Gte.Save(_gteTheirs);
        int countTheirs = (int?)Countdown?.GetValue(null) ?? 0;
        uint slowTheirs = Interrupts.SlowPolls;

        _before.CopyTo(ram);
        Differential.LoadPad(mem, _padBefore);
        c.Restore(entry);
        Gte.Load(_gteEntry);
        Countdown?.SetValue(null, countdown);
        _body(c, mem);
        Differential.SavePad(mem, _padOurs);
        var ours = c.Snapshot();
        Gte.Save(_gteOurs);
        int countOurs = (int?)Countdown?.GetValue(null) ?? 0;
        uint slowOurs = Interrupts.SlowPolls;

        _calls++;
        string at = $"a0={a0:X} a1={a1:X} ra={ra:X}";
        if (slowTheirs != slow || slowOurs != slowTheirs) _interrupted++;
        else
        {
            string? why = null;
            int hi = (int)(entry.SP & (uint)(ram.Length - 1));
            int lo = Math.Max(0, hi - (int)StackWindow);
            why ??= Differential.Describe(ram, _theirs, 0, lo) ?? Differential.Describe(ram, _theirs, hi, ram.Length);
            why ??= Differential.DescribePad(_padOurs, _padTheirs);
            why ??= Registers(ours, theirs);
            why ??= Gte.Diff(_gteTheirs, _gteOurs) is { } g ? $"GTE {g}" : null;
            why ??= countOurs != countTheirs ? $"polls: recompiled left {countTheirs}, ours {countOurs}" : null;
            if (why != null)
            {
                if (_bad == 0 && Dump is { } dir) DumpStates(dir, ram);
                _bad++;
                if (_samples.Count < 8) _samples.Add($"{at}: {why}");
            }
        }

        // The recompiled result stands.
        _theirs.CopyTo(ram);
        Differential.LoadPad(mem, _padTheirs);
        c.Restore(theirs);
        Gte.Load(_gteTheirs);
        Countdown?.SetValue(null, countTheirs);

        double now = Environment.TickCount64 / 1000.0;
        if (now < _reportAt) return;
        _reportAt = now + 2.0;
        Console.WriteLine($"[KF1] verify {Name} (func_{Address:X8}): {_calls} call(s), {_bad} mismatch(es), " +
                          $"{_interrupted} interrupted");
        foreach (var s in _samples) Console.WriteLine($"[KF1]   {s}");
        _samples.Clear();
        _calls = _bad = _interrupted = 0;
    }

    /// <summary>KF1_LITERAL_DUMP=dir: the first mismatch of each routine writes the
    /// entry, recompiled and transcribed RAM images there, for diffing.</summary>
    static readonly string? Dump = Environment.GetEnvironmentVariable("KF1_LITERAL_DUMP");

    void DumpStates(string dir, Span<byte> ours)
    {
        Directory.CreateDirectory(dir);
        string stem = Path.Combine(dir, $"{Address:X8}-{DateTime.Now:HHmmss}");
        File.WriteAllBytes(stem + "-entry.bin", _before);
        File.WriteAllBytes(stem + "-recompiled.bin", _theirs);
        File.WriteAllBytes(stem + "-ours.bin", ours.ToArray());
    }

    static readonly string[] Names =
        ["at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
         "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "gp", "sp", "fp", "ra", "lo", "hi"];

    static uint[] Fields(in CpuSnapshot s) =>
        [s.At, s.V0, s.V1, s.A0, s.A1, s.A2, s.A3, s.T0, s.T1, s.T2, s.T3, s.T4, s.T5, s.T6, s.T7,
         s.S0, s.S1, s.S2, s.S3, s.S4, s.S5, s.S6, s.S7, s.T8, s.T9, s.GP, s.SP, s.FP, s.RA, s.LO, s.HI];

    static string? Registers(in CpuSnapshot ours, in CpuSnapshot theirs)
    {
        var o = Fields(ours);
        var t = Fields(theirs);
        for (int i = 0; i < o.Length; i++)
            if (o[i] != t[i]) return $"{Names[i]} recompiled {t[i]:X8} ours {o[i]:X8}";
        return null;
    }
}

/// <summary>The instructions the transcriptions spell as calls, exactly as the
/// recompiler emits them.</summary>
public static class Mips
{
    /// <summary><c>div</c> with the compiler's zero and overflow checks after it: LO
    /// and HI left alone by a zero divisor (its <c>break</c> is a no-op), and
    /// <c>at</c> left at 0x80000000 by the overflow check.</summary>
    public static void Div(CpuContext c, uint s, uint t)
    {
        if (t != 0u)
        {
            if ((int)s == int.MinValue && (int)t == -1) { c.LO = 0x80000000u; c.HI = 0u; }
            else { c.LO = (uint)((int)s / (int)t); c.HI = (uint)((int)s % (int)t); }
        }
        c.At = 0x80000000u;
    }

    /// <summary><c>divu</c>, with its zero check (a no-op <c>break</c>).</summary>
    public static void DivU(CpuContext c, uint s, uint t)
    {
        if (t != 0u) { c.LO = s / t; c.HI = s % t; }
    }

    public static void Mult(CpuContext c, uint s, uint t)
    {
        long r = (long)(int)s * (int)t;
        c.LO = (uint)r;
        c.HI = (uint)(r >> 32);
    }

    public static void MultU(CpuContext c, uint s, uint t)
    {
        ulong r = (ulong)s * t;
        c.LO = (uint)r;
        c.HI = (uint)(r >> 32);
    }

    /// <summary>The interrupt poll the recompiler puts at every loop head.</summary>
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)]
    public static void Poll(CpuContext c, PSMemory mem) => Interrupts.Poll(c, mem);
}
