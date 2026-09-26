using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// The game's per-frame primitive buffers, moved into RAM above the console's 2 MB
/// and made larger, and a probe of how close a frame comes to running out.
///
///     KF2_PRIMBUF=4           buffer size as a multiple of the game's (4 by default; 1 leaves them alone)
///     KF2_PRIMBUF_PROBE=1     peak usage, capacity and overflows, on the console
///     KF2_RAMSIZE=4           guest RAM in MB, at least what the buffers need
///     KF2_RAM_PROBE=1         also count accesses above 2 MB, per 64 KiB page
///
/// `func_8002DF80` hands out two buffers of `0x19000` bytes, one per frame, from
/// `0x800FC99C` and `0x8011599C`, and keeps each one's `{start, end, current}` in a
/// 12-byte descriptor at `0x8017E08C` / `0x8017E098`, with `0x8017E0A4` pointing at
/// whichever is this frame's. A `POLY_GT4` is `0x34` bytes, so the frame's budget is
/// **1969 quads** — and the two buffers run back to back into `0x8012E99C`, so there
/// is no slack after them to grow into.
///
/// Every assembler bumps `current` per polygon and, when it passes `end`,
/// **abandons the rest of the call**; the bump is never undone, so the rest of the
/// frame is lost too. A widened cull cone spends exactly this budget.
///
/// Everything else derives from `start` of the first descriptor: `func_80022754`
/// and `func_80035B48` shrink both buffers to `0x6400` and keep a menu's frozen
/// frame at `start + 0xC800`, and `func_80035B48` puts the full layout back from
/// `start` when it returns. So the relocation writes the full layout at
/// `0x80200000` whenever it finds the game's full layout anywhere (after
/// `func_8002DF80`, and at each frame head `func_8002E064`), and leaves a shrunk
/// one alone. See "The primitive buffer ran out" in docs/WIDESCREEN.md.
/// </summary>
public static class PrimBuffer
{
    /// <summary>Points at this frame's `{start, end, current}` descriptor.</summary>
    const uint ActiveDescriptor = 0x8017E0A4;

    const uint Descriptor0 = 0x8017E08C, Descriptor1 = 0x8017E098;

    const uint StockBase = 0x800FC99C;
    const uint StockBytes = 0x19000;

    /// <summary>The first address past the console's RAM.</summary>
    const uint Base = 0x80200000;

    /// <summary>func_8002DF80, which lays the buffers out.</summary>
    const uint Init = 0x8002DF80;

    /// <summary>func_8002E064, stage 13's head: swaps and rewinds the buffers.</summary>
    const uint FrameHead = 0x8002E064;

    /// <summary>func_800342D8, stage 13: the whole frame's allocations are behind it.</summary>
    const uint Renderer = 0x800342D8;

    /// <summary>A POLY_GT4, which is what the world is made of.</summary>
    const int PacketBytes = 0x34;

    const int MaxScale = 16;

    static readonly uint Scale = ParseScale(Environment.GetEnvironmentVariable("KF2_PRIMBUF"));

    static uint Bytes => Scale * StockBytes;

    /// <summary>What <see cref="PlanarWalk"/> needs past the two buffers: an arena of
    /// the same size for the mirrored walk's packets, its own ordering table, and a
    /// page of scratch.</summary>
    const uint OtBytes = 0x2000u * 4u, ScratchBytes = 0x8000u;

    static uint MirrorBytes => Bytes + OtBytes + ScratchBytes;

    /// <summary>Past the mirrored walk's scratch: a copy of the mesh vertices
    /// <see cref="WaterSwell"/> moves, which the assemblers read in place of the bank's.</summary>
    public const uint WaveBytes = 0x10000u;
    public static uint WaveScratch => MirrorScratch + ScratchBytes;

    /// <summary>The mirrored walk's arena, ordering table and scratch, in that order
    /// after the two buffers; valid only while <see cref="Relocated"/>.</summary>
    public static uint MirrorArena => Base + 2u * Bytes;
    public static uint MirrorArenaBytes => Bytes;
    public static uint MirrorOt => MirrorArena + Bytes;
    public static uint MirrorScratch => MirrorOt + OtBytes;

    /// <summary>Whether the buffers were moved, which is what gives the mirrored walk
    /// somewhere to put its own.</summary>
    public static bool Relocated => _relocated;

    /// <summary>Guest RAM the port asks for: enough for the buffers, or KF2_RAMSIZE.</summary>
    public static uint RamSize
    {
        get
        {
            uint need = Scale > 1 ? (Base & 0x1FFFFFFFu) + 2u * Bytes + MirrorBytes + WaveBytes : MemoryMap.RetailRamSize;
            if (uint.TryParse(Environment.GetEnvironmentVariable("KF2_RAMSIZE"), out uint mb) && mb is > 0 and <= 8)
                need = Math.Max(need, mb << 20);
            uint size = MemoryMap.RetailRamSize;
            while (size < need) size <<= 1;
            return size;
        }
    }

    static bool _measure, _relocated;

    static long _frames, _overflows, _peak, _capacity, _moves;
    static double _windowStart;
    static readonly long[] _pages = new long[RamProbe.Pages.Length];

    static double Now => Environment.TickCount64 / 1000.0;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.primbuffer",
        Name = "Primitive buffer",
        Version = "1.0",
        Description = "Moves the frame's primitive buffers above 2 MB and enlarges them.",
    };

    static uint ParseScale(string? v) =>
        uint.TryParse(v, out uint s) && s >= 1 ? Math.Min(s, (uint)MaxScale) : 4u;

    public static void Configure(string? probe)
    {
        if (!string.IsNullOrWhiteSpace(probe) && !probe.Equals("0", StringComparison.Ordinal))
            _measure = true;
        if (RamProbe.On) _measure = true;
    }

    /// <summary>The range the packet depth, lighting and texture-rect records cover: wherever the
    /// buffers are.</summary>
    public static void PublishRange()
    {
        uint lo = _relocated ? Base : StockBase;
        // The mirrored walk's arena follows the two buffers, and its packets want
        // the same records.
        uint bytes = _relocated ? 3u * Bytes : 2u * StockBytes;
        GtePacketDepth.SetRange(lo, bytes);
        GteLightMap.SetRange(lo, bytes);
        GteTexRect.SetRange(lo, bytes);
    }

    /// <summary>The second of the relocated buffers, which nothing else writes while a
    /// menu has shrunk the layout into the first; false when the buffers were not
    /// moved.</summary>
    public static bool TrySecondBuffer(out uint lo, out uint hi)
    {
        lo = Base + Bytes;
        hi = Base + 2u * Bytes;
        return _relocated;
    }

    public static void Install()
    {
        _windowStart = Now;
        PublishRange();
        if (Scale > 1) HookAttach.OnOverlayLoad("primbuf", AttachRelocation);
        if (_measure) HookAttach.OnOverlayLoad("primbuf probe", AttachProbe);
    }

    static bool AttachRelocation()
    {
        SymbolRegistry.Build();
        var init = SymbolRegistry.Resolve("game", null, Init);
        var head = SymbolRegistry.Resolve("game", null, FrameHead);
        if (init == null || head == null)
        {
            Console.Error.WriteLine("[KF2] primbuf: game functions not found; the buffers stay at 2 MB.");
            return false;
        }

        var impl = typeof(PrimBuffer).GetMethod(nameof(Relocate), BindingFlags.Public | BindingFlags.Static)!;
        if (!HookAttach.Installed(init)) HookManager.AddPost(_self, init, impl);
        if (!HookAttach.Installed(head)) HookManager.AddPre(_self, head, impl);
        HookManager.Commit();

        // Both, or the records would follow a move that only one site makes.
        if (!HookAttach.Installed(init) || !HookAttach.Installed(head)) return false;
        _relocated = true;
        PublishRange();
        Console.WriteLine($"[KF2] primbuf: 2 x 0x{Bytes:X} bytes at 0x{Base:X8} ({Scale}x), RAM {RecompOne.Runtime.Runtime.RamSize >> 20} MB");
        return true;
    }

    static bool AttachProbe()
    {
        SymbolRegistry.Build();
        var target = SymbolRegistry.Resolve("game", null, Renderer);
        if (target == null) return false;
        HookManager.AddPost(_self, target,
            typeof(PrimBuffer).GetMethod(nameof(AfterFrame), BindingFlags.Public | BindingFlags.Static)!);
        HookManager.Commit();
        if (!HookAttach.Installed(target)) return false;
        Console.WriteLine("[KF2] primbuf: probing");
        return true;
    }

    /// <summary>Writes the enlarged layout over the game's full one, wherever its
    /// start is; a shrunk layout means a menu holds its frame in the tail, and is
    /// left alone.</summary>
    public static void Relocate(CpuContext c, IMemory m)
    {
        uint s0 = m.ReadU32(Descriptor0), e0 = m.ReadU32(Descriptor0 + 4u);
        uint s1 = m.ReadU32(Descriptor1), e1 = m.ReadU32(Descriptor1 + 4u);
        if (s0 == Base && e0 == Base + Bytes && s1 == e0 && e1 == s1 + Bytes) return;
        if (e0 - s0 != StockBytes || s1 != e0 || e1 - s1 != StockBytes) return;

        m.WriteU32(Descriptor0, Base);
        m.WriteU32(Descriptor0 + 4u, Base + Bytes);
        m.WriteU32(Descriptor0 + 8u, Base);
        m.WriteU32(Descriptor1, Base + Bytes);
        m.WriteU32(Descriptor1 + 4u, Base + 2u * Bytes);
        m.WriteU32(Descriptor1 + 8u, Base + Bytes);
        _moves++;
    }

    public static void AfterFrame(CpuContext c, IMemory m)
    {
        uint desc = m.ReadU32(ActiveDescriptor);
        if (desc == 0) return;

        uint start = m.ReadU32(desc), end = m.ReadU32(desc + 4u), cur = m.ReadU32(desc + 8u);
        if (start == 0 || end <= start) return;

        _frames++;
        _capacity = end - start;
        long used = (long)cur - start;
        if (used > _peak) _peak = used;
        if (cur > end) _overflows++;

        double now = Now;
        if (now - _windowStart < 2.0) return;

        Console.WriteLine($"[primbuf] peak {_peak}/{_capacity} bytes " +
                          $"({(_capacity > 0 ? 100.0 * _peak / _capacity : 0):0.0}%, " +
                          $"{_peak / PacketBytes} of {_capacity / PacketBytes} packets), " +
                          $"{_overflows} of {_frames} frames ran out, at 0x{start:X8}, {_moves} moves");
        if (RamProbe.On) ReportRam();

        _windowStart = now;
        _frames = _overflows = _peak = 0;
    }

    /// <summary>Accesses above 2 MB since the last window, by 64 KiB page. Outside the
    /// buffers, any at all means something reads or writes through the mirror.</summary>
    static void ReportRam()
    {
        uint lo = _relocated ? (Base & 0x1FFFFFFFu) : uint.MaxValue;
        uint hi = _relocated ? lo + 2u * Bytes + MirrorBytes : 0u;
        long inside = 0, outside = 0;
        var pages = new List<string>();
        for (int i = 0; i < _pages.Length; i++)
        {
            long n = RamProbe.Pages[i] - _pages[i];
            _pages[i] = RamProbe.Pages[i];
            if (n == 0) continue;
            uint page = MemoryMap.RetailRamSize + ((uint)i << 16);
            if (page + 0x10000u > lo && page < hi) inside += n;
            else { outside += n; pages.Add($"0x{page:X6}:{n}"); }
        }
        Console.WriteLine($"[ram] above 2 MB: {inside} in the buffers, {outside} elsewhere" +
                          (pages.Count > 0 ? $" ({string.Join(' ', pages.Take(12))})" : ""));
    }
}
