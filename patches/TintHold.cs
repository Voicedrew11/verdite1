using System.Runtime.InteropServices;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// Stage 1, <c>func_8002C944</c>, in C#, with its screen-tint and light-record resets moved onto the tick.
///
///     KF2_TINTHOLD=0        the recompiled routine: the tints strobe above the tick rate
///     KF2_TINTHOLD=verify   run both on every call and compare RAM and registers
///
/// The routine does three things every main-loop iteration: copies 80 records of
/// 0x2C bytes from <c>0x800679A0</c> into the 0x68-byte records at <c>0x801930F0</c>
/// (the first 0x14 bytes to +0x00, the other 0x18 to +0x50), clears the word at
/// <c>0x801930EC</c>, and resets the tint request block -- mode <c>0xFF</c> at
/// <c>0x80192D45</c>, and the second wash at <c>0x80192D49</c>..<c>0x80192D4F</c>
/// zeroed. The stages that ask for a tint (2 and 3: the death fade, the damage
/// flash) are gated to the tick, so above the tick rate the reset alone ran on the
/// frames between and a tint showed one frame in seven. Here the resets -- the
/// tint block and the record copy, which stage 3 blends the DARK fog into -- run
/// only when the gated stages will; the clear still runs every frame, as stage 10
/// (ungated) reads that word. See "The tints strobed between ticks" in
/// docs/PATCHES_AND_MODS.md.
/// </summary>
public static class TintHold
{
    const uint Stage1 = 0x8002C944;
    const uint MainLoopSite = 0x80013920;   // return address of func_8001369C's per-frame call

    const uint Src = 0x800679A0, Dst = 0x801930F0;
    const uint SrcStride = 0x2C, DstStride = 0x68;
    const int Records = 80;
    const uint LoadFlag = 0x801930EC;
    const uint TintMode = 0x80192D45;   // mode, r, g, b
    const uint Wash = 0x80192D49;       // u8, then three u16 at +1, +3, +5

    enum Mode { Off, On, Verify }
    static Mode _mode = Mode.On;
    static bool _queued;

    public static bool Enabled => _mode != Mode.Off;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.tinthold",
        Name = "Tint hold",
        Version = "2.0",
        Description = "func_8002C944 in C#; keeps the death fade and damage flash up between world ticks.",
    };

    public static void Configure(string? mode)
    {
        _mode = mode?.Trim().ToLowerInvariant() switch
        {
            "0" or "off" => Mode.Off,
            "verify" => Mode.Verify,
            _ => Mode.On,
        };
    }

    public static void Install() => HookAttach.OnOverlayLoad("tint hold", Attach);

    static bool Attach()
    {
        var target = SymbolRegistry.Resolve("game", null, Stage1);
        if (target == null) return false;

        if (!_queued)
        {
            var impl = typeof(TintHold).GetMethod(nameof(Replace),
                System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!;
            _queued = HookManager.AddReplace(_self, target, impl);
            if (!_queued) return false;
        }

        HookManager.Commit();
        bool ok = HookAttach.Installed(target);
        Console.WriteLine(ok
            ? $"[KF2] tint hold: {_mode.ToString().ToLowerInvariant()}, stage 1 at 0x{Stage1:X8}"
            : "[KF2] tint hold: not installed; tints strobe above the tick rate.");
        return ok;
    }

    static void Replace(Action<CpuContext, IMemory> orig, CpuContext c, IMemory m)
    {
        // PGXP follows values through the registers, which this does not model.
        if (_mode == Mode.Off || RecompOne.Runtime.Pgxp.Pgxp.CpuTracking || m is not PSMemory mem)
        {
            orig(c, m);
            return;
        }

        // Only the main loop's call is held. The other eight callers reload the
        // lights and rebuild the quarter-turn matrices from the copy straight after.
        if (_mode == Mode.Verify) Verify(orig, c, mem);
        else Run(c, mem, c.RA != MainLoopSite || FramePacing.StagesWillRun);
    }

    static void Run(CpuContext c, PSMemory m, bool reset)
    {
        // The record copy is a reset too, and belongs on the tick with the tint
        // one: stage 3 blends the darkness condition's fog into these records
        // (func_8002A318 -> func_8002CC70 -> func_8002CBD4), and above the tick
        // rate the copy alone ran on the frames between, putting the undarkened
        // records back -- the screen strobed while DARK was up. Held, the records
        // keep the last tick's blend until the next tick rebuilds them. The load
        // flag below is still cleared every frame: func_8002CBD4 sets it only when
        // the light matrices change, and stage 10 recomputes on that frame.
        uint src = Src, dst = Dst;
        if (reset)
        {
            for (int i = 0; i < Records; i++)
            {
                Interrupts.Poll(c, m);
                for (uint o = 0; o < 0x14; o += 2) m.WriteU16(dst + o, m.ReadU16(src + o));
                for (uint o = 0x14; o < SrcStride; o += 2) m.WriteU16(dst + 0x3C + o, m.ReadU16(src + o));
                src += SrcStride;
                dst += DstStride;
            }
        }
        else
        {
            src += SrcStride * Records;
            dst += DstStride * Records;
        }

        m.WriteU32(LoadFlag, 0);
        if (reset)
        {
            m.WriteU8(Wash, 0);
            m.WriteU16(Wash + 5, 0);
            m.WriteU16(Wash + 3, 0);
            m.WriteU16(Wash + 1, 0);
            m.WriteU8(TintMode, 0xFF);
        }

        // The registers the routine leaves, for a caller that reads a temporary.
        uint last = src - SrcStride;
        c.V1 = m.ReadU32(last + 0x4);
        c.A0 = m.ReadU32(last + 0x8);
        c.A1 = m.ReadU32(last + 0xC);
        c.V0 = 0xFFu;
        c.A2 = src + 0x2A;
        c.A3 = dst + 0x66;
        c.T0 = dst;
        c.T1 = src;
        c.T2 = 0xFFFFFFFFu;
        c.T3 = 0xFFFFFFFFu;
        c.At = 0x80190000u;
    }

    // ---- KF2_TINTHOLD=verify -------------------------------------------------

    static byte[] _before = [], _theirs = [];
    static long _calls, _badRam, _badReg;
    static readonly List<string> _samples = new();
    static double _reportAt;

    /// <summary>Ours is run with the reset on, which is the routine as the game
    /// wrote it, and the recompiled result stands -- so under verify the tint
    /// strobes as it did before.</summary>
    static void Verify(Action<CpuContext, IMemory> orig, CpuContext c, PSMemory mem)
    {
        var ro = mem.Ram;
        var ram = MemoryMarshal.CreateSpan(ref MemoryMarshal.GetReference(ro), ro.Length);
        if (_before.Length != ram.Length)
        {
            _before = new byte[ram.Length];
            _theirs = new byte[ram.Length];
        }

        ram.CopyTo(_before);
        var entry = c.Snapshot();

        orig(c, mem);
        ram.CopyTo(_theirs);
        var theirs = c.Snapshot();

        _before.CopyTo(ram);
        c.Restore(entry);
        Run(c, mem, reset: true);
        var ours = c.Snapshot();

        _calls++;
        int first = ram.CommonPrefixLength(_theirs);
        if (first < ram.Length)
        {
            _badRam++;
            if (_samples.Count < 8)
                _samples.Add($"first RAM difference 0x{0x80000000u + (uint)first:X8}: " +
                             $"recompiled {_theirs[first]:X2} ours {ram[first]:X2}");
        }

        if (ours.V0 != theirs.V0 || ours.V1 != theirs.V1 || ours.A0 != theirs.A0 || ours.A1 != theirs.A1 ||
            ours.A2 != theirs.A2 || ours.A3 != theirs.A3 || ours.T0 != theirs.T0 || ours.T1 != theirs.T1 ||
            ours.T2 != theirs.T2 || ours.T3 != theirs.T3 || ours.At != theirs.At || ours.SP != theirs.SP ||
            ours.RA != theirs.RA)
        {
            _badReg++;
            if (_samples.Count < 8)
                _samples.Add($"registers: v0 {theirs.V0:X}/{ours.V0:X} v1 {theirs.V1:X}/{ours.V1:X} " +
                             $"a0 {theirs.A0:X}/{ours.A0:X} a1 {theirs.A1:X}/{ours.A1:X} " +
                             $"a2 {theirs.A2:X}/{ours.A2:X} a3 {theirs.A3:X}/{ours.A3:X} " +
                             $"t0 {theirs.T0:X}/{ours.T0:X} t1 {theirs.T1:X}/{ours.T1:X}");
        }

        _theirs.CopyTo(ram);
        c.Restore(theirs);

        double now = Environment.TickCount64 / 1000.0;
        if (now < _reportAt) return;
        _reportAt = now + 2.0;
        Console.WriteLine($"[tinthold] verify func_8002C944: {_calls} call(s), {_badRam} RAM mismatch(es), " +
                          $"{_badReg} register mismatch(es)");
        foreach (var s in _samples) Console.WriteLine($"[tinthold]   {s}");
        _samples.Clear();
        _calls = _badRam = _badReg = 0;
    }
}
