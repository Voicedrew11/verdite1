using System.Globalization;
using RecompOne.Runtime.Diagnostics;

namespace Verdite.Core;

/// <summary>
/// GPU time per profiler frame (runtime <c>0084</c>). A frame keeps the range of
/// presents it issued, and each query's time is added to the frame that issued it
/// when the GPU finishes it, a few frames later; a frame is complete once every one
/// of its presents has resolved. Everything here runs on the thread that presents,
/// which is the game thread. See "GPU time per present" in a port's docs/DEVELOPMENT.md.
/// </summary>
public static class GpuFrames
{
    const int N = Profiler.HistoryFrames, P = GpuTimes.Passes;

    static readonly long[] _index = Filled(N), _first = new long[N], _last = new long[N];
    static readonly long[] _ns = new long[N * P];
    static readonly bool[] _written = new bool[N];
    // What resolved for the frame still being recorded.
    static readonly long[] _open = new long[P];
    static long _issuedAt;

    static long[] Filled(int n)
    {
        var a = new long[n];
        Array.Fill(a, -1L);
        return a;
    }

    static int Slot(long index) => (int)(index % N);

    public static void Install() => GpuTimes.Resolved += OnResolved;

    /// <summary>A frame has ended: the presents it issued are the ones since the last.</summary>
    public static void Close(Profiler.Frame f)
    {
        int s = Slot(f.Index);
        _index[s] = f.Index;
        _first[s] = _issuedAt;
        _last[s] = GpuTimes.Issued - 1;
        _issuedAt = GpuTimes.Issued;
        _written[s] = false;
        for (int i = 0; i < P; i++)
        {
            _ns[s * P + i] = _open[i];
            _open[i] = 0;
        }
    }

    static void OnResolved(long present, GpuTimes.Pass pass, long ns)
    {
        if (present >= _issuedAt)
        {
            _open[(int)pass] += ns;
            return;
        }
        // Resolution runs a few frames behind; look back from the newest.
        for (int i = 0; i < Math.Min(Profiler.HistoryCount, 256); i++)
        {
            var f = Profiler.GetFrame(i);
            int s = Slot(f.Index);
            if (_index[s] != f.Index || present > _last[s]) continue;
            if (present >= _first[s])
            {
                _ns[s * P + (int)pass] += ns;
                return;
            }
        }
    }

    /// <summary>The frame's GPU milliseconds by pass, once all of it has resolved.</summary>
    public static bool TryGet(long index, Span<double> ms)
    {
        int s = Slot(index);
        if (!GpuTimes.Enabled || _index[s] != index || _last[s] > GpuTimes.Complete) return false;
        for (int i = 0; i < P; i++) ms[i] = _ns[s * P + i] / 1e6;
        return true;
    }

    public static double Total(ReadOnlySpan<double> ms)
    {
        double t = 0;
        foreach (var m in ms) t += m;
        return t;
    }

    /// <summary>A frame's GPU rows, in the CSV's pseudo-section form, if it is complete.</summary>
    public static bool WriteRows(StreamWriter w, Profiler.Frame f)
    {
        Span<double> ms = stackalloc double[P];
        if (!TryGet(f.Index, ms)) return false;
        var t = (f.Start * Profiler.TicksToMs).ToString("0.000", CultureInfo.InvariantCulture);
        for (int i = 0; i < P; i++) Row(GpuTimes.Names[i], ms[i]);
        Row("total", Total(ms));
        return true;

        void Row(string name, double v)
            => w.WriteLine($"{f.Index},{t},gpu.{name},Frame,{v.ToString("0.0000", CultureInfo.InvariantCulture)},,");
    }

    /// <summary>For the streaming CSV: the rows of every recent frame that has become
    /// complete since it was written, each under its own frame index.</summary>
    public static void WriteCompleted(StreamWriter w)
    {
        for (int i = 0; i < Math.Min(Profiler.HistoryCount, 256); i++)
        {
            var f = Profiler.GetFrame(i);
            int s = Slot(f.Index);
            if (_index[s] != f.Index || _written[s]) continue;
            if (WriteRows(w, f)) _written[s] = true;
        }
    }
}
