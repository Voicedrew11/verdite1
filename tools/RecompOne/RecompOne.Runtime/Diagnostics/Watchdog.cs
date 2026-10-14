using System.Diagnostics;
using System.Runtime.CompilerServices;

namespace RecompOne.Runtime.Diagnostics;

//0107. A hang leaves nothing at all: the window stops, the player closes it, and
//what the game was doing is gone. The watchdog notices the game's frames stop
//(Runtime.PresentFrame counts them) and writes a hang report. A thread cannot
//read another's managed stack, so it asks the game thread for its own: a flag
//that the dispatcher's indirect calls and every access off the RAM fast path
//(hardware registers, which a game spinning on a flag reads) check, and that
//the game thread answers by taking its own stack. A loop that does neither has
//no stack in its report, but still its registers, its last indirect calls and
//its RAM.
public static class Watchdog
{
    /// <summary>Seconds without a frame that count as a hang; 0 turns it off.</summary>
    public static double HangSeconds = 15;

    /// <summary>Set around a wait the port knows is long and frameless.</summary>
    public static volatile bool Paused;

    private static long _frames;
    private static volatile bool _wanted;
    private static volatile string? _stack;
    private static Thread? _thread;

    /// <summary>A frame was presented.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal static void Frame()
    {
        _frames++;
    }

    /// <summary>A place the game thread passes often: answer a stack request.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal static void Probe()
    {
        if (_wanted) Answer();
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Answer()
    {
        _wanted = false;
        _stack = new StackTrace(2, false).ToString();
    }

    internal static void Start()
    {
        if (_thread != null) return;
        _thread = new Thread(Loop) { IsBackground = true, Name = "watchdog", Priority = ThreadPriority.BelowNormal };
        _thread.Start();
    }

    private static void Loop()
    {
        long seen = 0;
        var still = Stopwatch.StartNew();
        var reported = false;
        while (true)
        {
            Thread.Sleep(1000);
            var frames = Volatile.Read(ref _frames);
            if (frames != seen || Paused || Debugger.IsAttached)
            {
                if (reported && frames != seen)
                    Console.WriteLine($"[Runtime] frames again after {still.Elapsed.TotalSeconds:F0}s");
                seen = frames;
                still.Restart();
                reported = false;
                continue;
            }

            if (frames == 0 || reported || HangSeconds <= 0 || still.Elapsed.TotalSeconds < HangSeconds) continue;
            reported = true;

            _stack = null;
            _wanted = true;
            for (var i = 0; i < 20 && _stack == null; i++) Thread.Sleep(50);
            _wanted = false;

            CrashReport.Write("hang", null,
                $"no frame for {still.Elapsed.TotalSeconds:F0}s (the window may only have been held, e.g. dragged)",
                _stack ?? "(the game thread made no indirect call and touched no hardware register in 1s, " +
                "so its stack could not be taken; the registers and RAM below are what is left)");
        }
    }
}
