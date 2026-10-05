using System.Diagnostics;
using System.Reflection;

namespace RecompOne.Runtime.Diagnostics;

/// <summary>What a section's time is spent on, which is what decides whether it is
/// a cost to chase: <see cref="Wait"/> is a deliberate sleep to a deadline, and
/// <see cref="Gpu"/> is the thread blocked on the driver, not running code.</summary>
/// <summary>0046. See <see cref="Profiler.Trace"/>.</summary>
public interface IProfileTrace
{
    void Enter(int id, long timestamp);
    void Leave(int id, long timestamp);
}

public enum ProfileGroup : byte
{
    /// <summary>Recompiled MIPS: the frame's unattributed remainder, and the body of
    /// every hooked game function.</summary>
    Game,
    /// <summary>A port or mod hook -- a pre, post or replace delegate's own time.</summary>
    Hook,
    /// <summary>The runtime's own work: HLE, GL submission, the interface.</summary>
    Runtime,
    /// <summary>Blocked on the GL driver: the buffer swap.</summary>
    Gpu,
    /// <summary>Sleeping or spinning to a deadline.</summary>
    Wait,
}

/// <summary>
/// 0045. A per-frame, per-section profiler for the game thread.
///
/// Sections nest: <see cref="Begin"/> returns a token and <see cref="End"/> closes
/// everything down to it, so an exception that skipped an inner End is repaired by
/// the next outer one. Each close charges its section with *self* time (elapsed
/// minus its children) and, for the outermost instance of a section on the stack,
/// *inclusive* time. Whatever no section claims is charged to <see cref="Root"/>,
/// which is the recompiled game itself, so a frame's self times sum exactly to its
/// length.
///
/// The frame boundary is <see cref="FrameMark"/>, called at the end of
/// <c>Runtime.PresentFrame</c> -- the present, not a hook, so the boundary does not
/// depend on anything this is meant to be measuring. Sections still open at the
/// boundary (a hooked stage running a modal loop that presents its own frames, the
/// VSync that is presenting) are split there: the time so far goes to the frame
/// that ends, and the rest to the next.
///
/// Off, every site costs one static bool test. Only the thread that presents is
/// profiled; a Begin from any other thread returns -1 and its End does nothing.
/// </summary>
public static class Profiler
{
    public const int MaxSections = 2048;
    const int MaxDepth = 128;
    public const int HistoryFrames = 2048;

    /// <summary>Whether sections are recorded. Checked at every site.</summary>
    public static bool Enabled;

    /// <summary>0046. Receives every section's enter and leave, with the timestamp
    /// the profiler itself took, for a frame capture. Sections are recorded while
    /// this is set even with <see cref="Enabled"/> off.</summary>
    public static IProfileTrace? Trace;

    public static bool Recording => Enabled || Trace != null;

    /// <summary>Profile the calling thread, if no frame has claimed one yet.</summary>
    public static void AdoptThread()
    {
        if (_thread < 0) _thread = Environment.CurrentManagedThreadId;
    }

    public static readonly double TicksToMs = 1000.0 / Stopwatch.Frequency;

    // ---- the section registry ------------------------------------------------

    static readonly object _registry = new();
    static readonly Dictionary<string, int> _byName = new(StringComparer.Ordinal);
    static readonly string[] _names = new string[MaxSections];
    static readonly string?[] _labels = new string?[MaxSections];
    static readonly ProfileGroup[] _groups = new ProfileGroup[MaxSections];
    static int _count;

    public static int SectionCount => _count;
    public static string Name(int id) => _names[id];
    public static string? Label(int id) => _labels[id];
    public static ProfileGroup Group(int id) => _groups[id];

    /// <summary>The name to show: the label if one was set, then the name.</summary>
    public static string DisplayName(int id) => _labels[id] is { } l ? $"{l} ({_names[id]})" : _names[id];

    /// <summary>The section for <paramref name="name"/>, created on first use. The same
    /// name is always the same id, so a hook re-added after a mod reload keeps its row.
    /// Past <see cref="MaxSections"/> everything lands in one overflow section.</summary>
    public static int Register(string name, ProfileGroup group)
    {
        lock (_registry)
        {
            if (_byName.TryGetValue(name, out var id)) return id;
            if (_count >= MaxSections - 1)
                return _byName.TryGetValue("(too many sections)", out var o) ? o : Add("(too many sections)", group);
            return Add(name, group);
        }

        static int Add(string name, ProfileGroup group)
        {
            var id = _count;
            _names[id] = name;
            _groups[id] = group;
            _byName[name] = id;
            _count = id + 1;
            return id;
        }
    }

    /// <summary>A human name for a section, shown beside its own -- for a recompiled
    /// function, what the address is known to do.</summary>
    public static void SetLabel(string name, string label, ProfileGroup group = ProfileGroup.Game)
        => _labels[Register(name, group)] = label;

    /// <summary>The section name for a recompiled function. Overlays that load at the
    /// same address share function names, so it carries the overlay class's suffix:
    /// <c>func_80060818@game</c>.</summary>
    public static string FunctionName(MethodInfo target)
    {
        var type = target.DeclaringType?.Name ?? "";
        var cut = type.LastIndexOf('_');
        return $"{target.Name}@{(cut >= 0 ? type[(cut + 1)..] : type)}";
    }

    public static int Find(string name)
    {
        lock (_registry) return _byName.TryGetValue(name, out var id) ? id : -1;
    }

    // The runtime's own sections. Root must be registered first: it is id 0.
    public static readonly int Root = Register("game code (no section)", ProfileGroup.Game);
    public static readonly int VSync = Register("LibEtc.VSync", ProfileGroup.Runtime);
    public static readonly int VBlank = Register("vblank grid (IRQ 0, VSyncEvent listeners)", ProfileGroup.Runtime);
    public static readonly int VBlankWait = Register("LibEtc.WaitVBlanks (blocking timeline)", ProfileGroup.Wait);
    public static readonly int Present = Register("Runtime.PresentFrame", ProfileGroup.Runtime);
    public static readonly int HostEvents = Register("window events + input poll", ProfileGroup.Runtime);
    public static readonly int HostRender = Register("buffer swap + driver", ProfileGroup.Gpu);
    public static readonly int ImGuiUpdate = Register("ImGui new frame", ProfileGroup.Runtime);
    public static readonly int Display = Register("GlCore.PresentDisplay (compose the picture)", ProfileGroup.Runtime);
    public static readonly int Panels = Register("menu bar + panels + popups", ProfileGroup.Runtime);
    public static readonly int ImGuiRender = Register("ImGui render", ProfileGroup.Runtime);
    public static readonly int GlFlush = Register("GlCore.Flush (GL batch submit)", ProfileGroup.Runtime);
    public static readonly int DrawOTag = Register("LibGpu.DrawOTag (packet walk)", ProfileGroup.Runtime);
    public static readonly int Throttle = Register("FrameClock.Throttle (host ceiling)", ProfileGroup.Wait);
    public static readonly int VSyncWait = Register("VSync wait (Wayland: the CPU grid; Windows: the display's vblank)", ProfileGroup.Wait);
    public static readonly int Ticks = Register("CD, card and pad ticks", ProfileGroup.Runtime);
    public static readonly int Self = Register("profiler (its own reporting)", ProfileGroup.Runtime);
    public static readonly int Ao = Register("GlCore ambient occlusion pass", ProfileGroup.Runtime);
    public static readonly int Composite = Register("GlCore present blit + post-fx", ProfileGroup.Runtime);
    public static readonly int Writeback = Register("GlCore.Writeback (target to VRAM)", ProfileGroup.Runtime);
    public static readonly int VertexLookup = Register("vertex attribute lookup (perspective, sub-pixel, depth)", ProfileGroup.Runtime);
    public static readonly int Surfaces = Register("GlCore surface buffer (normals, materials)", ProfileGroup.Runtime);
    public static readonly int Ssr = Register("GlCore screen-space reflection pass", ProfileGroup.Runtime);

    // ---- the open stack, and this frame's accumulators --------------------------

    static readonly int[] _stId = new int[MaxDepth];
    static readonly long[] _stStart = new long[MaxDepth];
    static readonly long[] _stChild = new long[MaxDepth];
    static int _depth;
    static long _rootChild;

    static readonly long[] _self = new long[MaxSections];
    static readonly long[] _incl = new long[MaxSections];
    static readonly int[] _calls = new int[MaxSections];
    static readonly int[] _open = new int[MaxSections];
    static readonly bool[] _touched = new bool[MaxSections];
    static readonly int[] _touchedIds = new int[MaxSections];
    static int _touchedCount;

    static int _thread = -1;
    static long _frameStart;
    static long _frameIndex;

    static TimeSpan _gcPause;
    static int _gc0, _gc1, _gc2;
    static long _alloc;
    static TimeSpan _jit;
    static long _jitMethods;

    public static int Begin(int id)
    {
        if (!(Enabled || Trace != null) || _depth >= MaxDepth || Environment.CurrentManagedThreadId != _thread) return -1;

        var d = _depth++;
        _stId[d] = id;
        _stChild[d] = 0;
        _open[id]++;
        if (!_touched[id]) Touch(id);
        _calls[id]++;
        _stStart[d] = Stopwatch.GetTimestamp();
        Trace?.Enter(id, _stStart[d]);
        return d;
    }

    public static void End(int token)
    {
        if (token < 0 || token >= _depth || Environment.CurrentManagedThreadId != _thread) return;

        var now = Stopwatch.GetTimestamp();
        while (_depth > token)
        {
            var d = --_depth;
            var id = _stId[d];
            var el = now - _stStart[d];
            Trace?.Leave(id, now);
            if (!_touched[id]) Touch(id);
            _self[id] += el - _stChild[d];
            if (--_open[id] <= 0)
            {
                _open[id] = 0;
                _incl[id] += el;
            }

            if (d > 0) _stChild[d - 1] += el;
            else _rootChild += el;
        }
    }

    static void Touch(int id)
    {
        _touched[id] = true;
        _touchedIds[_touchedCount++] = id;
    }

    // ---- frames ----------------------------------------------------------------

    public readonly struct Sample(int id, long self, long incl, int calls)
    {
        public readonly int Id = id;
        public readonly long Self = self;
        public readonly long Incl = incl;
        public readonly int Calls = calls;
        public double SelfMs => Self * TicksToMs;
        public double InclMs => Incl * TicksToMs;
    }

    public sealed class Frame
    {
        public long Index;
        /// <summary>Stopwatch timestamp the frame started at.</summary>
        public long Start;
        public long Ticks;
        public Sample[] Samples = new Sample[64];
        public int Count;
        public long WaitTicks, GpuTicks;
        public double GcPauseMs;
        public int Gc0, Gc1, Gc2;
        public long AllocBytes;
        public double JitMs;
        public long JitMethods;

        public double Ms => Ticks * TicksToMs;
        /// <summary>The frame less its sleeps and its swap: the time something ran.</summary>
        public double WorkMs => (Ticks - WaitTicks - GpuTicks) * TicksToMs;
        public double WaitMs => WaitTicks * TicksToMs;
        public double GpuMs => GpuTicks * TicksToMs;
        public ReadOnlySpan<Sample> Span => Samples.AsSpan(0, Count);
    }

    static readonly Frame[] _history = CreateHistory();
    static int _head;          // the slot the next frame is written to
    static int _filled;

    static Frame[] CreateHistory()
    {
        var h = new Frame[HistoryFrames];
        for (var i = 0; i < h.Length; i++) h[i] = new Frame();
        return h;
    }

    /// <summary>Frames recorded, up to <see cref="HistoryFrames"/>.</summary>
    public static int HistoryCount => _filled;

    /// <summary>A recorded frame, 0 being the most recent. The history is written on
    /// the game thread, which is also the thread the interface draws on.</summary>
    public static Frame GetFrame(int ago) => _history[(_head - 1 - ago + 2 * HistoryFrames) % HistoryFrames];

    /// <summary>Raised on the game thread for each finished frame, inside the
    /// <see cref="Self"/> section of the next one.</summary>
    public static event Action<Frame>? FrameCompleted;

    public static void ClearHistory()
    {
        _filled = 0;
        _head = 0;
    }

    public static void FrameMark()
    {
        if (!Enabled)
        {
            _frameStart = 0;
            return;
        }

        var tid = Environment.CurrentManagedThreadId;
        if (_thread < 0) _thread = tid;
        else if (tid != _thread) return;

        var now = Stopwatch.GetTimestamp();
        if (_frameStart == 0)
        {
            // The first mark after being switched on: nothing before it was a
            // whole frame, so throw it away and start here.
            Discard();
            Snapshot(out _, out _, out _, out _, out _, out _, out _);
            _frameStart = now;
            return;
        }

        Split(now);
        var frame = Commit(now);
        _frameStart = now;

        if (FrameCompleted is { } handlers)
        {
            var t = Begin(Self);
            try
            {
                handlers(frame);
            }
            catch (Exception e)
            {
                FrameCompleted = null;
                Console.Error.WriteLine($"[Profiler] a frame listener threw and was removed: {e}");
            }
            finally
            {
                End(t);
            }
        }
    }

    /// <summary>Charge every open section with its time up to <paramref name="now"/>
    /// and restart it there, innermost first so each parent sees its children.</summary>
    static void Split(long now)
    {
        for (var d = _depth - 1; d >= 0; d--)
        {
            var id = _stId[d];
            var el = now - _stStart[d];
            if (!_touched[id]) Touch(id);
            _self[id] += el - _stChild[d];

            var outermost = true;
            for (var p = 0; p < d; p++)
                if (_stId[p] == id)
                {
                    outermost = false;
                    break;
                }

            if (outermost) _incl[id] += el;
            if (d > 0) _stChild[d - 1] += el;
            else _rootChild += el;
        }

        for (var d = 0; d < _depth; d++)
        {
            _stStart[d] = now;
            _stChild[d] = 0;
        }
    }

    static Frame Commit(long now)
    {
        var f = _history[_head];
        _head = (_head + 1) % HistoryFrames;
        if (_filled < HistoryFrames) _filled++;

        var ticks = now - _frameStart;
        f.Index = _frameIndex++;
        f.Start = _frameStart;
        f.Ticks = ticks;

        if (!_touched[Root]) Touch(Root);
        _self[Root] += Math.Max(0, ticks - _rootChild);
        _incl[Root] = ticks;

        if (f.Samples.Length < _touchedCount) f.Samples = new Sample[Math.Max(_touchedCount, f.Samples.Length * 2)];

        long wait = 0, gpu = 0;
        var n = 0;
        for (var i = 0; i < _touchedCount; i++)
        {
            var id = _touchedIds[i];
            f.Samples[n++] = new Sample(id, _self[id], _incl[id], _calls[id]);
            if (_groups[id] == ProfileGroup.Wait) wait += _self[id];
            else if (_groups[id] == ProfileGroup.Gpu) gpu += _self[id];
        }

        f.Count = n;
        f.WaitTicks = wait;
        f.GpuTicks = gpu;

        Snapshot(out var pause, out var g0, out var g1, out var g2, out var alloc, out var jit, out var jitN);
        f.GcPauseMs = (pause - _gcPause).TotalMilliseconds;
        f.Gc0 = g0 - _gc0;
        f.Gc1 = g1 - _gc1;
        f.Gc2 = g2 - _gc2;
        f.AllocBytes = alloc - _alloc;
        f.JitMs = (jit - _jit).TotalMilliseconds;
        f.JitMethods = jitN - _jitMethods;
        (_gcPause, _gc0, _gc1, _gc2, _alloc, _jit, _jitMethods) = (pause, g0, g1, g2, alloc, jit, jitN);

        Discard();
        return f;
    }

    static void Discard()
    {
        for (var i = 0; i < _touchedCount; i++)
        {
            var id = _touchedIds[i];
            _self[id] = 0;
            _incl[id] = 0;
            _calls[id] = 0;
            _touched[id] = false;
        }

        _touchedCount = 0;
        _rootChild = 0;
    }

    static void Snapshot(out TimeSpan pause, out int g0, out int g1, out int g2, out long alloc,
                         out TimeSpan jit, out long jitMethods)
    {
        pause = GC.GetTotalPauseDuration();
        g0 = GC.CollectionCount(0);
        g1 = GC.CollectionCount(1);
        g2 = GC.CollectionCount(2);
        alloc = GC.GetAllocatedBytesForCurrentThread();
        jit = System.Runtime.JitInfo.GetCompilationTime();
        jitMethods = System.Runtime.JitInfo.GetCompiledMethodCount();
    }
}
