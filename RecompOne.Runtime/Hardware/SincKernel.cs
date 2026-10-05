namespace RecompOne.Runtime;

public static class SincKernel
{
    public const int Taps = 8;
    public const int Phases = 256;

    private const double Beta = 5.0;

    // Fraction of the source's Nyquist each band passes; a voice stepping faster than its source needs a lower one.
    private static readonly double[] Cutoffs = { 1.0, 0.75, 0.5, 0.25 };

    private static readonly int[][] Bands = Build();

    public static int BandFor(int step)
    {
        return step <= 0x1000 ? 0 : step <= 0x1555 ? 1 : step <= 0x2000 ? 2 : 3;
    }

    // The interpolated point lies between src[start + 3] and src[start + 4].
    public static int Apply(short[] src, int start, int band, int phase)
    {
        var w = Bands[band];
        var row = phase * Taps;
        long acc = 0;
        for (var k = 0; k < Taps; k++) acc += (long)w[row + k] * src[start + k];
        return Clamp16((int)(acc >> 15));
    }

    // Catmull-Rom between src[start + 1] and src[start + 2]; phase is in 1/256ths.
    public static int Cubic(short[] src, int start, int phase)
    {
        long p0 = src[start], p1 = src[start + 1], p2 = src[start + 2], p3 = src[start + 3];
        long t = phase;
        var a = -p0 + 3 * p1 - 3 * p2 + p3;
        var b = 2 * p0 - 5 * p1 + 4 * p2 - p3;
        var c = p2 - p0;
        return Clamp16((int)(p1 + ((a * t * t * t + ((b * t * t) << 8) + ((c * t) << 16)) >> 25)));
    }

    private static int Clamp16(int v)
    {
        return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
    }

    private static int[][] Build()
    {
        var bands = new int[Cutoffs.Length][];
        var w = new double[Taps];
        for (var b = 0; b < Cutoffs.Length; b++)
        {
            var fc = Cutoffs[b];
            var table = bands[b] = new int[Phases * Taps];
            for (var p = 0; p < Phases; p++)
            {
                var t = p / (double)Phases;
                double sum = 0;
                for (var k = 0; k < Taps; k++)
                {
                    var x = k - (Taps / 2 - 1) - t;
                    w[k] = fc * Sinc(fc * x) * Kaiser(x / (Taps / 2.0));
                    sum += w[k];
                }

                var total = 0;
                for (var k = 0; k < Taps; k++)
                {
                    table[p * Taps + k] = (int)Math.Round(w[k] / sum * 32768);
                    total += table[p * Taps + k];
                }

                // Rounding residue goes on the nearer centre tap, so every row passes DC at exactly unity.
                table[p * Taps + (t < 0.5 ? Taps / 2 - 1 : Taps / 2)] += 32768 - total;
            }
        }

        return bands;
    }

    private static double Sinc(double x)
    {
        return x == 0 ? 1 : Math.Sin(Math.PI * x) / (Math.PI * x);
    }

    private static double Kaiser(double u)
    {
        return u is < -1 or > 1 ? 0 : BesselI0(Beta * Math.Sqrt(1 - u * u)) / BesselI0(Beta);
    }

    private static double BesselI0(double x)
    {
        double sum = 1, term = 1;
        for (var k = 1; k < 32; k++)
        {
            var h = x / (2 * k);
            term *= h * h;
            sum += term;
        }

        return sum;
    }
}
