using System.Globalization;
using System.Reflection;
using System.Text;
using RecompOne.Runtime.Config;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Diagnostics;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Host.Window;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;
using Silk.NET.Input;
using HostWindow = RecompOne.Runtime.Host.HostWindow;

namespace Verdite.Core;

/// <summary>
/// Where a frame's time goes: the shared half of the frame profiler
/// (<c>RecompOne.Runtime.Diagnostics.Profiler</c>, tools/RecompOne/patches/0045). The
/// port reads its own env vars (<c>{TAG}_PROFILE</c>, <c>_PROFILE_OUT</c>,
/// <c>_PROFILE_SPIKE</c>, <c>_PROFILE_FUNCS</c>) and hands them to
/// <see cref="Configure"/>, with the table of addresses it names.
///
///     {TAG}_PROFILE=1            record from boot; a summary on the console every 5 s
///     {TAG}_PROFILE=panel        record from boot and open the panel (Shift+P toggles it)
///     {TAG}_PROFILE_OUT=path     every frame's sections as CSV rows, for
///                                scripts/profile_report.py
///     {TAG}_PROFILE_SPIKE=12     a console line for every frame whose *work* passes 12 ms
///     {TAG}_PROFILE_FUNCS=stages time the main-loop stages; or a list,
///                                game:80040348+800342D8 (overlay defaults to game)
///
/// **What it can see without being asked.** Every hooked function is already a
/// section, because the runtime times inside <c>HookManager.Invoke</c>: the
/// function's own body (recompiled MIPS) and every pre, post and replace delegate
/// separately, named after the method that implements it. The runtime adds the
/// present path -- window events, picture compose, the interface, the swap -- and
/// the sleeps are sections of their own (<see cref="ProfileGroup.Wait"/>), so a frame
/// capped at 144 fps reads as work plus waiting rather than as 6.9 ms of something.
/// Whatever no section claims is "game code (no section)".
///
/// **To look inside the game's own time**, time more functions: an empty pre-hook
/// makes any recompiled function a section, which is what <see cref="AddProbe"/>
/// does. That is a detour, so it is opt-in.
///
/// Recording costs nothing when off -- one bool at every site -- and turns itself
/// on while the panel is open, so it is not a setting and saves nothing.
/// </summary>
public static class FrameProfiler
{
    static readonly ModInfo _self = new()
    {
        Id = $"{Game.Id}.profiler",
        Name = "Frame profiler",
        Version = "1.0",
        Description = "Times the frame by section; empty pre-hooks make game functions sections.",
    };

    static bool _envOn;
    static bool _openPanel;
    static bool _console;
    static double _spikeMs;
    static string? _csvPath;
    static string? _funcs;
    static (string Overlay, uint Addr, string Label)[] _known = [];

    /// <summary>Record while the panel is closed.</summary>
    public static bool KeepRecording;
    /// <summary>Hold the history still, for reading.</summary>
    public static bool Paused;

    /// <summary>
    /// The port's env values, and its table of addresses to label (a row whose label
    /// starts "stage " is one of the main-loop stages the "stages" probe times).
    /// </summary>
    public static void Configure(string? on, string? csv, string? spike, string? funcs,
                                 (string Overlay, uint Addr, string Label)[] known)
    {
        _envOn = on is "1" or "panel" or "on";
        _openPanel = on == "panel";
        _console = _envOn;
        _csvPath = string.IsNullOrWhiteSpace(csv) ? null : csv;
        if (double.TryParse(spike, NumberStyles.Float, CultureInfo.InvariantCulture, out var s) && s > 0) _spikeMs = s;
        _funcs = string.IsNullOrWhiteSpace(funcs) ? null : funcs;
        _known = known;
    }

    /// <summary>Recording follows whoever wants it: the environment, the CSV, the panel
    /// being open, or the panel's keep-recording box -- and a pause beats all of them.</summary>
    public static void UpdateEnabled()
    {
        var want = _envOn || _csv != null || _spikeMs > 0 || KeepRecording || ProfilerPanel.Instance.IsOpen;
        Profiler.Enabled = want && !Paused;
        GpuTimes.Enabled = Profiler.Enabled;
    }

    public static void Install()
    {
        Profiler.FrameCompleted += OnFrame;
        GpuFrames.Install();

        if (_csvPath != null)
        {
            try
            {
                _csv = new StreamWriter(_csvPath, false, new UTF8Encoding(false), 1 << 16);
                _csv.WriteLine(CsvHeader);
                AppDomain.CurrentDomain.ProcessExit += (_, _) => _csv?.Flush();
                Console.WriteLine($"[{Game.Tag}] profile: writing every frame to {_csvPath}");
            }
            catch (Exception e)
            {
                Console.Error.WriteLine($"[{Game.Tag}] profile: cannot write {_csvPath}: {e.Message}");
                _csv = null;
            }
        }

        UpdateEnabled();
        if (Profiler.Enabled)
            Console.WriteLine($"[{Game.Tag}] profile: recording" +
                              (_spikeMs > 0 ? $", spikes over {_spikeMs:0.#} ms of work" : ""));

        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            Localization.Merge($$"""
            {
              "strings": {
                "{{Game.Id}}.profiler": { "en": "Frame profiler", "pt-BR": "Perfilador de quadros",
                                  "es-419": "Perfilador de cuadros" }
              }
            }
            """);
            PanelManager.Register(ProfilerPanel.Instance);
            ConfigManager.ApplyViewToPanels([ProfilerPanel.Instance]);
            if (_openPanel) ProfilerPanel.Instance.IsOpen = true;
            UpdateEnabled();
        });

        // Off the event bus for the reason the port's map gives: a polled key would be
        // dead in the menus and through a load, which are two of the places worth
        // profiling. Shift+P, in the shape of the map's Shift+M: every function key is
        // taken -- F1 and F11 by the runtime, F2-F8 by mods, F9, F10 and F12 offered as
        // the mouse-capture key.
        Event.AddListener<KeyboardEvent>(e =>
        {
            if (!e.Pressed || e.Repeat || e.Key != (int)Key.P || PopupManager.AnyOpen || HotkeyGate.Typing) return;
            if (!HostWindow.IsKeyDown(Key.ShiftLeft) && !HostWindow.IsKeyDown(Key.ShiftRight)) return;
            ProfilerPanel.Instance.IsOpen = !ProfilerPanel.Instance.IsOpen;
        });

        HookAttach.OnOverlayLoad("profile", () =>
        {
            ApplyLabels();
            if (_funcs != null)
                foreach (var line in AddProbes(_funcs))
                    Console.WriteLine($"[{Game.Tag}] profile: {line}");
            return true;
        });
    }

    // ---- names for the addresses the port knows --------------------------------

    static bool _labelled;

    static void ApplyLabels()
    {
        if (_labelled) return;
        _labelled = true;
        foreach (var (overlay, addr, label) in _known)
            if (SymbolRegistry.Resolve(overlay, null, addr) is { } target)
                Profiler.SetLabel(Profiler.FunctionName(target), label);
        Profiler.SetLabel($"pre {nameof(FrameProfiler)}.{nameof(Probe)}", "profiler probes (empty)", ProfileGroup.Hook);
    }

    // ---- probes ------------------------------------------------------------------

    /// <summary>The empty pre-hook a probe installs. Its only job is to exist:
    /// HookManager times every hooked function.</summary>
    public static void Probe(CpuContext c, IMemory m)
    {
    }

    /// <summary>Make a recompiled function a section. Must run on the game thread,
    /// which is where the panel draws.</summary>
    public static string AddProbe(string overlay, uint addr)
    {
        var target = SymbolRegistry.Resolve(overlay, null, addr);
        if (target == null) return $"{overlay} 0x{addr:X8}: no function starts there";
        var name = Profiler.FunctionName(target);
        if (HookManager.IsRegistered(target)) return $"{name}: already hooked, so already timed";

        var probe = typeof(FrameProfiler).GetMethod(nameof(Probe), BindingFlags.Public | BindingFlags.Static)!;
        if (!HookManager.AddPre(_self, target, probe)) return $"{name}: HookManager refused the probe";
        HookManager.Commit();
        return HookAttach.Installed(target) ? $"{name}: timing" : $"{name}: queued but not installed";
    }

    /// <summary><c>stages</c>, or <c>[overlay:]hex</c> items separated by + or ,.</summary>
    public static IEnumerable<string> AddProbes(string spec)
    {
        foreach (var raw in spec.Split(['+', ',', ' '], StringSplitOptions.RemoveEmptyEntries))
        {
            if (raw.Equals("stages", StringComparison.OrdinalIgnoreCase))
            {
                foreach (var (overlay, addr, label) in _known)
                    if (label.StartsWith("stage ", StringComparison.Ordinal))
                        yield return AddProbe(overlay, addr);
                continue;
            }

            var colon = raw.IndexOf(':');
            var overlayName = colon >= 0 ? raw[..colon] : "game";
            var hex = (colon >= 0 ? raw[(colon + 1)..] : raw).Replace("0x", "").Replace("func_", "");
            yield return uint.TryParse(hex, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var a)
                ? AddProbe(overlayName.ToLowerInvariant(), a)
                : $"'{raw}': not an address";
        }
    }

    // ---- console and CSV ---------------------------------------------------------

    static StreamWriter? _csv;
    static long _csvFlushAt;

    static readonly long[] _sumSelf = new long[Profiler.MaxSections];
    static readonly List<double> _frameMs = new(4096);
    static long _winStart;
    static double _winWork, _winWait, _winGpu, _winGc, _winJit;
    static long _winAlloc;

    static void OnFrame(Profiler.Frame f)
    {
        GpuFrames.Close(f);
        if (_csv != null)
        {
            WriteCsv(_csv, f, gpu: false);
            GpuFrames.WriteCompleted(_csv);
            if (f.Start >= _csvFlushAt)
            {
                _csv.Flush();
                _csvFlushAt = f.Start + (long)(1000.0 / Profiler.TicksToMs);
            }
        }
        if (_spikeMs > 0 && f.WorkMs > _spikeMs)
            Console.WriteLine($"[{Game.Tag}] profile spike: frame {f.Index}, {f.WorkMs:0.00} ms of work in {f.Ms:0.00} ms" +
                              (f.GcPauseMs > 0 ? $", GC {f.GcPauseMs:0.00} ms" : "") +
                              (f.JitMs > 0.05 ? $", JIT {f.JitMs:0.00} ms ({f.JitMethods} methods)" : "") +
                              $": {Top(f.Span, 6)}");
        if (_console) Accumulate(f);
    }

    // ---- GPU time (runtime 0084) ---------------------------------------------------

    // What resolved since the window began, in ms per present; the queries are a few
    // presents old when they are read.
    static readonly double[] _winGpuMs = new double[GpuTimes.Passes];
    static readonly long[] _winGpuAt = new long[GpuTimes.Passes];
    static long _winGpuPresents, _winGpuPresentsAt;

    static void TakeGpu(double[] ms, ref long presents, long[] at, ref long presentsAt)
    {
        presents = GpuTimes.Presents - presentsAt;
        presentsAt = GpuTimes.Presents;
        for (int i = 0; i < GpuTimes.Passes; i++)
        {
            long ns = GpuTimes.Ns[i] - at[i];
            at[i] = GpuTimes.Ns[i];
            ms[i] = presents > 0 ? ns / 1e6 / presents : 0;
        }
    }

    static string GpuLine()
    {
        TakeGpu(_winGpuMs, ref _winGpuPresents, _winGpuAt, ref _winGpuPresentsAt);
        if (!GpuTimes.Supported) return "";
        if (_winGpuPresents <= 0) return "; GPU: nothing resolved";
        var parts = string.Join(", ", Enumerable.Range(0, GpuTimes.Passes)
            .Where(i => _winGpuMs[i] > 0).Select(i => $"{GpuTimes.Names[i]} {_winGpuMs[i]:0.000}"));
        return $"; GPU ms/present {_winGpuMs.Sum():0.00}: {parts}";
    }

    public const string CsvHeader = "frame,time_ms,section,group,self_ms,incl_ms,calls";

    /// <summary>One frame as CSV rows: a row per section, then the frame-level
    /// measurements as pseudo-sections in the same columns.</summary>
    public static void WriteCsv(StreamWriter w, Profiler.Frame f, bool gpu = true)
    {
        var t = (f.Start * Profiler.TicksToMs).ToString("0.000", CultureInfo.InvariantCulture);
        foreach (var s in f.Span)
        {
            w.Write(f.Index);
            w.Write(',');
            w.Write(t);
            w.Write(',');
            WriteQuoted(w, Profiler.DisplayName(s.Id));
            w.Write(',');
            w.Write(Profiler.Group(s.Id).ToString());
            w.Write(',');
            w.Write(s.SelfMs.ToString("0.0000", CultureInfo.InvariantCulture));
            w.Write(',');
            w.Write(s.InclMs.ToString("0.0000", CultureInfo.InvariantCulture));
            w.Write(',');
            w.WriteLine(s.Calls);
        }

        // Frame-level measurements as pseudo-sections in the same columns, so one
        // file carries everything and a reader needs no second format.
        Row(w, f, t, "frame.total", f.Ms);
        if (f.GcPauseMs > 0) Row(w, f, t, "frame.gc_pause", f.GcPauseMs);
        if (f.AllocBytes > 0) Row(w, f, t, "frame.alloc_kb", f.AllocBytes / 1024.0);
        if (f.JitMs > 0) Row(w, f, t, "frame.jit", f.JitMs);
        // Complete once the GPU has finished the frame; the streaming CSV writes them then.
        if (gpu) GpuFrames.WriteRows(w, f);

        static void Row(StreamWriter w, Profiler.Frame f, string t, string name, double v)
            => w.WriteLine($"{f.Index},{t},{name},Frame,{v.ToString("0.0000", CultureInfo.InvariantCulture)},,");

        static void WriteQuoted(StreamWriter w, string s)
        {
            if (s.IndexOfAny([',', '"']) < 0)
            {
                w.Write(s);
                return;
            }

            w.Write('"');
            w.Write(s.Replace("\"", "\"\""));
            w.Write('"');
        }
    }

    const double ReportSeconds = 5.0;

    static void Accumulate(Profiler.Frame f)
    {
        if (_frameMs.Count == 0) _winStart = f.Start;
        _frameMs.Add(f.Ms);
        foreach (var s in f.Span) _sumSelf[s.Id] += s.Self;
        _winWork += f.WorkMs;
        _winWait += f.WaitMs;
        _winGpu += f.GpuMs;
        _winGc += f.GcPauseMs;
        _winJit += f.JitMs;
        _winAlloc += f.AllocBytes;

        var seconds = (f.Start + f.Ticks - _winStart) * Profiler.TicksToMs / 1000.0;
        if (seconds < ReportSeconds) return;

        var n = _frameMs.Count;
        var avg = _frameMs.Sum() / n;
        _frameMs.Sort();
        var p99 = _frameMs[Math.Min(n - 1, (int)(n * 0.99))];
        var max = _frameMs[n - 1];

        var ids = new List<int>();
        for (var id = 0; id < Profiler.SectionCount; id++)
            if (_sumSelf[id] > 0) ids.Add(id);
        ids.Sort((a, b) => _sumSelf[b].CompareTo(_sumSelf[a]));

        var top = string.Join(", ", ids.Where(id => Profiler.Group(id) != ProfileGroup.Wait).Take(10)
            .Select(id => $"{Profiler.DisplayName(id)} {_sumSelf[id] * Profiler.TicksToMs / n:0.000}"));

        Console.WriteLine($"[{Game.Tag}] profile: {n / seconds:0.0} fps, frame {avg:0.00} ms avg / {p99:0.00} p99 / " +
                          $"{max:0.00} max; work {_winWork / n:0.00}, wait {_winWait / n:0.00}, " +
                          $"swap {_winGpu / n:0.00} ms; GC {_winGc / seconds:0.00} ms/s, JIT {_winJit / seconds:0.00} ms/s, " +
                          $"{_winAlloc / 1024.0 / n:0.0} KB/frame allocated{GpuLine()}; top self ms/frame: {top}");

        _frameMs.Clear();
        Array.Clear(_sumSelf);
        _winWork = _winWait = _winGpu = _winGc = _winJit = 0;
        _winAlloc = 0;
    }

    /// <summary>The <paramref name="n"/> biggest self times in a frame, work first.</summary>
    public static string Top(ReadOnlySpan<Profiler.Sample> samples, int n)
    {
        var list = samples.ToArray()
            .Where(s => Profiler.Group(s.Id) != ProfileGroup.Wait && s.Self > 0)
            .OrderByDescending(s => s.Self)
            .Take(n)
            .Select(s => $"{Profiler.DisplayName(s.Id)} {s.SelfMs:0.00}");
        return string.Join(", ", list);
    }
}
