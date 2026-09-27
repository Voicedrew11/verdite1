using System.Reflection;
using System.Text;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// What each drawn frame's ordering table actually got, across an area crossing.
///
///     KF2_CROSSPROBE=1  a per-frame dump around every fdat load
///
/// ## Why luminance could not answer this
///
/// A crossing was reported as "a moment of darkness or at least not the correct
/// frame", on **every** crossing, and with the appearance varying between them.
/// `BlackProbe` asks whether a frame is *dark*, which at a door into an unlit
/// corridor is the room and not the defect: both arms of the CD-pump A/B
/// reported the same one-frame `[black] flash` lines at luminance 3 on both
/// buffers. Two fixes were then aimed at a defect no counter could see.
///
/// The captured artifact is **HUD present, world absent**, and that is countable
/// without looking at a pixel. `TileWalk.CellsDrawn` is the map the last walk
/// submitted and `ModelWalk.Scene` is every model it submitted, so a frame that
/// drew 0 of ~200 cells is named in a pitch-black corridor exactly as it is in
/// daylight.
///
/// ## What it samples
///
/// **A post on `DrawOTag`, not on the renderer.** Sampling in the renderer's own
/// post can only ever see frames the renderer built, so a presented frame it did
/// not build is structurally invisible -- and that is one of the things worth
/// knowing. Each sample carries `built`: whether `func_800342D8` ran between this
/// `DrawOTag` and the previous one. Into a ring:
/// the time, whether the frame was a tick (`FramePacing.TickedThisFrame` — the
/// gated stages ran) or a between-tick redraw, cells walked, cells drawn, models
/// submitted, the area byte, and the view the frame was drawn from. An `fdat` load
/// marks the ring; the window from <see cref="BeforeMs"/> before it to
/// <see cref="AfterMs"/> after is dumped, one character per frame, with the census
/// underneath.
///
/// The view is in there because **"no world" is only one of the three ways a
/// crossing frame can be wrong**, and the other two are invisible to a cell count:
/// the right number of tiles from the *wrong area*, and the right world from the
/// *wrong camera*. The area byte names the first; a frame whose view jumps and
/// comes back names the second, which is what a between-tick redraw of a world
/// halfway through being replaced would look like — and it would look slightly
/// different every crossing, which is what play reports.
///
/// Diagnostic only, off by default. It allocates nothing per frame and reads two
/// longs and a span length.
/// </summary>
public static class CrossProbe
{
    /// <summary>Stage 13, the renderer that builds the frame's ordering table.</summary>
    const uint Renderer = 0x800342D8;

    const int Samples = 1200;   // ~8 s of frames at 144 fps
    const double BeforeMs = 600.0;
    const double AfterMs = 2000.0;
    const int PerLine = 40;

    public static bool Enabled { get; private set; }

    /// <summary>`KF2_CROSSPROBE=2`: also print every frame's cell count. A threshold
    /// on the count alone cannot tell "the new area has fewer tiles" from "the map is
    /// half built" -- the first version of this compared against the window's peak and
    /// called 285 of 371 frames partial, which was area 0 being smaller than area 1.
    /// The raw column is what says which.</summary>
    public static bool Verbose { get; private set; }

    static readonly double[] _t = new double[Samples];
    static readonly int[] _walked = new int[Samples];
    static readonly int[] _drawn = new int[Samples];
    static readonly int[] _models = new int[Samples];
    static readonly bool[] _tick = new bool[Samples];
    static readonly int[] _x = new int[Samples];
    static readonly int[] _z = new int[Samples];
    static readonly int[] _yaw = new int[Samples];
    static readonly int[] _area = new int[Samples];
    static readonly bool[] _built = new bool[Samples];
    static int _head, _count;

    // libgpu DrawOTag per overlay, the same three the frame boundary is taken from.
    // Posts only: Widescreen owns the one Replace.
    static readonly (string Overlay, uint Addr)[] DrawOTagAt =
    [
        .. SdkAddr.DrawOTag,
    ];

    static readonly HashSet<uint> _otHooked = [];
    static bool _rendererRan;

    /// <summary>Completed runs of the renderer since boot. A consumer that samples
    /// per presented frame records the delta, which is "did stage 13 build the
    /// picture this frame is showing". <see cref="BlackProbe"/> is the reader, and
    /// it needs a counter rather than a flag because both probes post on the same
    /// `DrawOTag` and neither may consume what the other needs.</summary>
    public static long RendererRuns { get; private set; }

    const uint AreaByte = 0x8017E060;   // u8
    const uint PosX     = 0x801994EC;   // s32
    const uint PosZ     = 0x801994F4;   // s32
    const uint Yaw      = 0x80199506;   // s16, composed view; 0x1000 to a turn

    /// <summary>Percent of the window's peak cell count below which a frame counts as
    /// drawn from a half-built map. **Partial, not empty, is the defect**: the first
    /// version of this probe counted only frames that drew *no* map and reported a
    /// clean crossing, while the frame at the swap was drawing 71 cells of 200.</summary>
    const int PartialPct = 60;

    /// <summary>World units the view may move between two drawn frames before the
    /// frame is called out. A walk is about 100 a tick, so this is well clear of
    /// anything the player can do and only a teleport or a stale camera trips it.</summary>
    const int ViewJump = 2000;

    static double _markMs = -1.0;
    static string _markName = "";
    static int _depth;
    static bool _hooked;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.crossprobe",
        Name = "Crossing probe",
        Version = "1.0",
        Description = "What each drawn frame's ordering table got, across an area crossing.",
    };

    public static void Configure(string? enabled)
    {
        if (string.IsNullOrWhiteSpace(enabled)) return;
        Enabled = enabled != "0";
        Verbose = enabled == "2";
    }

    public static void Install()
    {
        if (!Enabled) return;
        InstallRenderer();

        Event.AddListener<OverlayLoadedEvent>(e =>
        {
            if (!e.Name.StartsWith("fdat", StringComparison.Ordinal)) return;
            if (_markMs >= 0.0) return;          // a dump is already collecting
            _markMs = Interrupts.ClockMs;
            _markName = e.Name;
        });

        Console.WriteLine("[KF2] cross probe: on, every drawn frame's tile and model count around an fdat load");
    }

    static bool _installed;

    /// <summary>The renderer hook alone, for <see cref="BlackProbe"/>, whose
    /// built, camgap and camstep rows read it with this probe off.</summary>
    public static void InstallRenderer()
    {
        if (_installed) return;
        _installed = true;
        HookAttach.OnOverlayLoad("cross probe", Attach);
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var self = typeof(CrossProbe);

        if (!_hooked)
        {
            var fn = SymbolRegistry.Resolve("game", null, Renderer);
            if (fn != null)
            {
                HookManager.AddPre(_self, fn,
                    self.GetMethod(nameof(EnterRenderer), BindingFlags.Public | BindingFlags.Static)!);
                HookManager.AddPost(_self, fn,
                    self.GetMethod(nameof(LeaveRenderer), BindingFlags.Public | BindingFlags.Static)!);
                HookManager.Commit();
                _hooked = HookAttach.Installed(fn);
            }
        }

        if (!Enabled) return _hooked;

        var after = self.GetMethod(nameof(AfterDrawOTag), BindingFlags.Public | BindingFlags.Static)!;
        foreach (var (overlay, addr) in DrawOTagAt)
        {
            if (_otHooked.Contains(addr)) continue;
            var fn = SymbolRegistry.Resolve(overlay, null, addr);
            if (fn == null) continue;
            HookManager.AddPost(_self, fn, after);
            HookManager.Commit();
            if (HookAttach.Installed(fn)) _otHooked.Add(addr);
        }

        return _hooked && _otHooked.Count == DrawOTagAt.Length;
    }

    public static bool EnterRenderer(CpuContext c, IMemory m)
    {
        _depth++;
        return true;
    }

    public static void LeaveRenderer(CpuContext c, IMemory m)
    {
        if (_depth > 0) _depth--;
        if (_depth != 0) return;
        _rendererRan = true;
        RendererRuns++;

        // The camera func_8002E22C copied for this run, against the true position.
        CamX = (int)m.ReadU32(0x80192E78); CamZ = (int)m.ReadU32(0x80192E80);
        TrueX = (int)m.ReadU32(PosX); TrueZ = (int)m.ReadU32(PosZ);
        long f = FrameSmoothing.Frames;
        Stage8Ran = f != _s8Frames; _s8Frames = f;
    }

    public static int CamX, CamZ, TrueX, TrueZ;
    public static bool Stage8Ran;
    static long _s8Frames;

    /// <summary>One sample a presented frame.</summary>
    public static void AfterDrawOTag(CpuContext c, IMemory m)
    {
        var now = Interrupts.ClockMs;
        _t[_head] = now;
        _walked[_head] = (int)TileWalk.CellsWalked;
        _drawn[_head] = (int)TileWalk.CellsDrawn;
        _models[_head] = ModelWalk.Scene.Length;
        _tick[_head] = FramePacing.TickedThisFrame;
        _x[_head] = (int)m.ReadU32(PosX);
        _z[_head] = (int)m.ReadU32(PosZ);
        _yaw[_head] = (short)m.ReadU16(Yaw);
        _area[_head] = m.ReadU8(AreaByte);
        _built[_head] = _rendererRan;
        _rendererRan = false;
        _head = (_head + 1) % Samples;
        if (_count < Samples) _count++;

        if (_markMs >= 0.0 && now - _markMs >= AfterMs) Dump();
    }

    /// <summary>One character a frame. `.` a between-tick redraw that drew the map,
    /// `T` a tick that drew it; `0`/`t` the same two having drawn none of it, which
    /// is the "HUD on a black world" frame; `J` a frame whose view jumped from the
    /// one before it. `0`, `t` and `J` are the three shapes of a wrong frame.</summary>
    static char Mark(int i, int prev, IReadOnlyDictionary<int, int> peakOf)
    {
        if (!_built[i]) return '-';
        if (_drawn[i] == 0) return _tick[i] ? 't' : '0';
        if (prev >= 0 && Jumped(prev, i)) return 'J';
        if (Partial(i, peakOf)) return 'p';
        return _tick[i] ? 'T' : '.';
    }

    static bool Partial(int i, IReadOnlyDictionary<int, int> peakOf) =>
        _drawn[i] > 0 && peakOf.TryGetValue(_area[i], out var p) && p > 0 &&
        _drawn[i] * 100 < p * PartialPct;

    static bool Jumped(int a, int b)
    {
        if (_area[a] != _area[b]) return false;   // a real area change is not a jump
        long dx = _x[b] - _x[a], dz = _z[b] - _z[a];
        return dx * dx + dz * dz > (long)ViewJump * ViewJump;
    }

    static void Dump()
    {
        double lo = _markMs - BeforeMs, hi = _markMs + AfterMs;
        int first = (_head - _count + Samples) % Samples;

        var idx = new List<int>();
        for (int k = 0; k < _count; k++)
        {
            int i = (first + k) % Samples;
            if (_t[i] >= lo && _t[i] <= hi) idx.Add(i);
        }

        // The peak is taken per area, not over the window: area 0 steadily draws 71
        // cells where area 1 draws 200, so one peak for the whole crossing called 289
        // of 371 frames half-built when every one of them was right. A frame is only
        // suspect against the steady state of the area it was drawn in.
        var peakOf = new Dictionary<int, int>();
        foreach (var i in idx)
        {
            peakOf.TryGetValue(_area[i], out var p0);
            if (_drawn[i] > p0) peakOf[_area[i]] = _drawn[i];
        }

        int peak = 0;
        foreach (var i in idx)
            if (_drawn[i] > peak) peak = _drawn[i];

        var sb = new StringBuilder();
        for (int k = 0; k < idx.Count; k += PerLine)
        {
            int i0 = idx[k];
            sb.Append($"[cross] {_markName} {_t[i0] - _markMs,7:0} ms: ");
            for (int j = k; j < idx.Count && j < k + PerLine; j++)
                sb.Append(Mark(idx[j], j > 0 ? idx[j - 1] : -1, peakOf));
            sb.Append('\n');
        }

        // The census. "Drew none" is the frame with no map in its table; the steady
        // count either side is what it should have drawn.
        int blank = 0, blankTicks = 0, run = 0, longest = 0;
        int partial = 0, partialLow = int.MaxValue;
        double blankMs = 0.0;
        for (int k = 0; k < idx.Count; k++)
        {
            int i = idx[k];
            if (Partial(i, peakOf))
            {
                partial++;
                if (_drawn[i] < partialLow) partialLow = _drawn[i];
            }

            if (_drawn[i] > 0) { run = 0; continue; }
            blank++;
            if (_tick[i]) blankTicks++;
            if (k > 0) blankMs += _t[i] - _t[idx[k - 1]];
            if (++run > longest) longest = run;
        }
        if (partialLow == int.MaxValue) partialLow = 0;

        int models = 0, noModels = 0;
        foreach (var i in idx)
        {
            if (_models[i] > models) models = _models[i];
            if (_models[i] == 0) noModels++;
        }

        int unbuilt = 0;
        foreach (var i in idx)
            if (!_built[i]) unbuilt++;

        int jumps = 0, areaFlips = 0;
        for (int k = 1; k < idx.Count; k++)
        {
            if (Jumped(idx[k - 1], idx[k])) jumps++;
            if (_area[idx[k]] != _area[idx[k - 1]]) areaFlips++;
        }

        sb.Append($"[cross] {_markName}: {idx.Count} frame(s) drawn, peak {peak} cell(s); " +
                  $"{partial} drew a half-built map (fewest {partialLow}), " +
                  $"{blank} drew none (~{blankMs:0} ms, longest run {longest}, {blankTicks} a tick); " +
                  $"{noModels} frame(s) submitted no model, peak {models}; " +
                  $"{jumps} frame(s) drawn from a jumped view, {areaFlips} area change(s) in the window; " +
                  $"{unbuilt} frame(s) presented that the renderer never built");
        if (Verbose)
            for (int k = 0; k < idx.Count; k += 20)
            {
                sb.Append($"\n[cross] {_markName} cells {_t[idx[k]] - _markMs,7:0} ms:");
                for (int j = k; j < idx.Count && j < k + 20; j++)
                    sb.Append($" {_drawn[idx[j]],3}{(_tick[idx[j]] ? "T" : " ")}");
            }

        // Each jumped frame named, with where the view came from and went to. Two of
        // these a crossing is the signal; what the pair says is whether the view
        // went forward and stayed (one real teleport, split by a lagging area byte)
        // or went, came back and went again (a frame drawn from a stale camera).
        for (int k = 1; k < idx.Count; k++)
        {
            int a = idx[k - 1], b = idx[k];
            if (!Jumped(a, b)) continue;
            sb.Append($"\n[cross] {_markName} {_t[b] - _markMs,7:0} ms: view " +
                      $"({_x[a]},{_z[a]}) area {_area[a]} -> ({_x[b]},{_z[b]}) area {_area[b]}, " +
                      $"{(_tick[b] ? "tick" : "redraw")}, {_drawn[b]} cell(s)");
        }

        Console.WriteLine(sb.ToString());

        _markMs = -1.0;
    }
}
