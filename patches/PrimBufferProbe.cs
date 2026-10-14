using System.Reflection;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// KF1_PRIMBUF_PROBE=1: how much of the frame's primitive buffer the renderer spends,
/// and how often it runs out. A measurement only; nothing is moved. See "The renderer
/// in C#" in docs/GAME_INTERNALS.md.
///
/// FrameHead (<c>func_8001BFB8</c>) flips the buffer index and rewinds the new
/// buffer's cursor. Before that flip, the word at <c>0x80070EB8</c> still points at
/// the struct of the frame just drawn, so a pre hook there reads its final state:
/// <c>{ start, end, cursor }</c> at +0, +4, +8 (<c>func_8001E480</c> bumps the
/// cursor by 0x28 and refuses a packet when the bumped cursor is past <c>end</c>).
///
///     KF1_PRIMBUF_PROBE=1   a line every 2 s
/// </summary>
public static class PrimBufferProbe
{
    /// <summary>FrameHead: flips the buffer index and rewinds the new cursor.</summary>
    const uint FrameHead = 0x8001BFB8;

    /// <summary>u32: the frame struct of the buffer the last frame drew into.</summary>
    const uint FramePointer = 0x80070EB8;

    /// <summary>The packet the writers bump the cursor by (a POLY_FT4).</summary>
    const uint PacketBytes = 0x28;

    static bool _on;

    static readonly ModInfo _self = new()
    {
        Id = "kf1.primbufprobe",
        Name = "Primitive buffer probe",
        Version = "1.0",
        Description = "Measures the frame's primitive buffer use; moves nothing.",
    };

    // The window.
    static long _frames, _usedSum, _peak, _ranOut, _full;
    static long _capacity;
    static double _windowStart = Now;

    static double Now => Environment.TickCount64 / 1000.0;

    public static void Configure(string? on) => _on = on?.Trim() == "1";

    public static void Install()
    {
        if (!_on) return;
        HookAttach.OnOverlayLoad("primbuf probe", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var head = SymbolRegistry.Resolve("game", null, FrameHead);
        if (head == null)
        {
            Console.Error.WriteLine($"[KF1] primbuf: no game function at 0x{FrameHead:X8}");
            return false;
        }

        var pre = typeof(PrimBufferProbe).GetMethod(nameof(BeforeFrameHead), BindingFlags.Public | BindingFlags.Static)!;
        HookManager.AddPre(_self, head, pre);
        HookManager.Commit();
        if (!HookAttach.Installed(head))
        {
            Console.Error.WriteLine("[KF1] primbuf: not installed (the hook did not attach)");
            return false;
        }

        Console.WriteLine("[KF1] primbuf: probing");
        return true;
    }

    /// <summary>The frame just drawn, read before FrameHead flips and rewinds.</summary>
    public static void BeforeFrameHead(CpuContext c, IMemory m)
    {
        uint frame = m.ReadU32(FramePointer);
        if (frame < 0x80000000u) return;
        uint start = m.ReadU32(frame), end = m.ReadU32(frame + 4u), cur = m.ReadU32(frame + 8u);
        // The first frame reads a struct not yet set; a real one is in main RAM.
        if (start < 0x80000000u || end <= start || end - start > 0x200000u) return;
        if (cur < start) return;

        long used = cur - start;
        _frames++;
        _usedSum += used;
        _capacity = end - start;
        if (used > _peak) _peak = used;
        if (cur > end) _ranOut++;
        else if (cur + PacketBytes > end) _full++;

        Report();
    }

    static void Report()
    {
        double now = Now;
        if (now - _windowStart < 2.0) return;
        long mean = _frames > 0 ? _usedSum / _frames : 0;
        double pct = _capacity > 0 ? 100.0 * _peak / _capacity : 0.0;
        Console.WriteLine($"[KF1] primbuf: capacity {_capacity} bytes, peak {_peak} ({pct:0.0}%), " +
                          $"mean {mean}, {_frames} frames, {_ranOut} ran out, {_full} one packet short");
        _windowStart = now;
        _frames = _usedSum = _peak = _ranOut = _full = 0;
    }
}
