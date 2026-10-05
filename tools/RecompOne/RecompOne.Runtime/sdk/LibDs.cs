using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;

namespace RecompOne.Runtime.Sdk;

public static class LibDs
{
    private const byte Nop = 0x01,
        Setloc = 0x02,
        Play = 0x03,
        ReadN = 0x06,
        Stop = 0x08,
        Pause = 0x09,
        Setfilter = 0x0D,
        Setmode = 0x0E,
        GetTN = 0x13,
        GetTD = 0x14,
        SeekL = 0x15,
        SeekP = 0x16,
        ReadS = 0x1B;

    private const int NoIntr = 0;
    private const int DataReady = 1;
    private const int Complete = 2;
    private const int DataEnd = 2;
    private const int DiskError = 5;
    private const int NoResult = -1;

    private const int SysReady = 0;
    private const int SysBusy = 1;
    private const int SysNoCd = 2;

    private const int MaxCommands = 32;
    private const int MaxResults = 32;

    private const uint ParamAddr = 0x8000F810u;
    private const uint CallAddr = 0x8000F818u;
    private const uint TextAddr = 0x8000F830u;

    private sealed class Job
    {
        public int Id;
        public byte Com;
        public readonly byte[] Param = new byte[4];
        public bool HasParam;
        public uint Sync;
        public int Retries;
        public int Head;
    }

    private sealed class Slot
    {
        public int Id;
        public int Intr;
        public readonly byte[] Result = new byte[8];
    }

    private static readonly object _gate = new();
    private static readonly Queue<Job> _queue = new();
    private static readonly List<Slot> _results = [];
    private static readonly Queue<(uint Target, int Intr, byte[] Result)> _calls = new();

    private static bool _open;
    private static int _nextId;
    private static int _debug;
    private static int _shellOpen = 1;

    private static uint _cbSync;
    private static uint _cbReady;
    private static uint _cbRead;
    private static uint _cbData;

    private static uint _readySysFunc;
    private static int _readySysRetry;
    private static int _readySysMode;
    private static bool _readySysOn;

    private static uint _readBuf;
    private static int _readLeft;
    private static int _readMode = 0x80;
    private static bool _readBroken;
    private static bool _readFailed;
    private static bool _reading;

    private static bool _streaming;
    private static int _sectorLba = -1;
    private static int _sectorOff;
    private static int _readyIntr;

    private static int _daMode;
    private static uint _daTracks;
    private static int _daIndex;

    private static readonly byte[] _lastPos = new byte[4];
    private static readonly byte[] _scratch = new byte[8];

    private static readonly string[] ComNames =
    [
        "DslNop", "DslNop", "DslSetloc", "DslPlay", "DslForward", "DslBackward",
        "DslReadN", "DslStandby", "DslStop", "DslPause", "DslInit", "DslMute",
        "DslDemute", "DslSetfilter", "DslSetmode", "DslGetparam", "DslGetlocL",
        "DslGetlocP", "DslGetlocP", "DslGetTN", "DslGetTD", "DslSeekL", "DslSeekP"
    ];

    public static void DsInit(CpuContext c, IMemory m)
    {
        Reset();
        LibCd.ClearState();
        Runtime.Spu?.CdInitVolume();
        _open = true;
        _shellOpen = 1;
        Log.Sdk("DsInit");
        c.V0 = 1;
    }

    public static void DsReset(CpuContext c, IMemory m)
    {
        Reset();
        LibCd.ClearState();
        _open = true;
        Log.Sdk("DsReset");
        c.V0 = 1;
    }

    public static void DsClose(CpuContext c, IMemory m)
    {
        Reset();
        _open = false;
        Log.Sdk("DsClose");
        c.V0 = 0;
    }

    public static void DsSetDebug(CpuContext c, IMemory m)
    {
        c.V0 = (uint)_debug;
        _debug = (int)c.A0;
    }

    public static void DsCommand(CpuContext c, IMemory m)
    {
        c.V0 = (uint)Enqueue(m, (byte)c.A0, c.A1, c.A2, (int)c.A3, 1);
    }

    public static void DsControlF(CpuContext c, IMemory m)
    {
        c.V0 = (uint)Enqueue(m, (byte)c.A0, c.A1, 0, 0, 1);
    }

    public static void DsControl(CpuContext c, IMemory m)
    {
        var id = Enqueue(m, (byte)c.A0, c.A1, 0, 0, 1);
        Drain(m);
        c.V0 = (uint)(Report(id, m, c.A2) == DiskError ? 0 : 1);
    }

    public static void DsControlB(CpuContext c, IMemory m)
    {
        var id = Enqueue(m, (byte)c.A0, c.A1, 0, 0, 1);
        Drain(m);
        c.V0 = (uint)(Report(id, m, c.A2) == Complete ? 1 : 0);
    }

    public static void DsPacket(CpuContext c, IMemory m)
    {
        c.V0 = (uint)Packet(m, (byte)c.A0, c.A1, (byte)c.A2, c.A3, (int)m.ReadU32(c.SP + 0x10u));
    }

    public static void DsSync(CpuContext c, IMemory m)
    {
        Pump();
        c.V0 = (uint)Report((int)c.A0, m, c.A1);
    }

    public static void DsFlush(CpuContext c, IMemory m)
    {
        lock (_gate)
        {
            _queue.Clear();
        }

        Log.Sdk("DsFlush");
    }

    public static void DsQueueLen(CpuContext c, IMemory m)
    {
        lock (_gate)
        {
            c.V0 = (uint)_queue.Count;
        }
    }

    public static void DsSystemStatus(CpuContext c, IMemory m)
    {
        Pump();
        if (Runtime.Cd == null)
        {
            c.V0 = SysNoCd;
            return;
        }

        int queued;
        lock (_gate)
        {
            queued = _queue.Count;
        }

        c.V0 = (uint)(queued > 0 || _readLeft > 0 || _streaming || LibCd.CddaActive ? SysBusy : SysReady);
    }

    public static void DsStatus(CpuContext c, IMemory m)
    {
        Pump();
        c.V0 = LibCd.StatusByte;
    }

    public static void DsShellOpen(CpuContext c, IMemory m)
    {
        c.V0 = (uint)_shellOpen;
    }

    public static void DsLastCom(CpuContext c, IMemory m)
    {
        c.V0 = LibCd.LastComByte;
    }

    public static void DsLastPos(CpuContext c, IMemory m)
    {
        if (c.A0 != 0)
            for (var i = 0; i < 4; i++)
                m.WriteU8(c.A0 + (uint)i, _lastPos[i]);
        c.V0 = c.A0;
    }

    public static void DsIntToPos(CpuContext c, IMemory m)
    {
        LibCd.LbaToMsf((int)c.A0, out var mm, out var ss, out var ff);
        if (c.A1 != 0)
        {
            m.WriteU8(c.A1 + 0, mm);
            m.WriteU8(c.A1 + 1, ss);
            m.WriteU8(c.A1 + 2, ff);
            m.WriteU8(c.A1 + 3, 0);
        }

        c.V0 = c.A1;
    }

    public static void DsPosToInt(CpuContext c, IMemory m)
    {
        if (c.A0 == 0)
        {
            c.V0 = 0;
            return;
        }

        c.V0 = (uint)LibCd.MsfToLba(m.ReadU8(c.A0), m.ReadU8(c.A0 + 1), m.ReadU8(c.A0 + 2));
    }

    public static void DsMix(CpuContext c, IMemory m)
    {
        if (c.A0 != 0)
            Runtime.Spu?.SetCdMix(m.ReadU8(c.A0), m.ReadU8(c.A0 + 1), m.ReadU8(c.A0 + 2), m.ReadU8(c.A0 + 3));
        c.V0 = 1;
    }

    public static void DsSyncCallback(CpuContext c, IMemory m)
    {
        c.V0 = _cbSync;
        _cbSync = c.A0;
    }

    public static void DsReadyCallback(CpuContext c, IMemory m)
    {
        c.V0 = _cbReady;
        _cbReady = c.A0;
    }

    public static void DsReadCallback(CpuContext c, IMemory m)
    {
        c.V0 = _cbRead;
        _cbRead = c.A0;
    }

    public static void DsDataCallback(CpuContext c, IMemory m)
    {
        c.V0 = _cbData;
        _cbData = c.A0;
    }

    public static void DsStartReadySystem(CpuContext c, IMemory m)
    {
        if (_readySysOn)
        {
            c.V0 = 0;
            return;
        }

        _readySysOn = true;
        _readySysFunc = c.A0;
        _readySysRetry = (int)c.A1;
        Log.Sdk($"DsStartReadySystem func=0x{c.A0:X8} retry={(int)c.A1}");
        c.V0 = 1;
    }

    public static void DsEndReadySystem(CpuContext c, IMemory m)
    {
        _readySysOn = false;
        _readySysFunc = 0;
        c.V0 = 0;
    }

    public static void DsReadySystemMode(CpuContext c, IMemory m)
    {
        c.V0 = (uint)_readySysMode;
        _readySysMode = (int)c.A0;
    }

    public static void DsReadMode(CpuContext c, IMemory m)
    {
        c.V0 = (uint)_readMode;
        _readMode = (int)c.A0;
    }

    public static void DsRead(CpuContext c, IMemory m)
    {
        var pos = c.A0;
        var sectors = (int)c.A1;
        var buf = c.A2;
        var mode = (int)c.A3;

        _readBuf = buf;
        _readLeft = sectors;
        _readMode = mode;
        _readBroken = false;
        _readFailed = false;
        _reading = true;

        var id = Packet(m, (byte)mode, pos, ReadN, 0, 0);
        Log.Sdk($"DsRead sectors={sectors} buf=0x{buf:X8} mode=0x{mode:X2} id={id}");
        c.V0 = (uint)id;
    }

    public static void DsRead2(CpuContext c, IMemory m)
    {
        c.V0 = (uint)Packet(m, (byte)c.A1, c.A0, ReadS, 0, 0);
    }

    public static void DsReadBreak(CpuContext c, IMemory m)
    {
        _readBroken = true;
        _readLeft = 0;
        _reading = false;
        _streaming = false;
        Halt(m);
    }

    public static void DsReadSync(CpuContext c, IMemory m)
    {
        Pump();
        if (c.A0 != 0)
        {
            LibCd.GrabResult(_scratch);
            LibCd.PutResult(m, c.A0, _scratch);
        }

        if (_readFailed || _readBroken)
        {
            c.V0 = 0xFFFFFFFFu;
            return;
        }

        c.V0 = (uint)_readLeft;
    }

    public static void DsReady(CpuContext c, IMemory m)
    {
        Pump();
        var intr = _readyIntr;
        _readyIntr = NoIntr;

        if (c.A0 != 0)
        {
            if (intr == DataReady && _sectorLba >= 0) Header(m, _sectorLba, c.A0);
            else
            {
                LibCd.GrabResult(_scratch);
                LibCd.PutResult(m, c.A0, _scratch);
            }
        }

        c.V0 = (uint)intr;
    }

    public static void DsGetSector(CpuContext c, IMemory m)
    {
        Copy(m, c.A0, (int)c.A1 * 4);
        c.V0 = 1;
    }

    public static void DsGetSector2(CpuContext c, IMemory m)
    {
        Copy(m, c.A0, (int)c.A1 * 4);
        if (_cbData != 0) Post(_cbData, Complete);
        c.V0 = 1;
    }

    public static void DsDataSync(CpuContext c, IMemory m)
    {
        c.V0 = 0;
    }

    public static void DsSearchFile(CpuContext c, IMemory m)
    {
        LibCd.CdSearchFile(c, m);
    }

    public static void DsReadFile(CpuContext c, IMemory m)
    {
        var name = c.A0;
        var addr = c.A1;
        var bytes = (int)c.A2;

        if (name != 0)
        {
            c.A0 = ParamAddr;
            c.A1 = name;
            LibCd.CdSearchFile(c, m);
            if (c.V0 == 0)
            {
                c.V0 = 0;
                return;
            }

            var size = (int)m.ReadU32(ParamAddr + 4);
            if (bytes == 0 || bytes > size) bytes = size;
            LibCd.SeekTo(LibCd.MsfToLba(m.ReadU8(ParamAddr), m.ReadU8(ParamAddr + 1), m.ReadU8(ParamAddr + 2)));
        }

        if (bytes <= 0)
        {
            c.V0 = 0;
            return;
        }

        var sectors = (bytes + 2047) / 2048;
        var done = Fetch(m, addr, sectors);
        c.V0 = (uint)(done * 2048 < bytes ? done * 2048 : bytes);
    }

    public static void DsGetToc(CpuContext c, IMemory m)
    {
        var fs = Runtime.Cd?.Fs;
        if (fs == null || c.A0 == 0)
        {
            c.V0 = 0;
            return;
        }

        var count = 0;
        for (var t = fs.FirstTrack; t <= fs.LastTrack && count < 100; t++)
        {
            if (!fs.TrackStartLba(t, out var lba)) continue;
            LibCd.LbaToMsf(lba, out var mm, out var ss, out var ff);
            var slot = c.A0 + (uint)(count * 4);
            m.WriteU8(slot + 0, mm);
            m.WriteU8(slot + 1, ss);
            m.WriteU8(slot + 2, ff);
            m.WriteU8(slot + 3, (byte)t);
            count++;
        }

        c.V0 = (uint)count;
    }

    public static void DsGetDiskType(CpuContext c, IMemory m)
    {
        c.V0 = (uint)(Runtime.Cd == null ? 2 : 0);
    }

    public static void DsPlay(CpuContext c, IMemory m)
    {
        _daMode = (int)c.A0;
        _daTracks = c.A1;
        _daIndex = (int)c.A2;

        if (_daMode == 3)
        {
            c.V0 = (uint)_daIndex;
            return;
        }

        if (_daMode == 0 || _daTracks == 0)
        {
            _daTracks = 0;
            Enqueue(m, Stop, 0, 0, 0, 1);
            c.V0 = 0xFFFFFFFFu;
            return;
        }

        c.V0 = (uint)(Track(m, _daIndex) ? _daIndex : -1);
    }

    public static void DsComstr(CpuContext c, IMemory m)
    {
        var com = (int)(c.A0 & 0x1F);
        c.V0 = Text(m, com < ComNames.Length ? ComNames[com] : "DslNop");
    }

    public static void DsIntstr(CpuContext c, IMemory m)
    {
        c.V0 = Text(m, (int)c.A0 switch
        {
            DataReady => "DataReady",
            Complete => "Complete",
            DiskError => "Disk Error",
            _ => "NoIntr"
        });
    }

    internal static void Reset()
    {
        lock (_gate)
        {
            _queue.Clear();
            _results.Clear();
            _calls.Clear();
        }

        _nextId = 0;
        _cbSync = _cbReady = _cbRead = _cbData = 0;
        _readySysOn = false;
        _readySysFunc = 0;
        _readySysRetry = 0;
        _readySysMode = 0;
        _readBuf = 0;
        _readLeft = 0;
        _readBroken = false;
        _readFailed = false;
        _reading = false;
        _streaming = false;
        _sectorLba = -1;
        _sectorOff = 0;
        _readyIntr = NoIntr;
        _daTracks = 0;
        _daIndex = 0;
        Array.Clear(_lastPos);
    }

    internal static void Detach()
    {
        Reset();
        _open = false;
    }

    internal static bool Active => _open;

    internal static int SectorLba => _sectorLba;

    internal static void Tick()
    {
        if (!_open) return;
        Pump();
    }

    internal static void CddaEnd()
    {
        _readyIntr = DataEnd;
        Ready(DataEnd);
    }

    private static int Enqueue(IMemory m, byte com, uint param, uint sync, int retries, int head)
    {
        var job = new Job { Com = com, Sync = sync, Retries = retries, Head = head };
        if (param != 0)
        {
            job.HasParam = true;
            for (var i = 0; i < 4; i++) job.Param[i] = m.ReadU8(param + (uint)i);
        }

        lock (_gate)
        {
            if (_queue.Count >= MaxCommands) return 0;
            job.Id = NextId();
            _queue.Enqueue(job);
        }

        return job.Id;
    }

    private static int Packet(IMemory m, byte mode, uint pos, byte com, uint sync, int retries)
    {
        lock (_gate)
        {
            if (_queue.Count + 4 > MaxCommands) return 0;
        }

        Enqueue(m, Pause, 0, 0, 0, 0);

        m.WriteU8(ParamAddr, mode);
        Enqueue(m, Setmode, ParamAddr, 0, 0, 0);

        if (pos != 0)
        {
            for (var i = 0; i < 4; i++) _lastPos[i] = m.ReadU8(pos + (uint)i);
            Enqueue(m, Setloc, pos, 0, 0, 0);
        }

        return Enqueue(m, com, 0, sync, retries, 1);
    }

    private static int NextId()
    {
        if (++_nextId <= 0) _nextId = 1;
        return _nextId;
    }

    private static bool _pumping;

    internal static void Pump()
    {
        if (_pumping) return;
        var m = Runtime.Mem;
        if (m == null) return;

        _pumping = true;
        try
        {
            Drain(m);
            Feed(m);
            Deliver();
        }
        finally
        {
            _pumping = false;
        }
    }

    private static void Drain(IMemory m)
    {
        while (true)
        {
            Job job;
            lock (_gate)
            {
                if (_queue.Count == 0) return;
                job = _queue.Peek();
            }

            var param = 0u;
            if (job.HasParam)
            {
                for (var i = 0; i < 4; i++) m.WriteU8(ParamAddr + (uint)i, job.Param[i]);
                param = ParamAddr;
            }

            LibCd.Primitive(m, job.Com, param, 0);
            var intr = LibCd.LastIntrCode == 0x05 ? DiskError : Complete;

            if (intr == DiskError && job.Retries != 0)
            {
                if (job.Retries > 0) job.Retries--;
                Log.Sdk($"Ds retry cmd 0x{job.Com:X2} left={job.Retries}");
                return;
            }

            lock (_gate)
            {
                _queue.Dequeue();
            }

            Note(job.Id, intr);

            if (job.Com is ReadN or ReadS)
            {
                _streaming = true;
                _sectorLba = -1;
                if (intr == DiskError)
                {
                    _streaming = false;
                    if (_readySysOn && _readySysRetry != 0)
                    {
                        if (_readySysRetry > 0) _readySysRetry--;
                        Again(m, job.Com);
                    }
                    else
                    {
                        _readFailed = true;
                        _readLeft = 0;
                        _reading = false;
                    }
                }
            }
            else if (job.Com is Pause or Stop)
            {
                _streaming = false;
            }
            else if (job.Com == Setloc)
            {
                for (var i = 0; i < 4; i++) _lastPos[i] = job.Param[i];
            }

            if (job.Head == 0) continue;

            var target = job.Sync != 0 ? job.Sync : _cbSync;
            if (target != 0) Post(target, intr);
        }
    }

    private static void Feed(IMemory m)
    {
        if (!_streaming || !Listening) return;

        var cd = Runtime.Cd;
        if (cd == null) return;

        var lba = LibCd.CurrentLba;
        if (lba < 0 || lba >= cd.Fs.DataSectors)
        {
            _streaming = false;
            _readFailed = _readLeft > 0;
            _readLeft = 0;
            _reading = false;
            _readyIntr = DataEnd;
            Ready(DataEnd);
            return;
        }

        Dispatcher.LoadByLba(lba);
        _sectorLba = lba;
        _sectorOff = Head;
        _readyIntr = DataReady;

        if (_reading && _readLeft > 0 && _readBuf != 0)
        {
            Copy(m, _readBuf, UserBytes);
            _readBuf += UserBytes;
            _readLeft--;
        }

        LibCd.Step(1);
        Dispatcher.LoadByLba(LibCd.CurrentLba);

        Ready(DataReady);

        if (_readLeft > 0 || !_reading) return;

        _reading = false;
        _streaming = false;
        Halt(m);
        if (_cbRead != 0) Post(_cbRead, Complete);
    }

    private const int UserBytes = 2048;

    private static int Head => LibCd.SectorBytes >= 2340 ? 12 : 0;

    private static bool Listening =>
        (_reading && _readLeft > 0 && _readBuf != 0) || (_readySysOn && _readySysFunc != 0) || _cbReady != 0;

    private static readonly byte[] _headerBytes = new byte[8];

    private static void Ready(int intr)
    {
        var target = _readySysOn && _readySysFunc != 0 ? _readySysFunc : _cbReady;
        if (target == 0) return;

        if (intr != DataReady || _sectorLba < 0)
        {
            Post(target, intr);
            return;
        }

        LibCd.GrabHeader(_sectorLba, _headerBytes);
        Post(target, intr, _headerBytes);
    }

    private static void Halt(IMemory m)
    {
        LibCd.Primitive(m, Pause, 0, 0);
    }

    private static int Fetch(IMemory m, uint addr, int sectors)
    {
        var cd = Runtime.Cd;
        if (cd == null) return 0;

        var done = 0;
        for (var i = 0; i < sectors; i++)
        {
            var lba = LibCd.CurrentLba;
            if (lba < 0 || lba >= cd.Fs.DataSectors) break;
            Dispatcher.LoadByLba(lba);
            _sectorLba = lba;
            _sectorOff = Head;
            Copy(m, addr, UserBytes);
            addr += UserBytes;
            LibCd.Step(1);
            done++;
        }

        return done;
    }

    private static void Copy(IMemory m, uint addr, int bytes)
    {
        if (addr == 0 || bytes <= 0) return;
        var cd = Runtime.Cd;
        if (cd == null) return;

        var lba = _sectorLba >= 0 ? _sectorLba : LibCd.CurrentLba;
        if (lba < 0) return;

        byte[] data;
        lock (LibCd.DiscLock)
        {
            data = cd.ReadSectorData(lba, LibCd.SectorBytes);
        }

        if (_sectorOff >= data.Length) return;

        var n = Math.Min(data.Length - _sectorOff, bytes);
        for (var i = 0; i < n; i++) m.WriteU8(addr + (uint)i, data[_sectorOff + i]);
        _sectorOff += n;
    }

    private static void Header(IMemory m, int lba, uint addr)
    {
        LibCd.GrabHeader(lba, _headerBytes);
        LibCd.PutResult(m, addr, _headerBytes);
    }

    private static void Note(int id, int intr)
    {
        var slot = new Slot { Id = id, Intr = intr };
        LibCd.GrabResult(slot.Result);
        lock (_gate)
        {
            _results.Add(slot);
            if (_results.Count > MaxResults) _results.RemoveAt(0);
        }
    }

    private static int Report(int id, IMemory m, uint result)
    {
        Slot? slot = null;
        lock (_gate)
        {
            if (id == 0) slot = _results.Count > 0 ? _results[^1] : null;
            else
                for (var i = _results.Count - 1; i >= 0; i--)
                    if (_results[i].Id == id)
                    {
                        slot = _results[i];
                        break;
                    }

            if (slot == null)
            {
                if (id == 0) return NoIntr;
                foreach (var job in _queue)
                    if (job.Id == id)
                        return NoIntr;
                return id > _nextId ? NoIntr : NoResult;
            }
        }

        if (result != 0) LibCd.PutResult(m, result, slot.Result);
        return slot.Intr;
    }

    private static void Post(uint target, int intr)
    {
        if (target == 0) return;
        var snapshot = new byte[8];
        LibCd.GrabResult(snapshot);
        Post(target, intr, snapshot);
    }

    private static void Post(uint target, int intr, byte[] result)
    {
        if (target == 0) return;
        var snapshot = new byte[8];
        Array.Copy(result, snapshot, 8);
        lock (_gate)
        {
            _calls.Enqueue((target, intr, snapshot));
        }
    }

    private static bool _inCallback;

    private static void Deliver()
    {
        if (_inCallback) return;
        var c = Runtime.Cpu;
        var m = Runtime.Mem;
        if (c == null || m == null) return;

        _inCallback = true;
        var snap = c.Snapshot();
        try
        {
            while (true)
            {
                uint target;
                int intr;
                byte[] result;
                lock (_gate)
                {
                    if (_calls.Count == 0) break;
                    (target, intr, result) = _calls.Dequeue();
                }

                LibCd.PutResult(m, CallAddr, result);
                c.A0 = (uint)intr;
                c.A1 = CallAddr;
                Dispatcher.Call(c, m, target);
            }
        }
        finally
        {
            c.Restore(snap);
            _inCallback = false;
        }
    }

    private static void Again(IMemory m, byte com)
    {
        m.WriteU8(ParamAddr, (byte)_readMode);
        Enqueue(m, Setmode, ParamAddr, 0, 0, 0);
        for (var i = 0; i < 4; i++) m.WriteU8(ParamAddr + 4 + (uint)i, _lastPos[i]);
        Enqueue(m, Setloc, ParamAddr + 4, 0, 0, 0);
        Enqueue(m, com, 0, 0, 0, 1);
    }

    private static bool Track(IMemory m, int index)
    {
        var fs = Runtime.Cd?.Fs;
        if (fs == null || _daTracks == 0) return false;
        
        var track = (int)m.ReadU32(_daTracks + (uint)(index * 4));
        if (track == 0 || !fs.TrackStartLba(track, out var lba)) return false;
        
        LibCd.LbaToMsf(lba, out var mm, out var ss, out var ff);
        m.WriteU8(ParamAddr + 0, mm);
        m.WriteU8(ParamAddr + 1, ss);
        m.WriteU8(ParamAddr + 2, ff);
        m.WriteU8(ParamAddr + 3, 0);
        
        m.WriteU8(ParamAddr + 4, 0x02);
        Enqueue(m, Setmode, ParamAddr + 4, 0, 0, 0);
        Enqueue(m, Setloc, ParamAddr, 0, 0, 0);
        Enqueue(m, Play, 0, 0, 0, 1);
        return true;
    }

    private static uint Text(IMemory m, string s)
    {
        var i = 0u;
        foreach (var ch in s) m.WriteU8(TextAddr + i++, (byte)ch);
        m.WriteU8(TextAddr + i, 0);
        return TextAddr;
    }
}
