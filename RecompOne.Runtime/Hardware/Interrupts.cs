using System;
using System.Runtime.CompilerServices;
using RecompOne.Runtime.Bios;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;

namespace RecompOne.Runtime;

public static class Interrupts
{
    private static readonly HashSet<string> _reported2 = new();

    private static void ReportOnce(int irq, string kind, string what)
    {
        lock (_reported2)
            if (_reported2.Add($"{irq}:{kind}"))
                Log.Bios($"IRQ {irq} {what}");
    }

    /// <summary>
    /// Address of the game's PSY-Q interrupt-callback table, indexed by irq*4 --
    /// what libapi's InterruptCallback(irq, func) writes into.
    ///
    /// Zero means "derive it from the HookEntryInt argument", which only holds
    /// for libraries whose interrupt environment matches the offset below. The
    /// argument is really a jmp_buf, and where the callback table sits relative
    /// to it is a property of one link of one library version, so a game whose
    /// layout differs reads unrelated data here. Set this per resident overlay
    /// when the table has been identified.
    /// </summary>
    public static uint CallbackTable;

    private static bool _inHandler;
    private static bool _servicing;

    public static bool Servicing => _servicing;
    private static readonly bool[] _pending = new bool[16];

    private static bool _irqEnabled = true;

    private const uint IrqBits = 0x7FFu;
    private static uint _istat;
    private static uint _imask = IrqBits;

    public static uint ReadStat()
    {
        if (Hardware.Sio0.ConsumeAck()) Raise(7);
        return _istat;
    }

    public static uint ReadMask()
    {
        return _imask;
    }

    public static void WriteStat(uint value)
    {
        _istat &= value & IrqBits;
    }

    public static void WriteMask(uint value)
    {
        Log.Irq($"imask {_imask:X3} -> {value & IrqBits:X3}");
        _imask = value & IrqBits;
    }

    public static void Syscall(CpuContext cpu, IMemory mem)
    {
        switch (cpu.A0)
        {
            case 1:
                cpu.V0 = _irqEnabled ? 1u : 0u;
                if (_irqEnabled) Log.Irq("EnterCriticalSection: irq turned off");
                _irqEnabled = false;
                break;
            case 2:
                if (!_irqEnabled) Log.Irq("ExitCriticalSection: irq turned on");
                _irqEnabled = true;
                cpu.V0 = 0u;
                DrainPending(cpu, mem);
                break;
            default:
                cpu.V0 = 0u;
                break;
        }
    }

    private static void DrainPending(CpuContext cpu, IMemory mem)
    {
        if (_inHandler) return;
        for (var i = 0; i < _pending.Length; i++)
        {
            if (!_pending[i] || Masked(i)) continue;
            _pending[i] = false;
            Deliver(i, cpu, mem);
        }
    }

    private const int PollInterval = 2048;

    /// <summary>0047. Counts the polls that could have run a handler, so a caller can
    /// tell that nothing but its own code has touched memory since it last looked.</summary>
    public static uint SlowPolls;
    private static int _countdown = PollInterval;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void Poll(CpuContext cpu, IMemory mem)
    {
        if (--_countdown > 0) return;
        PollSlow(cpu, mem);
    }

    public static void PollNow(CpuContext cpu, IMemory mem)
    {
        _countdown = 1;
        PollSlow(cpu, mem);
    }

    public static double MsToNextVBlank
    {
        get
        {
            var left = Host.FrameClock.Due - Host.FrameClock.Now;
            return left > 0.0 ? left : 0.0;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)] //just making sure the stupid jit doenst fuck it up :D, it SHOULD be big enough now to not cause issues, but the previous one did
    private static void PollSlow(CpuContext cpu, IMemory mem)
    {
        _countdown = PollInterval;
        SlowPolls++;
        TickVBlank();
        Runtime.Timers?.Poll(RaiseTimer);
        if (_inHandler || _servicing || !_irqEnabled) return;

        var snap = cpu.Snapshot();
        TakeExceptionStack(cpu);
        try
        {
            DrainPending(cpu, mem);
            BiosB.PumpCardEvents(cpu, mem);
            Sdk.LibCd.Pump();
            Runtime.Cd?.AdvanceStreaming();
        }
        finally
        {
            cpu.Restore(snap);
        }
    }

    private const uint ExceptionStackTop = 0x0000E000u;

    private static void TakeExceptionStack(CpuContext cpu)
    {
        cpu.SP = ExceptionStackTop;
        cpu.FP = ExceptionStackTop;
    }

    public static bool Turbo;

    public static int VBlankCount => (int)Host.FrameClock.Count;

    public static double ClockMs => Host.FrameClock.Now;

    public static void ForceVBlank(CpuContext cpu, IMemory mem) //not ideal i believe
    {
        Host.FrameClock.Force();
        Raise(0);
        PollNow(cpu, mem);
    }

    private static void TickVBlank()
    {
        // The count must advance on both timelines -- VBlankCount is now this
        // clock, and BiosB's card-event pump and LibGpu's flip grace both read
        // it; freezing it stalls the memory card and so the save load. Only the
        // IRQ is gated: on the port's timeline LibEtc.AdvanceVBlanks delivers
        // IRQ 0 on its own wall-clock grid, and raising it here as well would
        // deliver every vblank twice.
        if (Host.FrameClock.Catch() > 0 && Sdk.LibEtc.BlockingVSync) Raise(0);
    }

    public static void ResyncVBlank()
    {
        Host.FrameClock.Resync();
    }

    private static void RaiseTimer(int irq)
    {
        if ((uint)irq >= _pending.Length) return;
        _istat |= 1u << irq;
        _pending[irq] = true;
    }

    public static void Raise(int irq)
    {
        if ((uint)irq >= _pending.Length) return;
        _istat |= 1u << irq;
        _pending[irq] = true;
        _countdown = 1;
    }

    public static void ClearPending()
    {
        Array.Clear(_pending);
        _istat = 0u;
        _inHandler = false;
    }

    private static bool Callable(uint addr)
    {
        if (addr == 0u || (addr & 3u) != 0u) return false;
        var ram = addr & 0x1FFFFFFFu;
        if (ram < 0x00010000u || ram >= 0x00200000u) return false;
        return Dispatcher.CanCall(addr);
    }

    private static bool Masked(int irq)
    {
        return (_imask & (1u << irq)) == 0;
    }

    public static void Deliver(int irq, CpuContext cpu, IMemory mem)
    {
        if ((uint)irq >= _pending.Length) return;

        _istat |= 1u << irq;

        if (_inHandler || !_irqEnabled || Masked(irq))
        {
            _pending[irq] = true;
            return;
        }

        _inHandler = true;
        try
        {
            Dispatch(irq, cpu, mem);

            var again = true;
            while (again)
            {
                again = false;
                for (var i = 0; i < _pending.Length; i++)
                {
                    if (!_pending[i] || Masked(i)) continue;
                    _pending[i] = false;
                    Dispatch(i, cpu, mem);
                    again = true;
                }
            }
        }
        finally
        {
            _inHandler = false;
        }
    }

    private static void Dispatch(int irq, CpuContext cpu, IMemory mem)
    {
        ServiceIrq(irq, cpu, mem);

        for (var i = 0; i < _pending.Length; i++)
        {
            if (i == irq || ((_istat & (1u << i)) == 0 && !_pending[i])) continue;
            _pending[i] = false;
            ServiceIrq(i, cpu, mem);
        }
    }

    private static void ServiceIrq(int irq, CpuContext cpu, IMemory mem)
    {
        BiosB.DeliverIrqEvents(cpu, mem, irq);

        DispatchChains(cpu, mem);

        // 0006. Upstream derives the slot from the HookEntryInt argument alone.
        // That argument is really a jmp_buf, and where the callback table sits
        // relative to it is a property of one link of one library version, so a
        // game whose layout differs reads unrelated data -- for King's Field it
        // lands in game data and eventually calls a data word. Program.cs
        // supplies the real per-overlay address; the derived path stays as the
        // fallback for a game that has not identified one.
        var intrEnv = BiosB.IntrEnvInInterruptAddr;
        uint slot;
        if (CallbackTable != 0)
        {
            slot = CallbackTable + (uint)irq * 4u;
        }
        else
        {
            if (intrEnv == 0)
            {
                ReportOnce(irq, "env", "dropped: no intr env (HookEntryInt never ran)");
                Ack(irq);
                return;
            }

            slot = intrEnv + 2u + (uint)irq * 4u;
        }

        var handler = mem.ReadU32(slot);
        Log.Irq($"irq {irq} env=0x{intrEnv:X8} table=0x{CallbackTable:X8} handler=0x{handler:X8} mask=0x{_imask:X}");
        if (handler != 0 && !Callable(handler))
        {
            // A table address that is wrong does not read as zero, it reads as
            // whatever the game keeps there, and calling that jumps into the
            // middle of nothing. Say which slot it came from, once.
            ReportOnce(irq, "bogus",
                $"dropped: handler 0x{handler:X8} at 0x{slot:X8} is not a known " +
                "function (the callback table address is wrong)");
            if (CallbackTable == 0) mem.WriteU32(slot, 0u);
            handler = 0u;
        }
        else if (handler == 0)
        {
            ReportOnce(irq, "empty", $"dropped: no handler at 0x{slot:X8}");
        }
        else
        {
            ReportOnce(irq, $"deliver:{handler:X8}", $"-> 0x{handler:X8} from 0x{slot:X8}");
        }

        if (handler == 0)
        {
            Ack(irq);
            return;
        }
        var snap = cpu.Snapshot();
        TakeExceptionStack(cpu);
        // The in-interrupt flag lives at the base of the same derived structure,
        // so it is only known to be that when the structure is what located the
        // handler. Setting it from a table address would write 1 into an address
        // nothing identified.
        var intrFlag = CallbackTable == 0 ? intrEnv : 0u;
        if (intrFlag != 0) mem.WriteU16(intrFlag, 1);
        var prev = _servicing;
        _servicing = true;
        try
        {
            Dispatcher.Call(cpu, mem, handler);
        }
        finally
        {
            _servicing = prev;
        }

        if (intrFlag != 0) mem.WriteU16(intrFlag, 0);
        cpu.Restore(snap);
        if (!_pending[irq]) Ack(irq);
    }

    private static bool DispatchChains(CpuContext cpu, IMemory mem)
    {
        var handled = false;
        var snap = cpu.Snapshot();
        TakeExceptionStack(cpu);
        var prev = _servicing;
        _servicing = true;
        try
        {
            for (var priority = 0; priority < 4; priority++)
            {
                var node = BiosB.IntChain(priority);
                var guard = 0;
                while (node != 0 && guard++ < 32)
                {
                    if ((node & 3u) != 0u || (node & 0x1FFFFFFFu) >= 0x00200000u) break;

                    var verifier = mem.ReadU32(node + 8u);
                    var handler = mem.ReadU32(node + 4u);
                    if (verifier != 0 && !Callable(verifier)) break;
                    if (handler != 0 && !Callable(handler)) break;

                    if (verifier != 0)
                    {
                        Dispatcher.Call(cpu, mem, verifier);
                        var taken = cpu.V0;
                        if (taken != 0)
                        {
                            handled = true;
                            if (handler != 0)
                            {
                                cpu.A0 = taken;
                                Dispatcher.Call(cpu, mem, handler);
                            }
                        }
                    }

                    node = mem.ReadU32(node);
                }
            }
        }
        finally
        {
            _servicing = prev;
            cpu.Restore(snap);
        }

        return handled;
    }

    private static void Ack(int irq)
    {
        _istat &= ~(1u << irq);
    }
}