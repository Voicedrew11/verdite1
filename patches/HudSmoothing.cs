using RecompOne.Runtime;
using RecompOne.Runtime.Memory;

namespace Kf1;

/// <summary>
/// The HUD's bars carried between ticks, Verdite3's <c>CarryFollowers</c> on this
/// disc. The renderer's gauge block (<c>Renderer.Gauges</c>) builds the four bars'
/// lengths every frame from HP, MP and the two charge gauges, which the stages step
/// once a tick; so under pacing the bars moved 15 times a second. While the view is
/// carried the four values are drawn between their last two tick samples for that
/// block and put back after it, so nothing else reads them.
///
///     KF1_SMOOTH_GAUGES=0    the bars on the tick
///     KF1_HUD_PROBE=1        a line every 2 s: bars carried, the needle's steps and
///                            the compass's vertices that carry a sub-pixel fraction
///
/// The compass needle is not a spring here: the gauge block's neighbour writes it
/// as the negated yaw of the camera the frame is drawn from, so it follows the
/// carried view and needs nothing of its own. The probe measures that. See "The
/// HUD" in docs/SMOOTHING.md.
/// </summary>
public static class HudSmoothing
{
    // Current HP, MP and the weapon and magic charge gauges, 5000 full.
    static readonly uint[] Values = [0x800A0792u, 0x800A0796u, 0x800A0798u, 0x800A079Cu];

    const uint NeedleYaw = 0x80055D86u;
    const uint PacketRecord = 0x80070EB8u;

    public static bool Enabled { get; set; } = true;

    public static bool ProbeOn { get; set; }

    static readonly ushort[] _prev = new ushort[4], _cur = new ushort[4];
    static long _tick = -1, _sampled = -1;
    static bool _drawn;

    static long _frames, _carried;
    static long _needleFrames, _needleMoved, _needleSum, _needleMax, _needleLast = -1;
    static long _vertices, _found, _fractional;
    static double _probeAt = -1.0;

    public static void Configure(string? mode, string? probe)
    {
        Enabled = mode?.Trim() is not ("0" or "off");
        ProbeOn = probe?.Trim() == "1";
    }

    /// <summary>Before the gauge block: the values interpolated between the last two
    /// ticks. The first frame of a tick samples them.</summary>
    public static void BeforeGauges(PSMemory mem)
    {
        _drawn = false;
        if (!Enabled || !ViewSmoothing.Active) return;
        long tick = FramePacing.Ticks;
        bool first = FramePacing.FirstWalkOfTick(ref _tick);
        for (int i = 0; i < Values.Length; i++)
        {
            ushort v = mem.ReadU16(Values[i]);
            if (first)
            {
                _prev[i] = _sampled < 0 || tick - _sampled > 1 ? v : _cur[i];
                _cur[i] = v;
            }
            else if (v != _cur[i])
            {
                // Written off the tick (a poke, a load): no carry from the old value.
                _prev[i] = _cur[i] = v;
            }
        }
        if (first) _sampled = tick;

        double f = FramePacing.TickFraction;
        for (int i = 0; i < Values.Length; i++)
        {
            int a = _prev[i], b = _cur[i];
            if (a == b) continue;
            ushort drawn = (ushort)(a + (int)Math.Round((b - a) * f));
            if (drawn == b) continue;
            mem.WriteU16(Values[i], drawn);
            _drawn = true;
        }
        if (_drawn) _carried++;
    }

    /// <summary>After the gauge block: the tick's values back.</summary>
    public static void AfterGauges(PSMemory mem)
    {
        if (!_drawn) return;
        for (int i = 0; i < Values.Length; i++) mem.WriteU16(Values[i], _cur[i]);
        _drawn = false;
    }

    /// <summary>Before the compass's drawer: the packet cursor, for the probe.</summary>
    public static uint BeforeCompass(PSMemory mem) =>
        ProbeOn ? mem.ReadU32(mem.ReadU32(PacketRecord) + 8u) : 0u;

    /// <summary>After the compass's drawer: the probe's needle step and the packets
    /// it wrote, each vertex looked up in the vertex map.</summary>
    public static void AfterCompass(PSMemory mem, uint from)
    {
        if (!ProbeOn) return;
        _frames++;

        if (mem.ReadU8(0x80055D74u) == 1)
        {
            long yaw = mem.ReadU16(NeedleYaw) & 0xFFF;
            if (_needleLast >= 0)
            {
                long d = Math.Abs(((yaw - _needleLast + 0x800) & 0xFFF) - 0x800);
                _needleFrames++;
                if (d != 0) { _needleMoved++; _needleSum += d; _needleMax = Math.Max(_needleMax, d); }
            }
            _needleLast = yaw;
        }
        else _needleLast = -1;

        uint to = mem.ReadU32(mem.ReadU32(PacketRecord) + 8u);
        for (uint p = from; p < to && to - from < 0x4000u;)
        {
            uint tag = mem.ReadU32(p);
            uint len = tag >> 24;
            if (len == 0) break;
            uint code = mem.ReadU32(p + 4u) >> 24;
            // G3: colour, xy, three times.
            if ((code & 0xFC) == 0x30 && len == 6)
            {
                for (uint k = 0; k < 3; k++)
                {
                    uint at = p + 8u + 8u * k;
                    _vertices++;
                    if (!GteVertexMap.Peek(at & 0x1FFFFFFFu, mem.ReadU32(at), out var a)) continue;
                    _found++;
                    if (a.Fx != 0f || a.Fy != 0f) _fractional++;
                }
            }
            p += 4u * (len + 1u);
        }

        double now = Environment.TickCount64 / 1000.0;
        if (_probeAt < 0) { _probeAt = now + 2.0; return; }
        if (now < _probeAt) return;
        _probeAt = now + 2.0;
        double mean = _needleMoved > 0 ? (double)_needleSum / _needleMoved : 0;
        Console.WriteLine($"[KF1] hud: {_frames} frame(s), bars carried on {_carried}; needle moved on " +
                          $"{_needleMoved}/{_needleFrames}, mean {mean:0.0} max {_needleMax} a frame; compass " +
                          $"vertices {_found}/{_vertices} mapped, {_fractional} with a fraction");
        _frames = _carried = _needleFrames = _needleMoved = _needleSum = _needleMax = 0;
        _vertices = _found = _fractional = 0;
    }
}
