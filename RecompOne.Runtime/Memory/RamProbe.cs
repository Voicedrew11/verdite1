using System.Runtime.CompilerServices;

namespace RecompOne.Runtime.Memory;

/// <summary>
/// Counts guest RAM accesses above the retail 2 MB, per 64 KiB page. With 2 MB of
/// RAM those addresses mirror the low 2 MB; with more they are separate memory, so
/// a game that reads through the mirror changes behaviour when the RAM grows.
/// <c>KF2_RAM_PROBE=1</c> arms it at boot; otherwise the JIT folds the test away.
/// </summary>
public static class RamProbe
{
    public static readonly bool On = Environment.GetEnvironmentVariable("KF2_RAM_PROBE") is { } v && v.Trim() != "0";

    const int PageShift = 16;

    /// <summary>Accesses in [2 MB, 8 MB), indexed by (phys - 2 MB) >> 16.</summary>
    public static readonly long[] Pages = new long[(MemoryMap.RamWindow - MemoryMap.RetailRamSize) >> PageShift];

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void Note(uint phys)
    {
        uint o = phys - MemoryMap.RetailRamSize;
        if (o < MemoryMap.RamWindow - MemoryMap.RetailRamSize) Pages[o >> PageShift]++;
    }
}
