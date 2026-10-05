namespace RecompOne.Runtime;

// A feedback delay network standing in for the SPU's comb/allpass reverb, sized from the game's own reverb registers.
internal sealed class SpuReverb
{
    private const int Fs = 44100;
    private const int Lines = 8;
    private const int SettleSamples = 2048;
    private const int FadeSamples = 1102;
    private const int ModDepth = 12;
    private const int PreSize = 8192;
    private const float Diffusion = 0.6f;
    // Matches the hardware network's wet RMS on the same room, measured with KF2_AUDIO_DUMP.
    private const float OutputGain = 0.48f;

    private static readonly double[] Spread = { 0.4471, 0.5143, 0.5821, 0.6563, 0.7331, 0.8127, 0.8941, 1.0 };
    private static readonly int[] DiffuserL = { 142, 107, 379, 277 };
    private static readonly int[] DiffuserR = { 165, 130, 402, 300 };

    private sealed record Params(bool Discrete, int[] Lengths, float[] Gains, float Damp, int PreDelay);

    private readonly ushort[] _reg;
    private int _seenVersion = -1, _settled;
    private Params? _active, _pending;
    private float _fade = 1f;

    private readonly float[][] _line = new float[Lines][];
    private readonly int[] _linePos = new int[Lines];
    private readonly float[] _lp = new float[Lines];
    private float _lfoA, _lfoB;

    private readonly float[][] _apL = DiffuserL.Select(n => new float[n]).ToArray();
    private readonly float[][] _apR = DiffuserR.Select(n => new float[n]).ToArray();
    private readonly int[] _apPosL = new int[4], _apPosR = new int[4];

    private readonly float[] _preL = new float[PreSize], _preR = new float[PreSize];
    private int _prePos;

    public SpuReverb(ushort[] reverbRegisters)
    {
        _reg = reverbRegisters;
    }

    public static string Identify(ushort[] r)
    {
        return (r[Spu.dAPF1], r[Spu.dAPF2]) switch
        {
            (0, 0) => "off",
            (0x7D, 0x5B) => "room",
            (0x33, 0x25) => "studio small",
            (0xB1, 0x7F) => "studio medium",
            (0xE3, 0xA9) => "studio large",
            (0x1A5, 0x139) => "hall",
            (0x33D, 0x231) => "space echo",
            (0x17, 0x13) => "pipe",
            (1, 1) => r[Spu.vWALL] != 0 ? "echo" : "delay",
            var (a, b) => $"custom {a:X}/{b:X}"
        };
    }

    // False until a register block has held still long enough to size from, and for the discrete echo presets.
    public bool Handles(int version)
    {
        if (version != _seenVersion)
        {
            _seenVersion = version;
            _settled = 0;
        }

        if (_settled < SettleSamples && ++_settled == SettleSamples)
        {
            var next = Derive(_reg);
            if (_active is not { Discrete: false } || next is not { Discrete: false })
            {
                _active = next;
                _pending = null;
            }
            else if (_active.Lengths.AsSpan().SequenceEqual(next.Lengths))
            {
                _active = next;
            }
            else
            {
                _pending = next;
            }
        }

        return _active is { Discrete: false };
    }

    public void Reset()
    {
        if (_active == null) return;
        for (var i = 0; i < Lines; i++)
        {
            _line[i] = new float[_active.Lengths[i] + ModDepth + 2];
            _linePos[i] = 0;
            _lp[i] = 0;
        }

        foreach (var a in _apL) Array.Clear(a);
        foreach (var a in _apR) Array.Clear(a);
        Array.Clear(_preL);
        Array.Clear(_preR);
        _fade = 1f;
    }

    public void Process(int inL, int inR, out int outL, out int outR)
    {
        if (_pending != null)
        {
            _fade -= 1f / FadeSamples;
            if (_fade <= 0f)
            {
                _active = _pending;
                _pending = null;
                Reset();
                _fade = 0f;
            }
        }
        else if (_fade < 1f)
        {
            _fade = MathF.Min(1f, _fade + 1f / FadeSamples);
        }

        var p = _active!;

        _preL[_prePos] = inL / 32768f;
        _preR[_prePos] = inR / 32768f;
        var rp = (_prePos - p.PreDelay) & (PreSize - 1);
        var xl = _preL[rp];
        var xr = _preR[rp];
        _prePos = (_prePos + 1) & (PreSize - 1);

        for (var k = 0; k < 4; k++)
        {
            xl = Allpass(_apL[k], ref _apPosL[k], xl);
            xr = Allpass(_apR[k], ref _apPosR[k], xr);
        }

        _lfoA += 2f * MathF.PI * 0.31f / Fs;
        _lfoB += 2f * MathF.PI * 0.47f / Fs;
        if (_lfoA > 2f * MathF.PI) _lfoA -= 2f * MathF.PI;
        if (_lfoB > 2f * MathF.PI) _lfoB -= 2f * MathF.PI;

        float sum = 0;
        for (var i = 0; i < Lines; i++)
        {
            var delay = (float)p.Lengths[i];
            if (i == 0) delay += ModDepth * MathF.Sin(_lfoA);
            else if (i == 5) delay += ModDepth * MathF.Sin(_lfoB);

            var lp = _lp[i] + (1f - p.Damp) * (Read(i, delay) - _lp[i]);
            if (MathF.Abs(lp) < 1e-12f) lp = 0f;
            _lp[i] = lp;
            sum += lp;
        }

        sum *= 2f / Lines;
        for (var i = 0; i < Lines; i++)
        {
            var buf = _line[i];
            buf[_linePos[i]] = p.Gains[i] * (_lp[i] - sum) + ((i & 1) == 0 ? xl : xr);
            if (++_linePos[i] == buf.Length) _linePos[i] = 0;
        }

        var g = OutputGain * _fade * 32768f;
        outL = (int)Math.Clamp((_lp[0] - _lp[2] + _lp[4] - _lp[6]) * g, -32768f, 32767f);
        outR = (int)Math.Clamp((_lp[1] - _lp[3] + _lp[5] - _lp[7]) * g, -32768f, 32767f);
    }

    private float Read(int i, float delay)
    {
        var buf = _line[i];
        var at = _linePos[i] - delay;
        if (at < 0) at += buf.Length;
        var i0 = (int)at;
        var i1 = i0 + 1 == buf.Length ? 0 : i0 + 1;
        return buf[i0] + (buf[i1] - buf[i0]) * (at - i0);
    }

    private static float Allpass(float[] buf, ref int pos, float x)
    {
        var delayed = buf[pos];
        var w = x + Diffusion * delayed;
        buf[pos] = w;
        if (++pos == buf.Length) pos = 0;
        return delayed - Diffusion * w;
    }

    // Offsets are in words of 22.05 kHz reverb buffer, times four: one word is 8 samples at 44.1 kHz.
    private static Params? Derive(ushort[] r)
    {
        if (r[Spu.dAPF1] <= 1 && r[Spu.dAPF2] <= 1)
            return r[Spu.dAPF1] == 0 ? null : new Params(true, [], [], 0, 0);

        var loops = new List<int>();
        if (r[Spu.mLSAME] > r[Spu.dLSAME]) loops.Add(r[Spu.mLSAME] - r[Spu.dLSAME]);
        if (r[Spu.mLDIFF] > r[Spu.dRDIFF]) loops.Add(r[Spu.mLDIFF] - r[Spu.dRDIFF]);
        if (loops.Count == 0) return null;

        var loopSeconds = loops.Average() * 8 / Fs;

        // The loop feeds back through vWALL once per pass; vIIR is a one-pole low-pass in the same loop.
        var wall = Math.Abs((short)r[Spu.vWALL]) / 32768.0;
        var rt60 = wall < 0.01 ? 0.3 : Math.Clamp(loopSeconds * 3 / -Math.Log10(Math.Min(wall, 0.999)), 0.3, 6.0);
        var a = Math.Clamp((short)r[Spu.vIIR] / 32768.0, 0.01, 0.99);
        var cutoff = Math.Clamp(-Math.Log(1 - a) * 22050 / (2 * Math.PI), 1000, 16000);

        var size = Math.Clamp(loopSeconds, 0.03, 0.25);
        var lengths = Spread.Select(s => Math.Max(64, (int)Math.Round(size * s * Fs))).ToArray();
        var gains = lengths.Select(n => (float)Math.Pow(10, -3.0 * n / Fs / rt60)).ToArray();

        var firstEcho = int.MaxValue;
        (int From, int Tap, int Gain)[] combs =
        {
            (Spu.mLSAME, Spu.mLCOMB1, Spu.vCOMB1), (Spu.mLSAME, Spu.mLCOMB2, Spu.vCOMB2),
            (Spu.mLDIFF, Spu.mLCOMB3, Spu.vCOMB3), (Spu.mLDIFF, Spu.mLCOMB4, Spu.vCOMB4)
        };
        foreach (var (from, tap, gain) in combs)
            if (r[gain] != 0 && r[tap] != 0 && r[from] > r[tap])
                firstEcho = Math.Min(firstEcho, (r[from] - r[tap]) * 8);

        var pre = firstEcho == int.MaxValue ? 0 : Math.Clamp(firstEcho - lengths.Min(), 0, PreSize - 1);
        return new Params(false, lengths, gains, (float)Math.Exp(-2 * Math.PI * cutoff / Fs), pre);
    }
}
