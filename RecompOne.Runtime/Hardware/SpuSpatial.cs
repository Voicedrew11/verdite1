namespace RecompOne.Runtime;

/// <summary>
/// One voice rendered from a position rather than from its volume registers.
///
/// The port decides *which* voices are positional and *where* they are (the game
/// thread knows the listener and the source); this is only the per-sample half, so
/// every target is in the game's own units and the mixer knows nothing about the
/// world. The chain is mono in, then a rear low-pass, a fractional delay per ear
/// (the interaural time difference) and a first-order head-shadow shelf per ear
/// (Brown and Duda's spherical head, 1998), then a broadband gain per ear. A
/// speaker target is the same chain with no delay and flat shelves.
///
/// Targets move once a frame; everything is smoothed per sample over ~15 ms so a
/// turn of the head is not a zipper. Loudness follows the registers: the game's
/// VAB scaling reaches the voice's volume registers, never the port, so the
/// registers' magnitude divided by what the game asked for at key-on is the scale
/// every target is multiplied by.
/// </summary>
public sealed class SpuSpatialVoice
{
    // Head radius 8.75 cm, sound 343 m/s; bilinear transform at 44.1 kHz.
    const float W = 2f * 343f / 0.0875f;
    const float K = 2f * 44100f;
    static readonly float Inv = 1f / (W + K);
    static readonly float A1 = (W - K) * Inv;
    static readonly float WInv = W * Inv;
    static readonly float KInv = K * Inv;

    static readonly float Smooth = 1f - MathF.Exp(-1f / (0.015f * 44100f));
    static readonly float RearLp = 1f - MathF.Exp(-2f * MathF.PI * 2500f / 44100f);

    const int Ring = 64;

    /// <summary>A delay can be at most this many samples; the ring and the
    /// four-tap read need the rest.</summary>
    public const float MaxDelay = Ring / 2 - 3;

    /// <summary>The key-on this voice's targets belong to; the mixer renders the voice
    /// positionally only while it is the voice's current key-on.</summary>
    internal int Serial = -1;

    /// <summary>What the game asked for at key-on, left plus right, 0..254.</summary>
    internal float KeyOnSum;

    internal float TgtL, TgtR, TgtDl, TgtDr, TgtAl = 1, TgtAr = 1, TgtRear;
    float _l, _r, _dl, _dr, _al = 1, _ar = 1, _rear;

    readonly float[] _ring = new float[Ring * 2];
    int _pos;
    float _lp, _xl, _yl, _xr, _yr;

    internal void Snap()
    {
        _l = TgtL;
        _r = TgtR;
        _dl = TgtDl;
        _dr = TgtDr;
        _al = TgtAl;
        _ar = TgtAr;
        _rear = TgtRear;
    }

    internal void ClearHistory()
    {
        Array.Clear(_ring);
        _pos = 0;
        _lp = _xl = _yl = _xr = _yr = 0;
    }

    internal void Render(int amp, short curL, short curR, out int outL, out int outR)
    {
        _l += (TgtL - _l) * Smooth;
        _r += (TgtR - _r) * Smooth;
        _dl += (TgtDl - _dl) * Smooth;
        _dr += (TgtDr - _dr) * Smooth;
        _al += (TgtAl - _al) * Smooth;
        _ar += (TgtAr - _ar) * Smooth;
        _rear += (TgtRear - _rear) * Smooth;

        float x = amp;
        _lp += (x - _lp) * RearLp;
        x -= _rear * (x - _lp);

        _pos = (_pos + 1) & (Ring - 1);
        _ring[_pos] = x;
        _ring[_pos + Ring] = x;

        var el = Tap(_dl);
        var er = Tap(_dr);

        var yl = (WInv + _al * KInv) * el + (WInv - _al * KInv) * _xl - A1 * _yl;
        var yr = (WInv + _ar * KInv) * er + (WInv - _ar * KInv) * _xr - A1 * _yr;
        _xl = el;
        _xr = er;
        _yl = MathF.Abs(yl) < 1e-12f ? 0 : yl;
        _yr = MathF.Abs(yr) < 1e-12f ? 0 : yr;

        var k = KeyOnSum > 0 ? (Math.Abs((int)curL) + Math.Abs((int)curR)) / (KeyOnSum * 32768f) : 0f;
        outL = (int)(yl * _l * k);
        outR = (int)(yr * _r * k);
    }

    // Catmull-Rom between the samples `delay` and `delay + 1` old. One sample of
    // latency is added so the newer neighbour always exists.
    float Tap(float delay)
    {
        delay = Math.Clamp(delay, 0f, MaxDelay) + 1f;
        var i = (int)delay;
        var f = delay - i;
        var b = _pos + Ring - i;
        float ym1 = _ring[b + 1], y0 = _ring[b], y1 = _ring[b - 1], y2 = _ring[b - 2];
        var c1 = 0.5f * (y1 - ym1);
        var c2 = ym1 - 2.5f * y0 + 2f * y1 - 0.5f * y2;
        var c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + y0;
    }
}
