using System.Text;
using RecompOne.Runtime.Assets;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hle;
using RecompOne.Runtime.Host;

namespace Kf2;

/// <summary>
/// What the display area actually held, vblank by vblank, across an area change.
///
///     KF2_BLACKPROBE=1        sample every drawn frame and dump the window around
///                             every fdat load
///     KF2_BLACKPROBE_OUT=dir  also write the display rectangle of the ~12 frames
///                             before the load and the ~250 ms after it as PNGs
///                             (default <c>scratch/blackprobe</c>; <c>0</c> skips)
///
/// A "hitch" reported as a black flash is either the game's own transition fade
/// (luminance ramping down over frames and back up), or frames arriving with
/// nothing in them (a cliff to 0 and back). Nothing else the port carries can
/// tell those apart: the profiler measures work, the pacing probe measures rate,
/// and both read healthy while the picture is black.
///
/// Three one-pixel-tall rows out of the current display rect, read straight out
/// of the backend's VRAM the way `TexProbe` does, averaged as 5-bit luminance and
/// scaled to 0..255. Diagnostic only, off by default, and it costs a readback a
/// drawn frame while it is on.
///
/// Luminance alone cannot say *why* a frame is dark, so the dump's `frame` block
/// carries two more characters per frame: the screen tint the game asked for
/// (`func_8003220C`'s request block, `-` for none) and what the picture did with
/// it (`.` new pixels, `=` the previous frame again, `2` the one before that --
/// a flip to a buffer nothing finished drawing -- `*` an older picture back). A
/// black frame with a tint is the transition fade working; a black frame with
/// none is a frame that drew nothing.
///
/// That still cannot say a frame is *wrong*, only dark or repeated. The PNG
/// dump is the same readback, of the whole display rect after the display
/// target has been written back, so a walking crossing can name the bad one.
/// Reads skip 0039's restore copies, the way <see cref="FrameCapture"/> does.
/// </summary>
public static class BlackProbe
{
    const int Samples = 1200;        // ~7 s of frames at 165 fps
    const int AfterMs = 2500;        // keep printing this long past a load
    const int BeforeMs = 1500;       // and dump this much of what came before
    const int DarkLevel = 3;         // 0..255; below this the picture reads as black
    const int DarkMs = 80;           // a dark stretch this long marks itself

    const int PicBefore = 12;        // full frames kept so a load has a past
    const int PicAfterMs = 250;      // then this long past the mark, and write
    const int PicMax = 80;           // a cap so 165 fps cannot flood the folder
    const int PicMaxW = 512, PicMaxH = 256;

    public static bool Enabled { get; private set; }

    static string? _out;

    static readonly double[] _t = new double[Samples];
    static readonly int[] _lum = new int[Samples];
    static readonly int[] _y = new int[Samples];
    static readonly int[] _pres = new int[Samples];
    static readonly int[] _vx = new int[Samples];
    static readonly int[] _vz = new int[Samples];
    static readonly int[] _yaw = new int[Samples];
    static readonly int[] _carry = new int[Samples];
    static readonly uint[] _sig = new uint[Samples];
    static readonly int[] _tint = new int[Samples];
    static readonly int[] _cells = new int[Samples];
    static readonly bool[] _built = new bool[Samples];
    static readonly int[] _cx = new int[Samples], _cz = new int[Samples], _tx = new int[Samples], _tz = new int[Samples];
    static readonly bool[] _s8 = new bool[Samples];
    static long _lastRuns;
    static int _head, _count;
    static long _lastPresents;

    static ushort[]? _row;
    static ushort[]? _pix;
    static int _pixW, _pixH;
    static double _markMs = -1.0, _runStart = -1.0;
    static string _markName = "";

    const int FlashMax = 40;         // a black run shorter than this is a flash
    static int _flashFrames, _flashY, _flashOther, _flashPres;
    static double _flashStart;

    // Ring of full display rects. Copied into _shots at a mark so the ring can
    // keep moving while the after-frames are collected.
    static readonly ushort[][] _ringPx = new ushort[PicBefore][];
    static readonly int[] _ringW = new int[PicBefore];
    static readonly int[] _ringH = new int[PicBefore];
    static readonly double[] _ringT = new double[PicBefore];
    static readonly int[] _ringLum = new int[PicBefore];
    static readonly int[] _ringY = new int[PicBefore];
    static readonly int[] _ringTint = new int[PicBefore];
    static readonly int[] _ringCarry = new int[PicBefore];
    static readonly int[] _ringPres = new int[PicBefore];
    static readonly uint[] _ringSig = new uint[PicBefore];
    static int _ringHead, _ringCount;
    static double _picUntil = -1, _picMark;
    static string _picName = "";
    static List<Shot>? _shots;

    public static void Configure(string? enabled, string? output)
    {
        if (!string.IsNullOrWhiteSpace(enabled)) Enabled = enabled != "0";
        if (output is "0" or "off") _out = null;
        else if (!string.IsNullOrWhiteSpace(output))
        {
            _out = output;
            Enabled = true;
        }
        else if (Enabled) _out = Path.Combine("scratch", "blackprobe");
    }

    public static void Install()
    {
        if (!Enabled) return;
        Console.WriteLine(_out == null
            ? "[KF2] black probe: on, sampling the display area every drawn frame"
            : $"[KF2] black probe: on, sampling every drawn frame, pictures to {_out}");

        // Per drawn frame, not per vblank: a flash one or two frames long at 165
        // fps aliases straight past a 60 Hz sampler, which is how three crossings
        // came back reading clean.
        HookAttach.OnOverlayLoad("black probe", Attach);
        CrossProbe.InstallRenderer();

        Event.AddListener<OverlayLoadedEvent>(e =>
        {
            if (!e.Name.StartsWith("fdat", StringComparison.Ordinal)) return;
            _markMs = Now;
            _markName = e.Name;
            ArmPics(e.Name);
        });
    }

    static double Now => Environment.TickCount64;

    // libgpu DrawOTag per overlay, the same three FramePacing takes the frame
    // boundary from. A post: Widescreen owns the one Replace.
    static readonly (string Overlay, uint Addr)[] DrawOTag =
    [
        .. SdkAddr.DrawOTag,
    ];

    static readonly HashSet<uint> _hooked = [];

    static readonly RecompOne.Runtime.Modding.ModInfo _self = new()
    {
        Id = "kf2.blackprobe",
        Name = "Black probe",
        Version = "1.0",
        Description = "Samples the display area every drawn frame.",
    };

    static bool Attach()
    {
        RecompOne.Runtime.Modding.SymbolRegistry.Build();
        var after = typeof(BlackProbe).GetMethod(nameof(AfterDrawOTag),
            System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static)!;

        foreach (var (overlay, addr) in DrawOTag)
        {
            if (_hooked.Contains(addr)) continue;
            var fn = RecompOne.Runtime.Modding.SymbolRegistry.Resolve(overlay, null, addr);
            if (fn == null) continue;
            RecompOne.Runtime.Modding.HookManager.AddPost(_self, fn, after);
            RecompOne.Runtime.Modding.HookManager.Commit();
            if (HookAttach.Installed(fn)) _hooked.Add(addr);
        }

        return _hooked.Count == DrawOTag.Length;
    }

    public static void AfterDrawOTag(RecompOne.Runtime.Context.CpuContext c,
                                     RecompOne.Runtime.Memory.IMemory m) => Sample();

    static void Sample()
    {
        var be = GpuHle.Backend;
        var gpu = RecompOne.Runtime.Runtime.Gpu;
        if (be is not { Ready: true } || gpu == null) return;

        int x = gpu.DisplayX, y = gpu.DisplayY;
        int w = gpu.DisplayWidth, h = gpu.DisplayHeight;
        if (w <= 0) w = 320;
        if (h <= 0) h = 240;
        if (w > PicMaxW) w = PicMaxW;
        if (h > PicMaxH) h = PicMaxH;

        int shown;
        uint sig;
        if (_out != null)
        {
            int need = w * h;
            if (_pix == null || _pix.Length < need) _pix = new ushort[PicMaxW * PicMaxH];
            if (!ReadDisplay(be, x, y, w, h, _pix)) return;
            shown = LumSig(_pix, w, h, out sig);
            _pixW = w;
            _pixH = h;
        }
        else
        {
            _row ??= new ushort[PicMaxW];
            shown = Rows(be, x, y, w, out sig);
            if (shown < 0) return;
        }

        _row ??= new ushort[PicMaxW];
        int other = Rows(be, x, y == 0 ? 240 : 0, w, out _);

        // Presents since the last sample: a black picture that is still being
        // presented is the game drawing black, and one that is not is the game
        // thread blocked with the last frame stuck on screen.
        long presents = RecompOne.Runtime.Sdk.LibEtc.VSyncCalls +
                        RecompOne.Runtime.Sdk.LibGpu.AutoPresents;
        int delta = (int)Math.Min(presents - _lastPresents, 99);
        _lastPresents = presents;

        // A masked display is black on screen whatever VRAM holds, and VRAM is
        // what the rows above read -- so it is recorded as its own state rather
        // than inferred from a luminance that stays lit right through it.
        int lum = gpu.DisplayEnabled ? shown : -1;
        Store(Now, lum, y, delta, sig);
        StoreView();
        if (_out != null) StorePic(lum, y, delta, sig);

        // Report a short flash where it happens. A run this brief never reaches
        // the dump's threshold and is exactly what "black for a fraction of a
        // second" is, so it says so on its own line, with what the OTHER display
        // buffer held at the same moment: black in both is the game clearing, and
        // black in one with the other lit is a flip to a buffer nothing had
        // finished drawing.
        if (lum >= DarkLevel && _flashFrames is > 0 and <= FlashMax)
            Console.WriteLine($"[black] flash: {_flashFrames} frame(s), " +
                              $"~{Now - _flashStart:0} ms, buffer y{_flashY}, " +
                              $"other buffer {_flashOther}, {_flashPres} present(s) during, " +
                              $"back to {lum}");
        if (lum < DarkLevel)
        {
            if (_flashFrames++ == 0) { _flashStart = Now; _flashY = y; _flashOther = other; _flashPres = 0; }
            _flashPres += delta;
        }
        else _flashFrames = 0;

        // A crossing that reuses a resident module fires no OverlayLoadedEvent,
        // and the picture is what is being asked about -- so any dark stretch
        // long enough to see marks itself too.
        if (lum < DarkLevel) _runStart = _runStart < 0 ? Now : _runStart;
        else
        {
            if (_runStart >= 0 && Now - _runStart >= DarkMs && _markMs < 0)
            {
                _markMs = _runStart;
                _markName = "dark";
                ArmPics("dark");
            }
            _runStart = -1.0;
        }

        if (_markMs >= 0 && Now - _markMs > AfterMs)
        {
            if (_picUntil >= 0) FlushPics();
            Dump();
        }
    }

    /// <summary>Mean 5-bit luminance of three rows of one display buffer, 0..255,
    /// or -1 if the read failed. `sig` fingerprints the same pixels: two frames
    /// with one signature held the same picture, which is how a repeat is told
    /// from a redraw that happens to be equally bright.</summary>
    static int Rows(IGpuBackend be, int x, int y, int w, out uint sig)
    {
        long sum = 0;
        int n = 0;
        uint h = 2166136261u;
        foreach (int dy in stackalloc[] { 60, 120, 180 })
        {
            if (!ReadDisplay(be, x, y + dy, w, 1, _row!))
            {
                sig = 0;
                return -1;
            }

            for (int i = 0; i < w; i++)
            {
                ushort p = _row![i];
                sum += (p & 0x1F) + ((p >> 5) & 0x1F) + ((p >> 10) & 0x1F);
                n += 3;
                h = (h ^ p) * 16777619u;
            }
        }
        sig = h;
        return n == 0 ? -1 : (int)(sum * 255 / (n * 31L));
    }

    static int LumSig(ushort[] px, int w, int h, out uint sig)
    {
        long sum = 0;
        int n = 0;
        uint hash = 2166136261u;
        foreach (int dy in stackalloc[] { 60, 120, 180 })
        {
            if ((uint)dy >= (uint)h) continue;
            int row = dy * w;
            for (int i = 0; i < w; i++)
            {
                ushort p = px[row + i];
                sum += (p & 0x1F) + ((p >> 5) & 0x1F) + ((p >> 10) & 0x1F);
                n += 3;
                hash = (hash ^ p) * 16777619u;
            }
        }
        sig = hash;
        return n == 0 ? -1 : (int)(sum * 255 / (n * 31L));
    }

    /// <summary>The display rectangle, without offering the pixels to 0039's
    /// scaled restore copies. A diagnostic that sampled every frame with the
    /// default <c>ReadVram</c> would evict the copy a menu is being restored
    /// from, and would itself be a candidate for the wrong frame.</summary>
    static bool ReadDisplay(IGpuBackend be, int x, int y, int w, int h, ushort[] dst)
    {
        try
        {
            void work()
            {
                if (be is GlCore gl) gl.ReadVram(x, y, w, h, dst, false);
                else be.ReadVram(x, y, w, h, dst);
            }
            if (GpuJobs.Claimed && !GpuJobs.IsOwner) GpuJobs.Run(work);
            else work();
            return true;
        }
        catch
        {
            return false;
        }
    }

    // The screen-tint request block func_8003220C leaves for stage 13 to submit:
    // mode, then r, g, b. Mode 0xFF is "no tint", written by stage 1 (TintHold).
    // A black frame with a tint is the game's own fade; a black frame without one
    // is a frame that drew nothing, and only this tells the two apart.
    const uint TintMode = 0x80192D45, TintRed = 0x80192D46;

    static int Tint()
    {
        var m = RecompOne.Runtime.Runtime.Mem;
        if (m == null) return -1;
        return m.ReadU8(TintMode) == 0xFF ? -1 : m.ReadU8(TintRed);
    }

    // The view this frame was actually drawn with. FrameSmoothing writes the
    // carried camera into the game's own globals before the render, so these are
    // the values the frame used, not the tick's.
    const uint PosX = 0x801994ECu, PosZ = 0x801994F4u, ComposedYaw = 0x80199506u;

    static long _lastFrames;

    static void StoreView()
    {
        var m = RecompOne.Runtime.Runtime.Mem;
        if (m == null) return;
        int i = (_head - 1 + Samples) % Samples;
        _vx[i] = (int)m.ReadU32(PosX);
        _vz[i] = (int)m.ReadU32(PosZ);
        _yaw[i] = m.ReadU16(ComposedYaw);

        // Did FrameSmoothing carry the view into this frame? The globals above are
        // restored by its post-hook, so they read the tick's value here whatever
        // the frame was drawn with; this is the only honest per-frame answer.
        long f = FrameSmoothing.Frames;
        _carry[i] = f != _lastFrames ? FrameSmoothing.LastVerdict : -1;
        _lastFrames = f;
    }

    static void Store(double t, int lum, int y, int presents, uint sig)
    {
        _t[_head] = t;
        _lum[_head] = lum;
        _y[_head] = y;
        _pres[_head] = presents;
        _sig[_head] = sig;
        _tint[_head] = Tint();

        // What went into this frame, beside what came out of it. `cells` is the map
        // the tile walk submitted and `built` is whether the renderer ran for this
        // present at all -- DrawOTag is called more than once a frame, so a sample
        // here is not necessarily a picture stage 13 assembled. A black frame with
        // a full map and a renderer run behind it is a different defect from a
        // black frame with neither, and nothing before this could tell them apart.
        _cells[_head] = (int)TileWalk.CellsDrawn;
        long runs = CrossProbe.RendererRuns;
        _built[_head] = runs != _lastRuns;
        _lastRuns = runs;
        _cx[_head] = CrossProbe.CamX; _cz[_head] = CrossProbe.CamZ;
        _tx[_head] = CrossProbe.TrueX; _tz[_head] = CrossProbe.TrueZ;
        _s8[_head] = CrossProbe.Stage8Ran;

        _head = (_head + 1) % Samples;
        if (_count < Samples) _count++;
    }

    static void StorePic(int lum, int y, int presents, uint sig)
    {
        if (_pix == null || _pixW <= 0 || _pixH <= 0) return;
        int n = _pixW * _pixH;
        if (_ringPx[_ringHead] == null || _ringPx[_ringHead].Length < n)
            _ringPx[_ringHead] = new ushort[PicMaxW * PicMaxH];
        _pix.AsSpan(0, n).CopyTo(_ringPx[_ringHead]);
        _ringW[_ringHead] = _pixW;
        _ringH[_ringHead] = _pixH;
        _ringT[_ringHead] = _t[(_head - 1 + Samples) % Samples];
        _ringLum[_ringHead] = lum;
        _ringY[_ringHead] = y;
        _ringTint[_ringHead] = _tint[(_head - 1 + Samples) % Samples];
        _ringCarry[_ringHead] = _carry[(_head - 1 + Samples) % Samples];
        _ringPres[_ringHead] = presents;
        _ringSig[_ringHead] = sig;
        _ringHead = (_ringHead + 1) % PicBefore;
        if (_ringCount < PicBefore) _ringCount++;

        if (_picUntil < 0 || _shots == null) return;
        _shots.Add(CopyShot(_ringHead == 0 ? PicBefore - 1 : _ringHead - 1));
        if (Now >= _picUntil || _shots.Count >= PicMax) FlushPics();
    }

    static void ArmPics(string name)
    {
        if (_out == null || _picUntil >= 0) return;
        _picMark = Now;
        _picName = name;
        var shots = new List<Shot>(_ringCount + 32);
        int start = (_ringHead - _ringCount + PicBefore) % PicBefore;
        for (int k = 0; k < _ringCount; k++)
            shots.Add(CopyShot((start + k) % PicBefore));
        _shots = shots;
        _picUntil = Now + PicAfterMs;
    }

    static Shot CopyShot(int i)
    {
        int n = _ringW[i] * _ringH[i];
        var px = new ushort[n];
        if (_ringPx[i] != null && n > 0) _ringPx[i].AsSpan(0, n).CopyTo(px);
        return new Shot
        {
            T = _ringT[i],
            Lum = _ringLum[i],
            Y = _ringY[i],
            Tint = _ringTint[i],
            Carry = _ringCarry[i],
            Pres = _ringPres[i],
            Sig = _ringSig[i],
            W = _ringW[i],
            H = _ringH[i],
            Px = px,
        };
    }

    static void FlushPics()
    {
        var shots = _shots;
        string name = _picName;
        double mark = _picMark;
        string dirBase = _out!;
        _shots = null;
        _picUntil = -1;
        if (shots == null || shots.Count == 0) return;

        new Thread(() => WritePics(dirBase, name, mark, shots))
        {
            IsBackground = true,
            Name = "kf2-blackprobe-dump",
        }.Start();
    }

    static void WritePics(string dirBase, string name, double mark, List<Shot> shots)
    {
        try
        {
            var dir = Path.Combine(dirBase, $"{name}-{DateTime.Now:yyyyMMdd-HHmmss}");
            Directory.CreateDirectory(dir);

            uint prevSig = 0, prev2Sig = 0;
            var seen = new Dictionary<uint, int>();
            int sigs = 0;
            var index = new StringBuilder();
            index.AppendLine("# n  rel_ms  lum  y  tint  drew  carry  presents  file");

            for (int i = 0; i < shots.Count; i++)
            {
                var s = shots[i];
                char asked = s.Tint < 0 ? '-' : "0123456789abcdef"[Math.Min(s.Tint >> 4, 15)];
                char drew = '.';
                if (sigs > 0 && s.Sig == prevSig) drew = '=';
                else if (sigs > 1 && s.Sig == prev2Sig) drew = '2';
                else if (seen.ContainsKey(s.Sig)) drew = '*';
                seen[s.Sig] = i;
                prev2Sig = prevSig;
                prevSig = s.Sig;
                sigs++;

                char carry = s.Carry switch
                {
                    3 => 'C', 2 => '.', 1 => 'U', 0 => 'o', _ => '-',
                };
                int rel = (int)Math.Round(s.T - mark);
                string file = $"{i:00}_{rel:+000;-000;+000}ms_{LumName(s.Lum)}_{asked}{drew}.png";
                WritePng(Path.Combine(dir, file), s);
                index.AppendLine($"{i,2}  {rel,6}  {LumName(s.Lum),4}  {s.Y,3}  {asked}     {drew}     {carry}      {s.Pres,2}         {file}");
            }

            File.WriteAllText(Path.Combine(dir, "index.txt"), index.ToString());
            Console.WriteLine($"[black] {name}: wrote {shots.Count} frame(s) to {dir}");
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[black] {name}: could not write frames: {e.Message}");
        }
    }

    static string LumName(int lum) => lum < 0 ? "off" : lum.ToString("000");

    static void WritePng(string path, Shot s)
    {
        if (s.W <= 0 || s.H <= 0) return;
        var rgba = new byte[s.W * s.H * 4];
        for (int i = 0; i < s.W * s.H; i++)
        {
            ushort p = s.Px[i];
            int o = i * 4;
            rgba[o]     = C5(p & 0x1F);
            rgba[o + 1] = C5((p >> 5) & 0x1F);
            rgba[o + 2] = C5((p >> 10) & 0x1F);
            rgba[o + 3] = 255;
        }
        PngWriter.WriteRgba(path, rgba, s.W, s.H);
    }

    static byte C5(int v) => (byte)((v << 3) | (v >> 2));

    sealed class Shot
    {
        public double T;
        public int Lum, Y, Tint, Carry, Pres, W, H;
        public uint Sig;
        public ushort[] Px = [];
    }

    /// <summary>One line per 20 frames: the offset from the load, then each
    /// sample's luminance, with `|` where the display buffer flipped.</summary>
    static void Dump()
    {
        double mark = _markMs;
        string name = _markName;
        _markMs = -1.0;

        var sb = new StringBuilder();
        int start = (_head - _count + Samples) % Samples;
        int col = 0, lastY = -1;
        int dark = 0, darkRun = 0, worstRun = 0;
        double runStart = 0, worstMs = 0;
        int presInDark = 0, stalled = 0;
        int off = 0, offRun = 0, worstOff = 0;
        int still = 0, stillRun = 0, worstStill = 0;
        double stillStart = 0, worstStillMs = 0;
        int prevX = int.MinValue, prevZ = 0, prevYaw = 0;
        var moves = new StringBuilder();
        var frames = new StringBuilder();
        var built = new StringBuilder();
        var cam = new StringBuilder();
        var camDetail = new StringBuilder();
        var step = new StringBuilder();
        int pcx = int.MinValue, pcz = 0, ptx = 0, ptz = 0, worstStep = 0;
        var steps = new List<int>();
        int unbuilt = 0;
        var seen = new Dictionary<uint, int>();
        int held = 0, flip = 0, stale = 0, blind = 0, blindRun = 0, worstBlind = 0;
        uint prevSig = 0, prev2Sig = 0;
        int sigs = 0;

        for (int k = 0; k < _count; k++)
        {
            int i = (start + k) % Samples;
            double rel = _t[i] - mark;
            if (rel < -BeforeMs || rel > AfterMs) continue;

            if (col % 20 == 0)
            {
                if (col > 0) sb.Append('\n');
                sb.Append($"[black] {name} {rel,+6:0} ms:");
            }

            if (lastY >= 0 && _y[i] != lastY) sb.Append(" |");
            lastY = _y[i];
            sb.Append(' ').Append(_lum[i] < 0 ? "off" : _lum[i].ToString().PadLeft(3));
            col++;

            if (_lum[i] < 0) { off++; offRun++; worstOff = Math.Max(worstOff, offRun); }
            else offRun = 0;

            if (_lum[i] == 0)
            {
                dark++;
                if (darkRun++ == 0) runStart = _t[i];
                worstRun = Math.Max(worstRun, darkRun);
                worstMs = Math.Max(worstMs, _t[i] - runStart);
                presInDark += _pres[i];
            }
            else darkRun = 0;

            if (_pres[i] == 0) stalled++;

            // What the game asked for, and what the picture did with it. A black
            // frame the game tinted black is its own transition fade; a black one
            // it did not is a frame that drew nothing, and that is the difference
            // between "the crossing is dark" and "the crossing shows a wrong
            // frame". Beside it, whether this frame's pixels are new, the previous
            // frame's again, or a picture from further back coming round a second
            // time -- which is what a flip to a buffer nothing finished drawing
            // looks like from here.
            if (frames.Length > 0 && (col - 1) % 20 == 0) frames.Append('\n');
            if ((col - 1) % 20 == 0) frames.Append($"[black] {name} frame {rel,+6:0} ms:");
            char asked = _tint[i] < 0 ? '-' : "0123456789abcdef"[Math.Min(_tint[i] >> 4, 15)];
            char drew = '.';
            if (sigs > 0 && _sig[i] == prevSig) { drew = '='; held++; }
            else if (sigs > 1 && _sig[i] == prev2Sig) { drew = '2'; flip++; }
            else if (seen.ContainsKey(_sig[i])) { drew = '*'; stale++; }
            seen[_sig[i]] = col;
            prev2Sig = prevSig; prevSig = _sig[i]; sigs++;
            frames.Append(' ').Append(asked).Append(drew);

            if (built.Length > 0 && (col - 1) % 20 == 0) built.Append('\n');
            if ((col - 1) % 20 == 0) built.Append($"[black] {name} built {rel,+6:0} ms:");
            built.Append(' ').Append(_built[i] ? 'R' : '-')
                 .Append(_cells[i].ToString().PadLeft(3));
            if (!_built[i]) unbuilt++;

            if (cam.Length > 0 && (col - 1) % 20 == 0) cam.Append('\n');
            if ((col - 1) % 20 == 0) cam.Append($"[black] {name} camgap {rel,+6:0} ms:");
            int gap = Math.Max(Math.Abs(_cx[i] - _tx[i]), Math.Abs(_cz[i] - _tz[i]));
            cam.Append(' ').Append(_s8[i] ? 'S' : '-').Append(gap.ToString().PadLeft(5));
            // How far the camera moved since the last present, with a placement's
            // offset taken out: a crossing that keeps the view reads like a walk.
            if (step.Length > 0 && (col - 1) % 20 == 0) step.Append('\n');
            if ((col - 1) % 20 == 0) step.Append($"[black] {name} camstep {rel,+6:0} ms:");
            if (pcx != int.MinValue)
            {
                int sx = _cx[i] - pcx, sz = _cz[i] - pcz;
                int jx = _tx[i] - ptx, jz = _tz[i] - ptz;
                if (Math.Abs(jx) > 1024 || Math.Abs(jz) > 1024) { sx -= jx; sz -= jz; }
                int st = Math.Max(Math.Abs(sx), Math.Abs(sz));
                step.Append(st.ToString().PadLeft(5));
                worstStep = Math.Max(worstStep, st);
                steps.Add(st);
            }
            else step.Append("    -");
            pcx = _cx[i]; pcz = _cz[i]; ptx = _tx[i]; ptz = _tz[i];

            if (_lum[i] == 0 && _tint[i] < 0 || gap > 1024)
                camDetail.AppendLine($"[black] {name} cam @{rel:0} ms lum {_lum[i]} built {(_built[i] ? 1 : 0)} " +
                                     $"stage8 {(_s8[i] ? 1 : 0)} cam ({_cx[i]},{_cz[i]}) true ({_tx[i]},{_tz[i]})");

            if (_lum[i] == 0 && _tint[i] < 0)
            {
                blind++;
                blindRun++;
                worstBlind = Math.Max(worstBlind, blindRun);
            }
            else blindRun = 0;

            // Did the view move at all this frame? At 165 fps with the smoothing
            // carrying, every frame moves; a run of identical frames is the world
            // stepping at the tick rate, which is the stutter.
            if (prevX != int.MinValue)
            {
                long dx = _vx[i] - prevX, dz = _vz[i] - prevZ;
                int dyaw = Math.Abs(((_yaw[i] - prevYaw + 2048) & 0xFFF) - 2048);
                bool moved = dx != 0 || dz != 0 || dyaw != 0;

                if (moves.Length > 0 && (col - 1) % 20 == 0) moves.Append('\n');
                if ((col - 1) % 20 == 0) moves.Append($"[black] {name} carry {rel,+6:0} ms:");
                moves.Append(' ').Append(_carry[i] switch
                {
                    3 => "C",     // carried: the view was interpolated into this frame
                    2 => ".",     // declined: nothing moved, so there was nothing to carry
                    1 => "U",     // declined: unprimed -- the stutter, if it is one
                    0 => "o",     // off, or the rate is at the tick rate
                    _ => "-",     // stage 8 did not run for this frame at all
                });
                _ = moved;

                if (_carry[i] is 1 or -1)
                {
                    if (stillRun++ == 0) stillStart = _t[i];
                    still++;
                    worstStill = Math.Max(worstStill, stillRun);
                    worstStillMs = Math.Max(worstStillMs, _t[i] - stillStart);
                }
                else stillRun = 0;
            }
            prevX = _vx[i]; prevZ = _vz[i]; prevYaw = _yaw[i];
        }

        Console.WriteLine(sb.ToString());
        if (frames.Length > 0) Console.WriteLine(frames.ToString());
        if (built.Length > 0)
            Console.WriteLine(built + $"\n[black] {name}: {unbuilt} of {col} present(s) " +
                              $"the renderer did not build");
        if (moves.Length > 0) Console.WriteLine(moves.ToString());
        if (cam.Length > 0) Console.WriteLine(cam.ToString());
        if (camDetail.Length > 0) Console.Write(camDetail.ToString());
        if (step.Length > 0)
        {
            steps.Sort();
            Console.WriteLine(step.ToString());
            Console.WriteLine($"[black] {name}: camera step per present, placement offsets removed: " +
                              $"largest {worstStep}, median {(steps.Count > 0 ? steps[steps.Count / 2] : 0)}");
        }
        Console.WriteLine($"[black] {name}: {blind} black frame(s) the game asked no tint for, " +
                          $"longest run {worstBlind}; {held} frame(s) repeated the one before, " +
                          $"{flip} repeated the one before that, {stale} brought an older picture back");
        Console.WriteLine($"[black] {name}: {col} sample(s), {dark} fully black, " +
                          $"longest black run {worstRun} frame(s) (~{worstMs:0} ms), " +
                          $"{presInDark} present(s) while black, " +
                          $"{stalled} frame(s) with no present at all; " +
                          $"display masked off for {off} frame(s), longest run {worstOff}; " +
                          $"view unprimed on {still} frame(s), longest run {worstStill} " +
                          $"(~{worstStillMs:0} ms)");
    }
}
