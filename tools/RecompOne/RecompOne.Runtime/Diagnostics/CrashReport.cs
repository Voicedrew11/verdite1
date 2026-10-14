using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;

namespace RecompOne.Runtime.Diagnostics;

//0107. What a crash leaves behind. The runtime printed an exception to stderr and
//closed the window, and a player who started the game from a launcher or a
//desktop icon has no stderr: the window vanished and nothing could be sent. A
//report is one text file in `crashes/` -- the exception, the CPU registers, the
//last indirect calls the game made, the overlays loaded, whatever the port adds
//(its version, its area, its switches) and the console's last lines -- with the
//2 MB of game RAM beside it, so the state the crash came out of can be read
//after the fact. Written for a crash (the game stopped), a fault (a hook threw
//and was turned off, HookManager) and a hang (Watchdog).
public static class CrashReport
{
    /// <summary>Where the reports go, relative to the working directory.</summary>
    public static string Folder = "crashes";

    /// <summary>The first line of every report: the port's name and version.</summary>
    public static string Title = "RecompOne";

    /// <summary>Keep the newest this many reports and delete the rest, so a crash
    /// loop cannot fill a disk.</summary>
    public static int Keep = 20;

    /// <summary>Most reports one session writes; past it, the console only.</summary>
    public static int MaxPerSession = 12;

    /// <summary>Write the 2 MB of game RAM beside the report.</summary>
    public static bool DumpRam = true;

    private static readonly List<(string Title, Func<string> Body)> _sections = [];
    private static readonly object _gate = new();
    private static readonly DateTime _started = DateTime.Now;
    private static int _written;
    private static bool _installed;

    [ThreadStatic] private static bool _writing;

    /// <summary>A section of the port's own, called when a report is written.
    /// It runs on whichever thread crashed, after the crash: it should read, not
    /// lock, and it may throw (the section then says so).</summary>
    public static void AddSection(string title, Func<string> body)
    {
        lock (_sections)
        {
            _sections.Add((title, body));
        }
    }

    /// <summary>Report what no one else catches: an exception on any other thread,
    /// and a task's exception nobody observed. Also starts the watchdog.</summary>
    public static void Install()
    {
        if (_installed) return;
        _installed = true;

        AppDomain.CurrentDomain.UnhandledException += (_, a) =>
        {
            var e = a.ExceptionObject as Exception;
            var path = Write("crash", e, $"unhandled on thread '{Thread.CurrentThread.Name ?? "?"}'" +
                                         (a.IsTerminating ? "; the process is ending" : ""));
            if (path != null) Console.Error.WriteLine($"[Runtime] crash report: {Path.GetFullPath(path)}");
        };
        TaskScheduler.UnobservedTaskException += (_, a) =>
        {
            Write("fault", a.Exception, "a background task failed and nothing observed it");
            a.SetObserved();
        };
        Watchdog.Start();
    }

    /// <summary>Write a report; its path, or null if none was written.
    /// <paramref name="kind"/> is "crash", "fault" or "hang". Never throws.</summary>
    public static string? Write(string kind, Exception? e, string? note = null, string? stack = null)
    {
        if (_writing) return null;
        _writing = true;
        try
        {
            lock (_gate)
            {
                if (_written >= MaxPerSession)
                {
                    Console.Error.WriteLine($"[Runtime] {kind} not written: {MaxPerSession} reports this session already");
                    return null;
                }

                _written++;
                Directory.CreateDirectory(Folder);
                var stamp = DateTime.Now.ToString("yyyy-MM-dd_HH-mm-ss");
                var name = Path.Combine(Folder, $"{stamp}-{kind}");
                for (var n = 2; File.Exists(name + ".log"); n++) name = Path.Combine(Folder, $"{stamp}-{kind}-{n}");

                var ram = DumpRam && WriteRam(name + ".ram.bin");
                File.WriteAllText(name + ".log", Compose(kind, e, note, stack, ram ? Path.GetFileName(name + ".ram.bin") : null));
                Prune();
                Console.Error.WriteLine($"[Runtime] {kind} report written: {Path.GetFullPath(name + ".log")}");
                return name + ".log";
            }
        }
        catch (Exception failure)
        {
            try
            {
                Console.Error.WriteLine($"[Runtime] could not write a {kind} report: {failure.Message}");
            }
            catch
            {
            }

            return null;
        }
        finally
        {
            _writing = false;
        }
    }

    private static string Compose(string kind, Exception? e, string? note, string? stack, string? ram)
    {
        var sb = new StringBuilder();
        sb.AppendLine($"{Title} {kind} report");
        sb.AppendLine($"time:    {DateTime.Now:yyyy-MM-dd HH:mm:ss zzz}");
        sb.AppendLine($"uptime:  {DateTime.Now - _started:hh\\:mm\\:ss}");
        sb.AppendLine($"thread:  {Thread.CurrentThread.Name ?? Thread.CurrentThread.ManagedThreadId.ToString()}");
        sb.AppendLine($"os:      {RuntimeInformation.OSDescription} ({RuntimeInformation.OSArchitecture})");
        sb.AppendLine($".NET:    {RuntimeInformation.FrameworkDescription}");
        sb.AppendLine($"memory:  {Environment.WorkingSet / (1024 * 1024)} MB working set, " +
                      $"{GC.GetTotalMemory(false) / (1024 * 1024)} MB managed");
        if (ram != null) sb.AppendLine($"ram:     {ram} (game RAM, 2 MB, from 0x80000000)");
        if (note != null) sb.AppendLine($"note:    {note}");

        Section(sb, "Exception", () => e?.ToString() ?? "(none)");
        if (stack != null) Section(sb, "Game thread", () => stack);

        (string Title, Func<string> Body)[] sections;
        lock (_sections)
        {
            sections = _sections.ToArray();
        }

        foreach (var (title, body) in sections) Section(sb, title, body);

        Section(sb, "CPU", Registers);
        Section(sb, "Last indirect calls, oldest first", RecentCalls);
        Section(sb, "Overlays", Overlays);
        Section(sb, "Hooks turned off this session", Modding.HookManager.DescribeFaults);
        Section(sb, "Console, last lines", ConsoleTail);
        return sb.ToString();
    }

    private static void Section(StringBuilder sb, string title, Func<string> body)
    {
        sb.AppendLine();
        sb.AppendLine($"== {title} ==");
        try
        {
            sb.AppendLine(body());
        }
        catch (Exception e)
        {
            sb.AppendLine($"(this section failed: {e.GetType().Name}: {e.Message})");
        }
    }

    private static string Registers()
    {
        if (Runtime.Cpu is not { } cpu) return "(no CPU yet)";
        var sb = new StringBuilder();
        var n = 0;
        foreach (var f in typeof(CpuContext).GetFields(BindingFlags.Public | BindingFlags.Instance))
        {
            if (f.FieldType != typeof(uint)) continue;
            sb.Append($"{f.Name,-3}={(uint)f.GetValue(cpu)!:X8}  ");
            if (++n % 6 == 0) sb.AppendLine();
        }

        return sb.ToString().TrimEnd();
    }

    //A loop makes the same call over and over: a run is one address and a count.
    private static string RecentCalls()
    {
        var calls = Dispatch.Dispatcher.Recent();
        var parts = new List<string>();
        for (var i = 0; i < calls.Length;)
        {
            var run = 1;
            while (i + run < calls.Length && calls[i + run] == calls[i]) run++;
            parts.Add(run == 1 ? $"{calls[i]:X8}" : $"{calls[i]:X8} x{run}");
            i += run;
        }

        return parts.Count == 0 ? "(none)" : string.Join(' ', parts);
    }

    private static string Overlays()
    {
        var sb = new StringBuilder();
        sb.AppendLine($"active: {string.Join(", ", Dispatch.Dispatcher.ActiveNames)}");
        var events = new List<Dispatch.OverlayEvent>();
        Runtime.OverlayLog.Read(events);
        foreach (var ev in events.TakeLast(16))
            sb.AppendLine($"{ev.TimestampMs / 1000.0,9:F1}s {ev.Kind,-12} {ev.OverlayName}" +
                          (ev.DisplacedBy != null ? $" (by {ev.DisplacedBy})" : ""));
        return sb.ToString().TrimEnd();
    }

    private static string ConsoleTail()
    {
        var lines = new List<string>();
        ConsoleMirror.SnapshotInto(lines);
        return string.Join('\n', lines.TakeLast(300));
    }

    private static bool WriteRam(string path)
    {
        try
        {
            if (Runtime.Mem is not PSMemory m) return false;
            File.WriteAllBytes(path, m.Ram.ToArray());
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static void Prune()
    {
        var logs = new DirectoryInfo(Folder).GetFiles("*.log").OrderByDescending(f => f.LastWriteTimeUtc).ToArray();
        foreach (var old in logs.Skip(Keep))
            try
            {
                old.Delete();
                File.Delete(Path.ChangeExtension(old.FullName, ".ram.bin"));
            }
            catch
            {
            }
    }
}
