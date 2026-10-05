using RecompOne.Runtime.Hle;

namespace RecompOne.Runtime;

//ToDO: HW Renderer must use a separate "view" quad text so widescren patches are possible
// or find a betrter approach, maybe allongate the vram as needed? a dufferent tex seens more ideal?
public sealed partial class Gpu
{
    public const int VramWidth = VramShadow.Width;
    public const int VramHeight = VramShadow.Height;

    public readonly VramShadow Shadow = new();
    public ushort[] Vram => Shadow.Pixels;

    private int _drawAreaLeft, _drawAreaTop, _drawAreaRight = VramWidth - 1, _drawAreaBottom = VramHeight - 1;
    private int _drawOffsetX, _drawOffsetY;
    
    public int DrawOffsetX => _drawOffsetX;

    public int DrawOffsetY => _drawOffsetY;

    public int DrawAreaLeft => _drawAreaLeft;
    public int DrawAreaTop => _drawAreaTop;
    public int DrawAreaRight => _drawAreaRight;
    public int DrawAreaBottom => _drawAreaBottom;

    // 0046. A detached GPU is a replay of a captured frame: it rasterizes in
    // software into its own VRAM and reaches nothing global -- no backend, no
    // trace, no prim event, no vertex map, no texture tracker.
    public bool Detached { get; init; }

    // Replay counters, null/zero on the live GPU. Coverage counts fragments per
    // VRAM pixel (a transparent texel is still a fragment), Owner is the
    // CoverTag of the last primitive that wrote the pixel.
    public int[]? Coverage;
    public int[]? Owner;
    public int CoverTag;
    public long Fragments;

    void Frag(int x, int y)
    {
        Fragments++;
        if (Coverage != null) Coverage[y * VramWidth + x]++;
    }

    /// <summary>0046. Take another GPU's registers and VRAM, with an empty FIFO.</summary>
    public void CopyStateFrom(Gpu src)
    {
        src.Vram.AsSpan().CopyTo(Vram);
        (_drawAreaLeft, _drawAreaTop, _drawAreaRight, _drawAreaBottom) =
            (src._drawAreaLeft, src._drawAreaTop, src._drawAreaRight, src._drawAreaBottom);
        (_drawOffsetX, _drawOffsetY) = (src._drawOffsetX, src._drawOffsetY);
        (_texPageX, _texPageY, _texDepth, _blendMode) = (src._texPageX, src._texPageY, src._texDepth, src._blendMode);
        (_dither, _texDisable) = (src._dither, src._texDisable);
        (_texWinMaskX, _texWinMaskY, _texWinOffX, _texWinOffY) =
            (src._texWinMaskX, src._texWinMaskY, src._texWinOffX, src._texWinOffY);
        (_setMask, _checkMask) = (src._setMask, src._checkMask);
        (_dispVramX, _dispVramY) = (src._dispVramX, src._dispVramY);
        (_hRange1, _hRange2, _vRange1, _vRange2) = (src._hRange1, src._hRange2, src._vRange1, src._vRange2);
        (_hres, _hres368, _vres480, _pal, _disp24, _interlace, _displayDisabled, _dmaDir) =
            (src._hres, src._hres368, src._vres480, src._pal, src._disp24, src._interlace, src._displayDisabled, src._dmaDir);
        ClearFifo();
        _polyline = _loadImage = _readImage = false;
    }

    private int _texPageX, _texPageY;
    private int _texDepth;
    private int _blendMode;
    private bool _dither;
    private bool _texDisable;

    private int _texWinMaskX, _texWinMaskY, _texWinOffX, _texWinOffY;

    private bool _setMask, _checkMask;

    private int _dispVramX, _dispVramY;
    private int _hRange1 = 0x200, _hRange2 = 0xC00, _vRange1 = 0x10, _vRange2 = 0x100;
    private int _hres;
    private bool _hres368, _vres480, _pal, _disp24, _interlace, _displayDisabled = true;
    private int _dmaDir;

    private readonly uint[] _fifo = new uint[1024];

    // 0012. The guest address each word of the command was read out of, word for
    // word alongside _fifo, or 0 for a word written straight to GP0 that never
    // lived in memory. A vertex's depth is keyed on that address, so this is what
    // reunites a packet coordinate with the GTE call that produced it. Upstream's
    // _fifoBase below is the same idea at packet granularity; this is per word,
    // which is what a DrawOTag walk needs.
    private readonly uint[] _fifoSrc = new uint[1024];
    private int _fifoCount;
    private uint _fifoBase;
    private int _need;
    private bool _polyline;

    private bool _loadImage;
    private int _loadX, _loadY, _loadW, _loadH, _loadPx;

    private bool _readImage;
    private int _readX, _readY, _readW, _readH, _readPx;
    private uint _gpuRead;

    private bool _statField;

    public int DisplayX => _dispVramX;
    public int DisplayY => _dispVramY;
    public bool DisplayEnabled => !_displayDisabled;
    public bool Display24Bit => _disp24;
    public bool Pal => _pal;

    private int CyclesPerPixel => _hres368 ? 7 : _hres switch { 0 => 10, 1 => 8, 2 => 5, _ => 4 };

    public int DisplayWidth
    {
        get
        {
            var w = ((_hRange2 - _hRange1) / CyclesPerPixel + 2) & ~3;
            return Math.Clamp(w, 0, VramWidth);
        }
    }

    public int DisplayHeight
    {
        get
        {
            var lines = _vRange2 - _vRange1;
            if (_vres480) lines <<= 1;
            return Math.Clamp(lines, 0, VramHeight);
        }
    }

    public uint ReadStat()
    {
        uint s = 0;
        s |= (uint)((_texPageX / 64) & 0xF);
        s |= (uint)(((_texPageY / 256) & 1) << 4);
        s |= (uint)((_blendMode & 3) << 5);
        s |= (uint)((_texDepth & 3) << 7);

        if (_dither) s |= 1u << 9;
        s |= 1u << 10;
        if (_setMask) s |= 1u << 11;
        if (_checkMask) s |= 1u << 12;
        s |= 1u << 13;

        if (_texDisable) s |= 1u << 15;
        if (_hres368) s |= 1u << 16;

        s |= (uint)((_hres & 3) << 17);

        if (_vres480) s |= 1u << 19;
        if (_pal) s |= 1u << 20;
        if (_disp24) s |= 1u << 21;
        if (_interlace) s |= 1u << 22;
        if (_displayDisabled) s |= 1u << 23;

        s |= 1u << 26;
        s |= 1u << 27;
        s |= 1u << 28;
        s |= (uint)((_dmaDir & 3) << 29);
        s |= _dmaDir switch { 1 => 1u << 25, 2 => 1u << 28, 3 => 1u << 27, _ => 0u };

        _statField = !_statField;
        if (_statField) s |= 1u << 31;
        return s;
    }

    public uint ReadData()
    {
        if (!_readImage) return _gpuRead;
        var lo = ReadImageHalfword();
        var hi = ReadImageHalfword();
        return (uint)(lo | (hi << 16));
    }

    private bool _polylineShaded;

    private void Push(uint word, uint srcAddr = 0u)
    {
        if (_fifoCount == 0) _fifoBase = 0u;
        if (_fifoCount >= _fifo.Length) return;
        _fifoSrc[_fifoCount] = srcAddr;
        _fifo[_fifoCount++] = word;
    }

    private void ClearFifo()
    {
        _fifoCount = 0;
        _fifoBase = 0u;
    }

    public uint FifoBase => _fifoBase;

    public void WriteGp0Packet(ReadOnlySpan<uint> words, uint baseAddress = 0u)
    {
        if (words.Length == 0) return;

        _fifoBase = 0u;

        if (_loadImage || _polyline || _fifoCount != 0 || words.Length > _fifo.Length)
        {
            for (var i = 0; i < words.Length; i++)
                WriteGp0(words[i], baseAddress == 0u ? 0u : baseAddress + (uint)i * 4u);
            return;
        }

        var need = CommandLength(words[0]);
        if (need != words.Length)
        {
            for (var i = 0; i < words.Length; i++)
                WriteGp0(words[i], baseAddress == 0u ? 0u : baseAddress + (uint)i * 4u);
            return;
        }

        words.CopyTo(_fifo);
        // 0012 needs the per-word address here too, or a packet taken by this
        // fast path arrives with no source and every vertex in it misses.
        for (var i = 0; i < words.Length; i++)
            _fifoSrc[i] = baseAddress == 0u ? 0u : baseAddress + (uint)i * 4u;
        _fifoCount = words.Length;
        _fifoBase = baseAddress;
        var trace = Detached ? null : GpuTrace.Sink;
        if (trace != null)
            for (var i = 0; i < words.Length; i++)
                trace.Word(words[i], _fifoSrc[i]);
        Execute();
        if (!_loadImage)
        {
            _fifoCount = 0;
            trace?.Executed();
        }
    }

    public void WriteGp0(uint word)
    {
        WriteGp0(word, 0u);
    }

    /// <summary><paramref name="srcAddr"/> is the guest address the word came from --
    /// DrawOTag and the DMA both read the command stream out of memory -- or 0 when
    /// it was written to the register directly and has no address.</summary>
    public void WriteGp0(uint word, uint srcAddr)
    {
        if (GpuTrace.Sink != null && !Detached) GpuTrace.Sink.Word(word, srcAddr);

        if (_loadImage)
        {
            StoreImageHalfword((ushort)word);
            StoreImageHalfword((ushort)(word >> 16));
            return;
        }

        if (_polyline)
        {
            //the terminator only sits where a vertex /colour pair start and the first pair is aways consumed untesting, psx_spx was unclear about dat, the implementation on pcsx redux is broken(somehow worse than mine), duckstation was the one that correctly implemented this, thanks duckstaion!!!!
            var data = _fifoCount - 1;
            var testable = _polylineShaded ? data >= 3 && (data & 1) == 1 : data >= 2;

            if (testable && (word & 0xF000F000u) == 0x50005000u)
            {
                _polyline = false;
                ExecutePolyline();
                ClearFifo();
                if (!Detached) GpuTrace.Sink?.Executed();
            }
            else
            {
                Push(word, srcAddr);
            }

            return;
        }

        Push(word, srcAddr);
        if (_fifoCount == 1)
        {
            _need = CommandLength(word);
            if (_need == LenPolyline)
            {
                _polyline = true;
                _polylineShaded = (word & (1u << 28)) != 0;
                return;
            }

            if (_need == LenImageLoad) _need = 3;
        }

        if (_fifoCount >= _need)
        {
            Execute();
            if (!_loadImage)
            {
                ClearFifo();
                if (!Detached) GpuTrace.Sink?.Executed();
            }
        }
    }


    public void WriteGp1(uint word)
    {
        var op = (word >> 24) & 0xFF;
        var p = word & 0xFFFFFF;
        // 0003. GP1 is display/control only -- a handful of writes per mode change
        // -- so tracing all of it is cheap and it is the only way to see whether
        // the game ever enabled the display (GP1(03)).
        if (Log.GpuOn) Log.Gpu($"GP1({op:X2}) 0x{p:X6}");
        if (!Detached) GpuTrace.Sink?.Gp1(word);
        switch (op)
        {
            case >= 0x05 and <= 0x08:
                WriteGp1Display(op, p);
                if (!Detached) GpuHle.NotifyDisplay(_dispVramX, _dispVramY, DisplayWidth, DisplayHeight);
                return;
            case 0x00: Reset(); break;
            case 0x01:
                ClearFifo();
                _polyline = false;
                _loadImage = false;
                break;
            case 0x02: break;
            case 0x03: _displayDisabled = (p & 1) != 0; break;
            case 0x04: _dmaDir = (int)(p & 3); break;
            case 0x10: SetGpuInfo(p); break;
        }
    }

    private void WriteGp1Display(uint op, uint p)
    {
        switch (op)
        {
            case 0x05:
                _dispVramX = (int)(p & 0x3FF);
                _dispVramY = (int)((p >> 10) & 0x1FF);
                break;
            case 0x06:
                _hRange1 = (int)(p & 0xFFF);
                _hRange2 = (int)((p >> 12) & 0xFFF);
                break;
            case 0x07:
                _vRange1 = (int)(p & 0x3FF);
                _vRange2 = (int)((p >> 10) & 0x3FF);
                break;
            case 0x08:
                _hres = (int)(p & 3);
                _hres368 = (p & 0x40) != 0;
                _vres480 = (p & 4) != 0;
                _pal = (p & 8) != 0;
                _disp24 = (p & 0x10) != 0;
                _interlace = (p & 0x20) != 0;
                break;
        }
    }

    private void Reset()
    {
        ClearFifo();
        _polyline = _loadImage = _readImage = false;
        _displayDisabled = true;
        _dmaDir = 0;
        _texPageX = _texPageY = _texDepth = _blendMode = 0;
        _dither = _texDisable = false;
        _texWinMaskX = _texWinMaskY = _texWinOffX = _texWinOffY = 0;
        _drawAreaLeft = _drawAreaTop = 0;
        _drawAreaRight = VramWidth - 1;
        _drawAreaBottom = VramHeight - 1;
        _drawOffsetX = _drawOffsetY = 0;
        _setMask = _checkMask = false;
        _dispVramX = _dispVramY = 0;
    }

    private void SetGpuInfo(uint p)
    {
        switch (p & 0xFF)
        {
            case 0x03: _gpuRead = (uint)(_drawAreaLeft | (_drawAreaTop << 10)); break;
            case 0x04: _gpuRead = (uint)(_drawAreaRight | (_drawAreaBottom << 10)); break;
            case 0x05: _gpuRead = (uint)((_drawOffsetX & 0x7FF) | ((_drawOffsetY & 0x7FF) << 11)); break;
            default: _gpuRead = 0; break;
        }
    }
}