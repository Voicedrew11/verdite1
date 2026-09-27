using System.Runtime.CompilerServices;

namespace RecompOne.Runtime.Memory;

/// <summary>
/// 0084. A write watchpoint for the port's diagnostics: <c>KF2_RAM_WATCH=80095230,800597D4</c>
/// names words, and the first store to each from every distinct recompiled
/// routine prints that routine's managed call chain once. Recompiled functions
/// carry their MIPS address in their name, so the chain names the writer and how
/// it was reached -- what a static search cannot do for a word written through a
/// pointer. Off, the JIT folds the test away, as it does for <see cref="RamProbe"/>.
/// </summary>
public static class RamWatch
{
    public static readonly bool On = Environment.GetEnvironmentVariable("KF2_RAM_WATCH") is { Length: > 0 };

    static readonly HashSet<uint> Words = Parse();
    static readonly HashSet<string> Seen = new();
    static uint _lo = uint.MaxValue, _hi;

    static HashSet<uint> Parse()
    {
        var set = new HashSet<uint>();
        var v = Environment.GetEnvironmentVariable("KF2_RAM_WATCH");
        if (string.IsNullOrWhiteSpace(v)) return set;
        foreach (var part in v.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            if (!uint.TryParse(part.Replace("0x", "", StringComparison.OrdinalIgnoreCase),
                    System.Globalization.NumberStyles.HexNumber, null, out var a)) continue;
            uint phys = MemoryMap.ToPhysical(a) & ~3u;
            set.Add(phys);
            _lo = Math.Min(_lo, phys);
            _hi = Math.Max(_hi, phys + 3);
        }
        return set;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void Note(uint phys)
    {
        if (phys < _lo || phys > _hi) return;
        Hit(phys);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Hit(uint phys)
    {
        if (!Words.Contains(phys & ~3u)) return;
        var trace = new System.Diagnostics.StackTrace(2, false);
        var chain = new List<string>();
        foreach (var frame in trace.GetFrames())
        {
            var m = frame.GetMethod();
            if (m?.DeclaringType?.Namespace != "Recompiled") continue;
            chain.Add(m.Name);
            if (chain.Count == 8) break;
        }
        var key = $"{phys:X8}:{(chain.Count > 0 ? chain[0] : "?")}";
        lock (Seen)
        {
            if (!Seen.Add(key)) return;
        }
        Console.WriteLine($"[watch] 0x{phys | 0x80000000u:X8} written by {string.Join(" <- ", chain)}");
    }
}
