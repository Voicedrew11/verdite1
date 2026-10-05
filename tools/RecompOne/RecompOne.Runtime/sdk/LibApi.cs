using RecompOne.Runtime.Bios;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;

namespace RecompOne.Runtime.Sdk;

/// <summary>
/// PSY-Q libapi pieces that a static recompilation cannot reach on its own.
///
/// DMACallback is the important one. On hardware a finished DMA raises IRQ 3 and
/// the library's own interrupt entry reads DICR and calls the per-channel
/// callback. A recompiled build has no exception path, so that entry never runs
/// and the callbacks are dead -- which silently breaks anything that does its
/// work in one, MDEC video playback above all: the callback is where the decoded
/// frame gets handed to LoadImage.
///
/// Recording the callbacks here and invoking them when the emulated transfer
/// finishes is the same shape as LibCd's ready/data callbacks.
///
/// The kernel-patch stubs below are upstream's own, added to this same file
/// after the pin. They do not overlap with DMACallback; this is a union of both.
/// </summary>
public static class LibApi
{
    public const int Channels = 7;

    private static readonly uint[] _dmaCallback = new uint[Channels];

    public static void DMACallback(CpuContext c, IMemory m)
    {
        var ch = (int)c.A0;
        if (ch < 0 || ch >= Channels)
        {
            c.V0 = 0;
            return;
        }

        c.V0 = _dmaCallback[ch];
        _dmaCallback[ch] = c.A1;
        Log.Sdk($"DMACallback({ch}, 0x{c.A1:X8})");
    }

    internal static void Reset()
    {
        Array.Clear(_dmaCallback);
    }

    /// <summary>Run the channel's callback, if one is installed.</summary>
    internal static void Complete(int channel)
    {
        if (channel < 0 || channel >= Channels) return;
        var cb = _dmaCallback[channel];
        if (cb == 0) return;

        var c = Runtime.Cpu;
        var m = Runtime.Mem;
        if (c == null || m == null) return;

        // The transfer completes inside the store that started it, so the
        // callback runs on top of whatever the game was doing; give it back the
        // registers it had, the way Interrupts.Deliver does.
        var snap = c.Snapshot();
        Dispatcher.Call(c, m, cb);
        c.Restore(snap);
    }

    public static void PatchPad(CpuContext c, IMemory m)
    {
        Log.Bios("_patch_pad");
    }

    public static void PatchCard(CpuContext c, IMemory m)
    {
        Log.Bios("_patch_card ");
    }

    public static void PatchCard2(CpuContext c, IMemory m)
    {
        Log.Bios("_patch_card2");
    }

    public static void PatchBios(CpuContext c, IMemory m)
    {
        Log.Bios("patch bios");
    }

    public static void PatchedBiosCall(CpuContext c, IMemory m)
    {
        c.V0 = 1u;
    }
}
