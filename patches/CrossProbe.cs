using System.Collections.Concurrent;
using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Assets;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1;

/// <summary>
/// KF1_CROSSPROBE=1: every presented picture across an area change, read back
/// (<see cref="PresentSnap"/>) and reduced to numbers, so a dark or stalled frame at a
/// crossing is named by a counter rather than by eye. Each present's luminance (the
/// world's rows, a sampled grid), its time since the last present, whether the
/// retained world drew it, whether the retained map was ready, the tick and the
/// player's position; the 400 ms before the area transition func_80036850 and the 3 s after it
/// are printed when the window closes. The transition is the load: it is a pre hook,
/// and its A0 is the area it goes to.
///
///     KF1_CROSSPROBE=1          the table, on each area change
///     KF1_CROSSPROBE_OUT=dir    also the pictures as PNGs: every present after the
///                               load, the last 12 before it (default
///                               scratch/crossprobe when set to 1; unset is off).
///                               Encoding runs on a writer thread, so the present
///                               is not held up by it.
///
/// Reading back every present stalls the GPU, so frame times are the probe's as much
/// as the game's; lower the render scale (shell <c>scale 1</c>) to keep it small.
/// Verdite3's CrossProbe, with the trigger and counters of this game.
/// </summary>
public static class CrossProbe
{
    record struct Row(double T, double Dt, int Lum, int Centre, long GpuFrames, long Retained, bool Ready, bool Main,
        long Tick, bool Ticked, int CamX, int CamZ, string Overlay, bool Tinted, long Submitted, char Same, int DispY,
        long Draws, long Tris, int Prims);

    // func_80036850(A0 area, A1 entry): the area transition the exits and warp call.
    const uint AreaTransition = 0x80036850;
    const double BeforeMs = 400, AfterMs = 3000;
    const int PicBefore = 12;
    // The player's position (s32 x, z): the camera block keeps the old area's after a warp.
    const uint PosX = 0x800A0824, PosZ = 0x800A082C;

    static readonly ModInfo _self = new() { Id = "kf1.crossprobe", Name = "Cross probe", Version = "1.0" };
    static readonly object _gate = new();
    static bool _on;
    static string? _out;
    static BlockingCollection<(string Path, byte[] Rgba, int W, int H)>? _writes;
    static long _seq, _wrote;
    static readonly Queue<Row> _before = new();
    static readonly Queue<(double T, int Lum, byte[] Rgba, int W, int H)> _pics = new();
    static readonly List<Row> _after = new();
    static readonly System.Diagnostics.Stopwatch _clock = System.Diagnostics.Stopwatch.StartNew();
    static double _last = -1, _loadAt = -1;
    static string _loaded = "";
    static long _gpuFrames, _retained, _submitted;
    static ulong _print1, _print2;
    static long _draws, _tris; static int _prims;
    static bool _tinted;

    public static void Configure(string? on, string? output)
    {
        if (string.IsNullOrWhiteSpace(on) || on == "0") return;
        _on = true;
        if (string.IsNullOrWhiteSpace(output) || output == "0") return;
        _out = output == "1" ? Path.Combine("scratch", "crossprobe") : output;
        Directory.CreateDirectory(_out);
        _writes = new();
        var writer = new Thread(() =>
        {
            foreach (var j in _writes.GetConsumingEnumerable())
            {
                try { PngWriter.WriteRgba(j.Path, j.Rgba, j.W, j.H); }
                catch (Exception e) { Console.Error.WriteLine($"[KF1] crossprobe: {j.Path}: {e.Message}"); }
            }
        }) { IsBackground = true, Name = "crossprobe pictures" };
        writer.Start();
    }

    public static void Install()
    {
        if (!_on) return;
        HookAttach.OnOverlayLoad("cross probe", Attach);
        Event.AddListener<RenderPrimEvent>(e =>
        {
            _prims++;
            if (_tinted || !e.SemiTransparent || e.Gouraud) return;
            int lo = int.MaxValue, hi = int.MinValue;
            for (int i = 0; i < e.Count; i++) { lo = Math.Min(lo, e.X[i]); hi = Math.Max(hi, e.X[i]); }
            if (lo <= e.DrawLeft + 2 && hi >= e.DrawRight - 1) _tinted = true;
        });
        PresentSnap.Request(Taken);
        Console.WriteLine(_out == null
            ? "[KF1] crossprobe: on"
            : $"[KF1] crossprobe: on, pictures to {Path.GetFullPath(_out)}");
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        if (SymbolRegistry.Resolve("game", null, AreaTransition) is not { } t) return false;
        HookManager.AddPre(_self, t, typeof(CrossProbe).GetMethod(nameof(BeforeTransition), BindingFlags.Public | BindingFlags.Static)!);
        HookManager.Commit();
        return HookAttach.Installed(t);
    }

    /// <summary>The area transition is the load: the window opens here, and A0 is the area.</summary>
    public static void BeforeTransition(CpuContext c, IMemory m)
    {
        lock (_gate)
        {
            if (_loadAt >= 0) Flush();
            _loadAt = _clock.Elapsed.TotalMilliseconds; _loaded = $"area {c.A0}";
            _wrote = 0;
            if (_out != null)
            {
                foreach (var p in _pics) Write(p.T - _loadAt, p.Lum, p.Rgba, p.W, p.H);
                _pics.Clear();
            }
            Console.WriteLine($"[KF1] crossprobe: area transition to {c.A0} at {_loadAt:0} ms, tick {FramePacing.Ticks}");
            Console.Out.Flush();
        }
    }

    static void Taken(byte[] rgba, int w, int h, int dispX, int dispY)
    {
        lock (_gate)
        {
            double now = _clock.Elapsed.TotalMilliseconds;
            // A grid of samples over the middle two thirds of the rows (the gauges and
            // the compass sit at the edges), and the centre ninth on its own.
            long sum = 0, centre = 0; int n = 0, nc = 0; ulong print = 14695981039346656037;
            for (int gy = 0; gy < 24; gy++)
                for (int gx = 0; gx < 32; gx++)
                {
                    int y = h / 6 + gy * (h * 2 / 3) / 24, x = gx * w / 32 + w / 64;
                    int p = (y * w + x) * 4;
                    int l = (rgba[p] * 3 + rgba[p + 1] * 6 + rgba[p + 2]) / 10;
                    sum += l; n++; print = (print ^ (uint)(rgba[p] | rgba[p + 1] << 8 | rgba[p + 2] << 16)) * 1099511628211;
                    if (gx >= 11 && gx < 21 && gy >= 8 && gy < 16) { centre += l; nc++; }
                }
            var mem = RecompOne.Runtime.Runtime.Mem;
            int camX = mem is null ? 0 : (int)mem.ReadU32(PosX), camZ = mem is null ? 0 : (int)mem.ReadU32(PosZ);
            long retained = RetainedTotal;
            var row = new Row(now, _last < 0 ? 0 : now - _last, (int)(sum / n), (int)(centre / nc),
                GpuWorld.Frames - _gpuFrames, retained - _retained, RetainedMap.Ready, RetainedScene.MainView,
                FramePacing.Ticks, FramePacing.IterationTicked, camX, camZ, AgentBeacon.Overlay, _tinted,
                GpuWorld.ModelSubmits - _submitted, print == _print1 ? '=' : print == _print2 ? '2' : '.',
                dispY, RetainedScene.MainDraws - _draws, RetainedScene.MainTriangles - _tris, _prims);
            _draws = RetainedScene.MainDraws; _tris = RetainedScene.MainTriangles; _prims = 0;
            _gpuFrames = GpuWorld.Frames; _retained = retained; _submitted = GpuWorld.ModelSubmits; _tinted = false;
            _print2 = _print1; _print1 = print;
            _last = now;
            if (_loadAt < 0)
            {
                _before.Enqueue(row);
                while (_before.Count > 0 && now - _before.Peek().T > BeforeMs) _before.Dequeue();
                // The snapshot array is fresh per present, so it can be kept as it is.
                if (_out != null)
                {
                    _pics.Enqueue((now, row.Lum, rgba, w, h));
                    while (_pics.Count > PicBefore) _pics.Dequeue();
                }
            }
            else
            {
                _after.Add(row);
                if (_out != null) Write(now - _loadAt, row.Lum, rgba, w, h);
                if (now - _loadAt > AfterMs) Flush();
            }
        }
        PresentSnap.Request(Taken);
    }

    // Retained draws of the three domains, since the counters are kept apart.
    static long RetainedTotal => GpuWorld.MapRetained + GpuWorld.ModelRetained + GpuWorld.ArmRetained;

    // A picture's name: its offset from the load and its luminance; the sequence keeps them unique.
    static void Write(double relMs, int lum, byte[] rgba, int w, int h)
    {
        string name = Path.Combine(_out!, $"{relMs:+0000;-0000;+0000}ms_lum{lum:000}_{_seq++:00000}.png");
        _writes!.Add((name, rgba, w, h));
        _wrote++;
    }

    static void Flush()
    {
        Console.WriteLine($"[KF1] crossprobe: {_loaded}, {_before.Count} present(s) before, {_after.Count} after");
        Console.WriteLine("[KF1] crossprobe:     t ms    dt   lum  ctr px  gpu  msub retained ready main  tick t tint  y draws   tris prims overlay  posX posZ");
        foreach (var r in _before.Concat(_after))
            Console.WriteLine($"[KF1] crossprobe: {r.T - _loadAt,7:0.0} {r.Dt,5:0.0} {r.Lum,5} {r.Centre,4} {r.Same,2} {r.GpuFrames,4} {r.Submitted,5} {r.Retained,8} " +
                $"{(r.Ready ? 1 : 0),5} {(r.Main ? 1 : 0),4} {r.Tick,5} {(r.Ticked ? 1 : 0)} {(r.Tinted ? 1 : 0),4} {r.DispY,3} {r.Draws,4} {r.Tris,7} {r.Prims,5} {r.Overlay,-8} {r.CamX} {r.CamZ}");
        int dark = _after.Count(r => r.Lum < 4), slow = _after.Count(r => r.Dt > 30);
        Console.WriteLine($"[KF1] crossprobe: {_loaded}: {dark} present(s) under luminance 4, {slow} over 30 ms, " +
            $"longest {(_after.Count > 0 ? _after.Max(r => r.Dt) : 0):0.0} ms" +
            (_out == null ? "" : $", {_wrote} picture(s) queued to {Path.GetFullPath(_out)}"));
        Console.Out.Flush();
        _before.Clear(); _after.Clear(); _loadAt = -1;
    }
}
