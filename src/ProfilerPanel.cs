using System.Globalization;
using System.Numerics;
using System.Text;
using ImGuiNET;
using RecompOne.Runtime.Diagnostics;
using RecompOne.Runtime.Host.Window;

namespace Verdite.Core;

/// <summary>
/// The frame profiler's window: Shift+P, or the port's <c>{TAG}_PROFILE=panel</c>.
///
/// A bar per recorded frame, stacked by what the time was spent on, then a table of
/// every section over the chosen window -- or over one frame, once a bar or a spike
/// is clicked. **Self** time is what a section did itself, so the column sums to the
/// frame; **incl** adds what it called. The panel draws inside the frame it measures
/// (the "menu bar + panels + popups" row), so that row is partly this window.
///
/// The port sets three things with <see cref="Configure"/>: the frame's target, the
/// address the probe box starts with, and how many stages the stage button times.
/// </summary>
public sealed class ProfilerPanel : IPanel
{
    public static readonly ProfilerPanel Instance = new();
    ProfilerPanel() { }

    static Func<double> _target = () => 0;
    static string _defaultProbe = "";
    static int _stages;

    /// <summary>
    /// The port's data for the panel: <paramref name="targetMs"/> is the frame the pacer
    /// aims at in milliseconds, or 0 for none; <paramref name="defaultProbe"/> is the
    /// address the probe box starts with; <paramref name="stages"/> is how many main-loop
    /// stages the stage button times. Called once, before the panel first draws.
    /// </summary>
    public static void Configure(Func<double> targetMs, string defaultProbe, int stages)
    {
        _target = targetMs;
        _defaultProbe = defaultProbe;
        _stages = stages;
        Instance._probeInput = defaultProbe;
    }

    public string Name => $"{Game.Id}profiler";
    public string TitleKey => $"{Game.Id}.profiler";

    bool _open;
    public bool IsOpen
    {
        get => _open;
        set
        {
            _open = value;
            FrameProfiler.UpdateEnabled();
        }
    }

    // The ImGui ids are built once, from the game's tag, so two ports' panels do not
    // share an id if both are ever open in one process.
    readonly string _graphId = $"##{Game.Id}profgraph";
    readonly string _gpuId = $"##{Game.Id}profgpu";
    readonly string _filterId = $"##{Game.Id}proffilter";
    readonly string _sideId = $"##{Game.Id}profside";
    readonly string _tableId = $"##{Game.Id}proftable";
    readonly string _spikeListId = $"##{Game.Id}profspikelist";
    readonly string _probeId = $"##{Game.Id}profprobe";
    readonly string _spikesId = $"###{Game.Id}profspikes";

    float _windowSeconds = 3f;
    float _spikeMs;             // 0 = automatic
    bool _showWaits;
    bool _fitTallest;
    long _selected = -1;        // a frame's Index, or -1 for the window
    string _filter = "";
    string _probeInput = "";
    string _probeResult = "";

    int _sortColumn = 2;
    bool _sortAscending;

    static readonly string[] GroupNames = ["game", "hook", "runtime", "swap", "wait"];

    static uint Colour(ProfileGroup g) => g switch
    {
        ProfileGroup.Game => ImGui.GetColorU32(new Vector4(0.93f, 0.60f, 0.22f, 1f)),
        ProfileGroup.Hook => ImGui.GetColorU32(new Vector4(0.62f, 0.45f, 0.90f, 1f)),
        ProfileGroup.Runtime => ImGui.GetColorU32(new Vector4(0.30f, 0.62f, 0.92f, 1f)),
        ProfileGroup.Gpu => ImGui.GetColorU32(new Vector4(0.90f, 0.33f, 0.35f, 1f)),
        _ => ImGui.GetColorU32(new Vector4(0.45f, 0.45f, 0.48f, 0.55f)),
    };

    public void Draw()
    {
        bool open = IsOpen;
        ImGui.SetNextWindowSize(new Vector2(760, 640), ImGuiCond.FirstUseEver);
        if (!ImGui.Begin(this.Title(), ref open))
        {
            if (open != IsOpen) IsOpen = open;
            ImGui.End();
            return;
        }
        if (open != IsOpen) IsOpen = open;

        DrawToolbar();

        if (Profiler.HistoryCount == 0)
        {
            ImGui.TextDisabled(FrameProfiler.Paused ? "Paused, and nothing recorded yet." : "Waiting for the first frame.");
            ImGui.End();
            return;
        }

        Aggregate();
        DrawSummary();
        DrawGraph();
        DrawSpikes();
        DrawTable();
        DrawProbes();
        ImGui.End();
    }

    // ---- toolbar -------------------------------------------------------------------

    void DrawToolbar()
    {
        bool paused = FrameProfiler.Paused;
        if (ImGui.Checkbox("Pause", ref paused))
        {
            FrameProfiler.Paused = paused;
            FrameProfiler.UpdateEnabled();
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip("Stop recording and hold the history still to read it.");

        ImGui.SameLine();
        bool keep = FrameProfiler.KeepRecording;
        if (ImGui.Checkbox("Record while closed", ref keep))
        {
            FrameProfiler.KeepRecording = keep;
            FrameProfiler.UpdateEnabled();
        }

        ImGui.SameLine();
        if (ImGui.Button("Clear"))
        {
            Profiler.ClearHistory();
            _selected = -1;
            _aggDirty = true;
        }

        ImGui.SameLine();
        if (ImGui.Button("Save CSV")) _probeResult = SaveCsv();
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("Write every recorded frame to profile-<time>.csv in the working directory. " +
                             "scripts/profile_report.py reads it.");

        ImGui.SetNextItemWidth(140);
        if (ImGui.SliderFloat("Window", ref _windowSeconds, 0.5f, 12f, "%.1f s")) _aggDirty = true;
        ImGui.SameLine();
        ImGui.SetNextItemWidth(140);
        if (ImGui.SliderFloat("Spike", ref _spikeMs, 0f, 100f, _spikeMs <= 0 ? "auto" : "%.1f ms work")) _aggDirty = true;
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("A frame whose work (the frame less sleeps and the swap) passes this is listed as a " +
                             "spike. Auto is twice the window's median work plus 2 ms.");
        ImGui.SameLine();
        if (ImGui.Checkbox("Show waits", ref _showWaits)) _aggDirty = true;
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("Stack the sleeps into the bars and list them in the table. Off, a capped frame " +
                             "shows only the time something ran.");
        ImGui.SameLine();
        ImGui.Checkbox("Fit tallest", ref _fitTallest);
    }

    // ---- aggregation ------------------------------------------------------------------

    readonly long[] _aggSelf = new long[Profiler.MaxSections];
    readonly long[] _aggMax = new long[Profiler.MaxSections];
    readonly long[] _aggIncl = new long[Profiler.MaxSections];
    readonly long[] _aggCalls = new long[Profiler.MaxSections];
    readonly List<int> _rows = new();
    readonly List<double> _sortedMs = new();
    readonly List<double> _sortedWork = new();
    int _aggFrames;
    double _aggSeconds, _avgMs, _p50, _p99, _maxMs, _avgWork, _avgWait, _avgGpu, _gcMs, _jitMs, _allocKb, _medianWork;
    int _gcCount;
    // GPU time over the window, from the frames whose queries have all resolved.
    readonly double[] _gpuSum = new double[GpuTimes.Passes], _gpuTmp = new double[GpuTimes.Passes];
    readonly double[] _gpuPassMax = new double[GpuTimes.Passes];
    double _gpuMax;
    int _gpuFrames;
    long _aggNewest = -1;
    double _aggAt;
    long _aggSelected = -2;
    bool _aggDirty;

    Profiler.Frame? FindFrame(long index)
    {
        for (var i = 0; i < Profiler.HistoryCount; i++)
            if (Profiler.GetFrame(i).Index == index) return Profiler.GetFrame(i);
        return null;
    }

    void Aggregate()
    {
        var newest = Profiler.GetFrame(0);
        var now = ImGui.GetTime();
        if (!_aggDirty && _aggSelected == _selected && newest.Index == _aggNewest) return;
        if (!_aggDirty && _aggSelected == _selected && now - _aggAt < 0.25) return;
        _aggDirty = false;
        _aggAt = now;
        _aggNewest = newest.Index;
        _aggSelected = _selected;

        Array.Clear(_aggSelf);
        Array.Clear(_aggMax);
        Array.Clear(_aggIncl);
        Array.Clear(_aggCalls);
        _sortedMs.Clear();
        _sortedWork.Clear();
        double work = 0, wait = 0, gpu = 0, gc = 0, jit = 0, alloc = 0;
        _gcCount = 0;
        Array.Clear(_gpuSum);
        Array.Clear(_gpuPassMax);
        _gpuMax = 0;
        _gpuFrames = 0;

        var horizon = newest.Start + newest.Ticks - (long)(_windowSeconds * 1000.0 / Profiler.TicksToMs);
        long oldestStart = newest.Start;
        for (var i = 0; i < Profiler.HistoryCount; i++)
        {
            var f = Profiler.GetFrame(i);
            if (f.Start < horizon) break;
            oldestStart = f.Start;
            Add(f);
        }

        _aggFrames = _sortedMs.Count;
        _aggSeconds = (newest.Start + newest.Ticks - oldestStart) * Profiler.TicksToMs / 1000.0;
        _sortedMs.Sort();
        _sortedWork.Sort();
        var n = Math.Max(1, _aggFrames);
        _avgMs = _sortedMs.Sum() / n;
        _p50 = Percentile(_sortedMs, 0.5);
        _p99 = Percentile(_sortedMs, 0.99);
        _maxMs = _sortedMs.Count > 0 ? _sortedMs[^1] : 0;
        _medianWork = Percentile(_sortedWork, 0.5);
        _avgWork = work / n;
        _avgWait = wait / n;
        _avgGpu = gpu / n;
        _gcMs = gc;
        _jitMs = jit;
        _allocKb = alloc / 1024.0 / n;

        // The table reads one frame when one is selected; the header above it still
        // describes the window, which is what the selection is being compared with.
        if (_selected >= 0 && FindFrame(_selected) is { } sel)
        {
            Array.Clear(_aggSelf);
            Array.Clear(_aggMax);
            Array.Clear(_aggIncl);
            Array.Clear(_aggCalls);
            foreach (var s in sel.Span)
            {
                _aggSelf[s.Id] = s.Self;
                _aggMax[s.Id] = s.Self;
                _aggIncl[s.Id] = s.Incl;
                _aggCalls[s.Id] = s.Calls;
            }
        }
        else if (_selected >= 0) _selected = -1;

        _rows.Clear();
        for (var id = 0; id < Profiler.SectionCount; id++)
            if (_aggCalls[id] > 0 || _aggSelf[id] > 0) _rows.Add(id);
        SortRows();
        BuildRows();
        BuildSpikes();
        BuildSummary();
        return;

        void Add(Profiler.Frame f)
        {
            _sortedMs.Add(f.Ms);
            _sortedWork.Add(f.WorkMs);
            work += f.WorkMs;
            wait += f.WaitMs;
            gpu += f.GpuMs;
            gc += f.GcPauseMs;
            jit += f.JitMs;
            alloc += f.AllocBytes;
            _gcCount += f.Gc0 + f.Gc1 + f.Gc2;
            if (GpuFrames.TryGet(f.Index, _gpuTmp))
            {
                _gpuFrames++;
                for (var i = 0; i < GpuTimes.Passes; i++)
                {
                    _gpuSum[i] += _gpuTmp[i];
                    _gpuPassMax[i] = Math.Max(_gpuPassMax[i], _gpuTmp[i]);
                }
                _gpuMax = Math.Max(_gpuMax, GpuFrames.Total(_gpuTmp));
            }
            foreach (var s in f.Span)
            {
                _aggSelf[s.Id] += s.Self;
                _aggIncl[s.Id] += s.Incl;
                _aggCalls[s.Id] += s.Calls;
                if (s.Self > _aggMax[s.Id]) _aggMax[s.Id] = s.Self;
            }
        }
    }

    static double Percentile(List<double> sorted, double p)
        => sorted.Count == 0 ? 0 : sorted[Math.Min(sorted.Count - 1, (int)(sorted.Count * p))];

    double SpikeThreshold => _spikeMs > 0 ? _spikeMs : _medianWork * 2 + 2;

    // ---- the header -------------------------------------------------------------------

    // Built when the numbers change, not every frame the panel draws.
    string _summaryFrame = "", _summaryWork = "", _summaryGc = "", _summaryGpu = "";
    readonly string[] _gpuLegend = new string[GpuTimes.Passes];
    string? _summarySelected;

    void BuildSummary()
    {
        var fps = _aggSeconds > 0 ? _aggFrames / _aggSeconds : 0;
        _summaryFrame = $"{fps:0.0} fps over {_aggFrames} frames   frame {_avgMs:0.00} ms avg, {_p50:0.00} median, " +
                        $"{_p99:0.00} p99, {_maxMs:0.00} max";
        _summaryWork = $"work {_avgWork:0.00} ms   swap {_avgGpu:0.00} ms   wait {_avgWait:0.00} ms";
        _summaryGc = $"  GC {_gcCount} collection(s), {_gcMs:0.0} ms paused   JIT {_jitMs:0.0} ms   " +
                     $"{_allocKb:0.0} KB/frame allocated";
        _summarySelected = _selected >= 0 && FindFrame(_selected) is { } f
            ? $"Frame {f.Index}: {f.Ms:0.00} ms, work {f.WorkMs:0.00}, swap {f.GpuMs:0.00}, wait {f.WaitMs:0.00}" +
              (f.GcPauseMs > 0 ? $", GC {f.GcPauseMs:0.00} ms" : "") +
              (f.JitMs > 0.05 ? $", JIT {f.JitMs:0.00} ms / {f.JitMethods} methods" : "") +
              $", {f.AllocBytes / 1024.0:0.0} KB" + GpuOf(f.Index)
            : null;

        var n = Math.Max(1, _gpuFrames);
        _summaryGpu = !GpuTimes.Supported ? "GPU: this backend has no timer queries"
            : _gpuFrames == 0 ? "GPU: waiting for the first frame to resolve"
            : $"GPU {_gpuSum.Sum() / n:0.00} ms avg, {_gpuMax:0.00} max over {_gpuFrames} frames";
        for (var i = 0; i < GpuTimes.Passes; i++)
            _gpuLegend[i] = $"■ {GpuTimes.Names[i]} {_gpuSum[i] / n:0.00}";
    }

    string GpuOf(long index)
    {
        if (!GpuFrames.TryGet(index, _gpuTmp)) return ", GPU not resolved yet";
        return $", GPU {GpuFrames.Total(_gpuTmp):0.00} ms (" +
               string.Join(", ", Enumerable.Range(0, GpuTimes.Passes).Where(i => _gpuTmp[i] > 0)
                   .Select(i => $"{GpuTimes.Names[i]} {_gpuTmp[i]:0.00}")) + ")";
    }

    void DrawSummary()
    {
        ImGui.TextUnformatted(_summaryFrame);
        ImGui.TextUnformatted(_summaryWork);
        ImGui.SameLine();
        ImGui.PushStyleColor(ImGuiCol.Text, ImGui.GetStyle().Colors[(int)ImGuiCol.TextDisabled]);
        ImGui.TextUnformatted(_summaryGc);
        ImGui.PopStyleColor();
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("Over the window. GC pauses stop every thread, the audio mixer's included. " +
                             "Allocation is the game thread's own. JIT is methods compiled for the first time -- " +
                             "QuickJit is off, so first-hit code compiles fully optimised and can spike a frame.");

        ImGui.TextUnformatted(_summaryGpu);
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("GPU time a frame, from timer queries around every batch submit and each present " +
                             "pass (runtime 0084), read back once the GPU has finished them, a few frames late. " +
                             "Not counted: VRAM uploads, writebacks, the interface and the swap.");

        if (_selected >= 0 && _summarySelected != null)
        {
            ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.85f, 0.35f, 1f));
            ImGui.TextUnformatted(_summarySelected);
            ImGui.PopStyleColor();
            ImGui.SameLine();
            if (ImGui.SmallButton("back to the window")) _selected = -1;
        }
    }

    // ---- the graph ----------------------------------------------------------------------

    const int GroupCount = 5;
    static readonly string[] Legend = [.. GroupNames.Select(n => "■ " + n)];

    // Keyed by frame index, which never repeats, in a table the history's size.
    readonly long[] _barIndex = NewBarIndex();
    readonly long[] _barGroups = new long[Profiler.HistoryFrames * GroupCount];
    double _targetMs = -1, _scaleTop = -1, _scaleSpike = -1;
    string _targetText = "", _scaleText = "";

    static long[] NewBarIndex()
    {
        var a = new long[Profiler.HistoryFrames];
        Array.Fill(a, -1L);
        return a;
    }

    ReadOnlySpan<long> GroupTicks(Profiler.Frame f)
    {
        var slot = (int)(f.Index % Profiler.HistoryFrames);
        var groups = _barGroups.AsSpan(slot * GroupCount, GroupCount);
        if (_barIndex[slot] == f.Index) return groups;

        groups.Clear();
        foreach (var s in f.Span) groups[(int)Profiler.Group(s.Id)] += s.Self;
        _barIndex[slot] = f.Index;
        return groups;
    }

    void DrawGraph()
    {
        var width = ImGui.GetContentRegionAvail().X;
        const float height = 130f;
        var p0 = ImGui.GetCursorScreenPos();
        var dl = ImGui.GetWindowDrawList();
        dl.AddRectFilled(p0, p0 + new Vector2(width, height), ImGui.GetColorU32(ImGuiCol.FrameBg));

        const float barW = 3f;
        var bars = Math.Min(Profiler.HistoryCount, (int)(width / barW));

        // The scale: the tallest bar when asked, otherwise the p99 with room above it
        // and never less than the frame the pacer is aiming at.
        double target = _target();
        double top = 0;
        for (var i = 0; i < bars; i++)
        {
            var f = Profiler.GetFrame(i);
            top = Math.Max(top, _showWaits ? f.Ms : f.WorkMs + f.GpuMs);
        }
        if (!_fitTallest) top = Math.Min(top, Math.Max(_p99 * 1.3, target * 1.25));
        top = Math.Max(top, Math.Max(target * 1.1, 2.0));
        var scale = height / top;

        var mouse = ImGui.GetMousePos();
        var hovered = -1;
        for (var i = 0; i < bars; i++)
        {
            var f = Profiler.GetFrame(i);
            var x1 = p0.X + width - i * barW;
            var x0 = x1 - barW + 1f;

            var stack = GroupTicks(f);

            var y = p0.Y + height;
            for (var g = 0; g < stack.Length; g++)
            {
                if (g == (int)ProfileGroup.Wait && !_showWaits) continue;
                var h = (float)(stack[g] * Profiler.TicksToMs * scale);
                if (h <= 0) continue;
                var yTop = Math.Max(p0.Y, y - h);
                dl.AddRectFilled(new Vector2(x0, yTop), new Vector2(x1, y), Colour((ProfileGroup)g));
                y = yTop;
            }

            var work = f.WorkMs;
            if (work > SpikeThreshold)
                dl.AddRectFilled(new Vector2(x0, p0.Y), new Vector2(x1, p0.Y + 3), ImGui.GetColorU32(new Vector4(1, 0.2f, 0.2f, 1)));
            if (f.Index == _selected)
                dl.AddRect(new Vector2(x0 - 1, p0.Y), new Vector2(x1 + 1, p0.Y + height), ImGui.GetColorU32(new Vector4(1, 1, 1, 1)));

            if (mouse.X >= x0 - 0.5f && mouse.X < x1 + 0.5f && mouse.Y >= p0.Y && mouse.Y < p0.Y + height) hovered = i;
        }

        if (target != _targetMs)
        {
            _targetMs = target;
            _targetText = target > 0 ? $"{target:0.0} ms  {1000.0 / target:0.#} fps" : "";
        }
        Line(target, _targetText);
        if (Math.Abs(target - 1000.0 / 60) > 1) Line(1000.0 / 60, "16.7 ms  60 fps");

        ImGui.InvisibleButton(_graphId, new Vector2(width, height));
        if (hovered >= 0 && ImGui.IsItemHovered())
        {
            var f = Profiler.GetFrame(hovered);
            ImGui.BeginTooltip();
            ImGui.Text($"frame {f.Index}: {f.Ms:0.00} ms  (work {f.WorkMs:0.00}, swap {f.GpuMs:0.00}, wait {f.WaitMs:0.00})");
            if (f.GcPauseMs > 0) ImGui.Text($"GC pause {f.GcPauseMs:0.00} ms");
            if (f.JitMs > 0.05) ImGui.Text($"JIT {f.JitMs:0.00} ms, {f.JitMethods} methods");
            ImGui.TextUnformatted(GpuOf(f.Index).TrimStart(',', ' '));
            foreach (var s in f.Span.ToArray().OrderByDescending(s => s.Self).Take(10))
            {
                if (!_showWaits && Profiler.Group(s.Id) == ProfileGroup.Wait) continue;
                ImGui.TextColored(GroupTint(Profiler.Group(s.Id)), $"{s.SelfMs,7:0.00}  {Profiler.DisplayName(s.Id)}");
            }
            ImGui.TextDisabled("click to read this frame in the table");
            ImGui.EndTooltip();
            if (ImGui.IsItemClicked()) _selected = f.Index;
        }

        DrawGpuStrip(width, barW, bars, scale, target);

        // The legend doubles as the key to the table's group column.
        for (var g = 0; g < GroupNames.Length; g++)
        {
            if (g > 0) ImGui.SameLine();
            ImGui.TextColored(GroupTint((ProfileGroup)g), Legend[g]);
        }
        ImGui.SameLine();
        var spike = Math.Round(SpikeThreshold, 1);
        if (Math.Round(top, 1) != _scaleTop || spike != _scaleSpike)
        {
            (_scaleTop, _scaleSpike) = (Math.Round(top, 1), spike);
            _scaleText = $"   scale {_scaleTop:0.0} ms, newest on the right, red tick = spike (> {spike:0.0} ms work)";
        }
        ImGui.TextDisabled(_scaleText);
        return;

        void Line(double ms, string text)
        {
            if (ms <= 0 || ms > top) return;
            var y = p0.Y + height - (float)(ms * scale);
            var c = ImGui.GetColorU32(new Vector4(1, 1, 1, 0.35f));
            dl.AddLine(new Vector2(p0.X, y), new Vector2(p0.X + width, y), c);
            dl.AddText(new Vector2(p0.X + 4, y - ImGui.GetTextLineHeight()), c, text);
        }
    }

    // ---- the GPU strip ---------------------------------------------------------------------

    static readonly Vector4[] PassColours =
    [
        new(0.35f, 0.78f, 0.55f, 1f),   // scene
        new(0.25f, 0.85f, 0.90f, 1f),   // capture
        new(0.70f, 0.70f, 0.40f, 1f),   // ao
        new(0.55f, 0.55f, 0.95f, 1f),   // reflections
        new(0.95f, 0.55f, 0.80f, 1f),   // composite
        new(0.90f, 0.60f, 0.30f, 1f),   // world
    ];

    /// <summary>The GPU time of the same frames, at the CPU graph's milliseconds per
    /// pixel so the two can be read against each other; a bar the strip cannot hold
    /// is cut, with a tick. A frame still waiting on the GPU is left empty.</summary>
    void DrawGpuStrip(float width, float barW, int bars, double scale, double target)
    {
        const float height = 64f;
        var p0 = ImGui.GetCursorScreenPos();
        var dl = ImGui.GetWindowDrawList();
        dl.AddRectFilled(p0, p0 + new Vector2(width, height), ImGui.GetColorU32(ImGuiCol.FrameBg));

        var mouse = ImGui.GetMousePos();
        var hovered = -1;
        Span<double> ms = stackalloc double[GpuTimes.Passes];
        for (var i = 0; i < bars; i++)
        {
            var f = Profiler.GetFrame(i);
            var x1 = p0.X + width - i * barW;
            var x0 = x1 - barW + 1f;
            if (mouse.X >= x0 - 0.5f && mouse.X < x1 + 0.5f && mouse.Y >= p0.Y && mouse.Y < p0.Y + height) hovered = i;
            if (!GpuFrames.TryGet(f.Index, ms)) continue;

            var y = p0.Y + height;
            for (var k = 0; k < ms.Length; k++)
            {
                var h = (float)(ms[k] * scale);
                if (h <= 0) continue;
                var yTop = Math.Max(p0.Y, y - h);
                dl.AddRectFilled(new Vector2(x0, yTop), new Vector2(x1, y), ImGui.GetColorU32(PassColours[k]));
                y = yTop;
            }
            if (GpuFrames.Total(ms) * scale > height)
                dl.AddRectFilled(new Vector2(x0, p0.Y), new Vector2(x1, p0.Y + 3), ImGui.GetColorU32(new Vector4(1, 1, 1, 1)));
            if (f.Index == _selected)
                dl.AddRect(new Vector2(x0 - 1, p0.Y), new Vector2(x1 + 1, p0.Y + height), ImGui.GetColorU32(new Vector4(1, 1, 1, 1)));
        }

        if (target > 0 && target * scale <= height)
        {
            var y = p0.Y + height - (float)(target * scale);
            dl.AddLine(new Vector2(p0.X, y), new Vector2(p0.X + width, y), ImGui.GetColorU32(new Vector4(1, 1, 1, 0.35f)));
        }
        dl.AddText(new Vector2(p0.X + 4, p0.Y + 2), ImGui.GetColorU32(new Vector4(1, 1, 1, 0.5f)), "GPU");

        ImGui.InvisibleButton(_gpuId, new Vector2(width, height));
        if (hovered >= 0 && ImGui.IsItemHovered())
        {
            var f = Profiler.GetFrame(hovered);
            ImGui.BeginTooltip();
            if (!GpuFrames.TryGet(f.Index, ms)) ImGui.Text($"frame {f.Index}: GPU not resolved yet");
            else
            {
                ImGui.Text($"frame {f.Index}: GPU {GpuFrames.Total(ms):0.00} ms, CPU work {f.WorkMs:0.00} ms");
                for (var k = 0; k < ms.Length; k++)
                    if (ms[k] > 0) ImGui.TextColored(PassColours[k], $"{ms[k],7:0.00}  {GpuTimes.Names[k]}");
            }
            ImGui.TextDisabled("click to read this frame in the table");
            ImGui.EndTooltip();
            if (ImGui.IsItemClicked()) _selected = f.Index;
        }

        for (var k = 0; k < GpuTimes.Passes; k++)
        {
            if (k > 0) ImGui.SameLine();
            ImGui.TextColored(PassColours[k], _gpuLegend[k] ?? GpuTimes.Names[k]);
        }
        ImGui.SameLine();
        ImGui.TextDisabled("   GPU ms a frame by pass, the same scale as the bars above");
    }

    static Vector4 GroupTint(ProfileGroup g)
    {
        var c = ImGui.ColorConvertU32ToFloat4(Colour(g));
        c.W = 1f;
        return c;
    }

    // ---- spikes -------------------------------------------------------------------------

    int _spikeCount;
    bool _spikesOpen;
    readonly List<(long Index, string Label)> _spikes = new();

    // The labels are only built while the list is open; opening it re-aggregates.
    void BuildSpikes()
    {
        var threshold = SpikeThreshold;
        _spikeCount = 0;
        _spikes.Clear();
        for (var i = 0; i < Profiler.HistoryCount; i++)
        {
            var f = Profiler.GetFrame(i);
            if (f.WorkMs <= threshold) continue;
            _spikeCount++;
            if (!_spikesOpen || _spikes.Count >= 64) continue;
            var extra = (f.GcPauseMs > 0 ? $" GC {f.GcPauseMs:0.0}" : "") + (f.JitMs > 0.05 ? $" JIT {f.JitMs:0.0}" : "");
            _spikes.Add((f.Index,
                $"frame {f.Index,7}  {f.WorkMs,6:0.00} ms work{extra}   {FrameProfiler.Top(f.Span, 3)}##s{f.Index}"));
        }
    }

    void DrawSpikes()
    {
        var open = ImGui.CollapsingHeader($"Spikes ({_spikeCount} in the history){_spikesId}");
        if (open != _spikesOpen)
        {
            _spikesOpen = open;
            if (open) _aggDirty = true;
        }
        if (!open) return;

        ImGui.BeginChild(_spikeListId, new Vector2(0, Math.Min(160, 22 + _spikeCount * ImGui.GetTextLineHeightWithSpacing())));
        foreach (var (index, label) in _spikes)
            if (ImGui.Selectable(label, index == _selected))
                _selected = index;
        if (_spikeCount == 0) ImGui.TextDisabled("None over the threshold.");
        ImGui.EndChild();
    }

    // ---- the table ------------------------------------------------------------------------

    // The rows sort in BuildRows, where the GPU passes join the sections.
    void SortRows() { }

    /// <summary>A table row: a CPU section, or a GPU pass (<see cref="Gpu"/>, whose
    /// <see cref="Id"/> is the pass). The numbers sort; the strings are drawn.</summary>
    readonly record struct Row(int Id, bool Gpu, ProfileGroup Group, string Name, double SelfV, double MaxV,
                               double InclV, double CallsV, string Self, string Max, string Incl,
                               string Calls, string Share);

    readonly List<Row> _visible = new();
    string _tableCaption = "";

    // 0 both, 1 the CPU's sections, 2 the GPU's passes.
    int _side;
    static readonly string[] Sides = ["CPU + GPU", "CPU", "GPU"];

    static string Ms(double v) => v.ToString("0.000", CultureInfo.InvariantCulture);

    void BuildRows()
    {
        var frames = _selected >= 0 ? 1.0 : Math.Max(1, _aggFrames);
        var workTicks = 0L;
        foreach (var id in _rows)
            if (Profiler.Group(id) is not (ProfileGroup.Wait or ProfileGroup.Gpu)) workTicks += _aggSelf[id];

        _visible.Clear();
        if (_side != 2)
            foreach (var id in _rows)
            {
                var group = Profiler.Group(id);
                if (!_showWaits && group == ProfileGroup.Wait) continue;
                var name = Profiler.DisplayName(id);
                if (_filter.Length > 0 && name.IndexOf(_filter, StringComparison.OrdinalIgnoreCase) < 0) continue;

                double self = _aggSelf[id] * Profiler.TicksToMs / frames, max = _aggMax[id] * Profiler.TicksToMs,
                       incl = _aggIncl[id] * Profiler.TicksToMs / frames, calls = _aggCalls[id] / frames;
                _visible.Add(new Row(id, false, group, name, self, max, incl, calls, Ms(self), Ms(max), Ms(incl),
                    calls.ToString(frames == 1 ? "0" : "0.0", CultureInfo.InvariantCulture),
                    group is not (ProfileGroup.Wait or ProfileGroup.Gpu) && workTicks > 0
                        ? (100.0 * _aggSelf[id] / workTicks).ToString("0.0", CultureInfo.InvariantCulture) + "%"
                        : ""));
            }

        // The GPU's passes: the window's average and worst, or the selected frame's.
        var gpuNote = "";
        if (_side != 1)
        {
            Span<double> avg = stackalloc double[GpuTimes.Passes], max = stackalloc double[GpuTimes.Passes];
            bool have;
            if (_selected >= 0)
            {
                have = GpuFrames.TryGet(_selected, _gpuTmp);
                _gpuTmp.CopyTo(avg);
                _gpuTmp.CopyTo(max);
            }
            else
            {
                have = _gpuFrames > 0;
                for (var k = 0; k < GpuTimes.Passes; k++)
                {
                    avg[k] = _gpuSum[k] / Math.Max(1, _gpuFrames);
                    max[k] = _gpuPassMax[k];
                }
            }
            if (!have) gpuNote = _selected >= 0 ? ", GPU not resolved yet" : ", no GPU time yet";
            var total = 0.0;
            foreach (var a in avg) total += a;
            for (var k = 0; have && k < GpuTimes.Passes; k++)
            {
                var name = "GPU " + GpuTimes.Names[k];
                if (_filter.Length > 0 && name.IndexOf(_filter, StringComparison.OrdinalIgnoreCase) < 0) continue;
                if (avg[k] <= 0 && max[k] <= 0) continue;
                _visible.Add(new Row(k, true, ProfileGroup.Gpu, name, avg[k], max[k], avg[k], 0, Ms(avg[k]), Ms(max[k]),
                    Ms(avg[k]), "",
                    total > 0 ? (100.0 * avg[k] / total).ToString("0.0", CultureInfo.InvariantCulture) + "%" : ""));
            }
        }

        _visible.Sort((a, b) =>
        {
            int c = _sortColumn switch
            {
                0 => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase),
                1 => (a.Gpu ? 99 : (int)a.Group).CompareTo(b.Gpu ? 99 : (int)b.Group),
                2 => a.SelfV.CompareTo(b.SelfV),
                3 => a.MaxV.CompareTo(b.MaxV),
                4 => a.InclV.CompareTo(b.InclV),
                5 => a.CallsV.CompareTo(b.CallsV),
                _ => 0,
            };
            if (c == 0) c = a.SelfV.CompareTo(b.SelfV);
            return _sortAscending ? c : -c;
        });

        _tableCaption = (_selected >= 0 ? "one frame" : $"per frame, averaged over {_aggFrames} frames") + gpuNote;
    }

    unsafe void DrawTable()
    {
        ImGui.SetNextItemWidth(220);
        if (ImGui.InputTextWithHint(_filterId, "filter sections", ref _filter, 128)) BuildRows();
        ImGui.SameLine();
        ImGui.SetNextItemWidth(120);
        if (ImGui.Combo(_sideId, ref _side, Sides, Sides.Length)) BuildRows();
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("CPU: the game thread's sections; % is the share of the frame's work. GPU: the GPU's " +
                             "passes, from timer queries; % is the share of the frame's GPU time. \"swap\" is the " +
                             "CPU waiting on the driver, so it is a CPU row.");
        ImGui.SameLine();
        ImGui.TextDisabled(_tableCaption);

        const ImGuiTableFlags flags = ImGuiTableFlags.Sortable | ImGuiTableFlags.RowBg | ImGuiTableFlags.Borders |
                                      ImGuiTableFlags.ScrollY | ImGuiTableFlags.Resizable |
                                      ImGuiTableFlags.SizingStretchProp;
        var height = Math.Max(160, ImGui.GetContentRegionAvail().Y - 64);
        if (!ImGui.BeginTable(_tableId, 7, flags, new Vector2(0, height))) return;

        ImGui.TableSetupScrollFreeze(0, 1);
        ImGui.TableSetupColumn("section", ImGuiTableColumnFlags.WidthStretch, 4f);
        ImGui.TableSetupColumn("group", ImGuiTableColumnFlags.WidthStretch, 0.8f);
        ImGui.TableSetupColumn("self ms", ImGuiTableColumnFlags.DefaultSort | ImGuiTableColumnFlags.PreferSortDescending, 1f);
        ImGui.TableSetupColumn("max self", ImGuiTableColumnFlags.PreferSortDescending, 1f);
        ImGui.TableSetupColumn("incl ms", ImGuiTableColumnFlags.PreferSortDescending, 1f);
        ImGui.TableSetupColumn("calls", ImGuiTableColumnFlags.PreferSortDescending, 0.9f);
        ImGui.TableSetupColumn(_side == 2 ? "% GPU" : _side == 1 ? "% work" : "% work/GPU", ImGuiTableColumnFlags.NoSort, 0.9f);
        ImGui.TableHeadersRow();

        var specs = ImGui.TableGetSortSpecs();
        if (specs.NativePtr != null && specs.SpecsDirty && specs.SpecsCount > 0)
        {
            _sortColumn = specs.Specs.ColumnIndex;
            _sortAscending = specs.Specs.SortDirection == ImGuiSortDirection.Ascending;
            specs.SpecsDirty = false;
            SortRows();
            BuildRows();
        }

        var clipper = new ImGuiListClipperPtr(ImGuiNative.ImGuiListClipper_ImGuiListClipper());
        clipper.Begin(_visible.Count);
        while (clipper.Step())
            for (var i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
            {
                var row = _visible[i];
                ImGui.TableNextRow();
                ImGui.TableNextColumn();
                ImGui.TextUnformatted(row.Name);
                if (!row.Gpu && ImGui.IsItemHovered() && Profiler.Label(row.Id) != null) ImGui.SetTooltip(Profiler.Name(row.Id));
                ImGui.TableNextColumn();
                if (row.Gpu) ImGui.TextColored(PassColours[row.Id], "gpu");
                else ImGui.TextColored(GroupTint(row.Group), GroupNames[(int)row.Group]);
                ImGui.TableNextColumn();
                ImGui.TextUnformatted(row.Self);
                ImGui.TableNextColumn();
                ImGui.TextUnformatted(row.Max);
                ImGui.TableNextColumn();
                ImGui.TextUnformatted(row.Incl);
                ImGui.TableNextColumn();
                ImGui.TextUnformatted(row.Calls);
                ImGui.TableNextColumn();
                ImGui.TextUnformatted(row.Share);
            }
        clipper.End();
        clipper.Destroy();

        ImGui.EndTable();
    }

    // ---- probes ---------------------------------------------------------------------------

    void DrawProbes()
    {
        if (ImGui.Button($"Time the {_stages} stages"))
            _probeResult = string.Join("; ", FrameProfiler.AddProbes("stages"));
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("Put an empty pre-hook on each main-loop stage that is not already hooked, so every " +
                             "stage is a row. A detour each, for the rest of the session.");
        ImGui.SameLine();
        ImGui.SetNextItemWidth(160);
        ImGui.InputTextWithHint(_probeId, _defaultProbe, ref _probeInput, 64);
        ImGui.SameLine();
        if (ImGui.Button("Time function"))
            _probeResult = string.Join("; ", FrameProfiler.AddProbes(_probeInput));
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("[overlay:]address of a recompiled function start. Its body becomes a section, and " +
                             "whatever it calls stops counting as 'game code (no section)'.");
        if (_probeResult.Length > 0) ImGui.TextWrapped(_probeResult);
    }

    string SaveCsv()
    {
        var path = $"profile-{DateTime.Now:yyyyMMdd-HHmmss}.csv";
        try
        {
            using var w = new StreamWriter(path, false, new UTF8Encoding(false), 1 << 16);
            w.WriteLine(FrameProfiler.CsvHeader);
            for (var i = Profiler.HistoryCount - 1; i >= 0; i--) FrameProfiler.WriteCsv(w, Profiler.GetFrame(i));

            return $"wrote {Profiler.HistoryCount} frames to {Path.GetFullPath(path)}";
        }
        catch (Exception e)
        {
            return $"could not write {path}: {e.Message}";
        }
    }
}
