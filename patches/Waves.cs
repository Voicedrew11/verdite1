using System.Diagnostics;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;

namespace Kf2;

/// <summary>
/// Water that moves: a slow swell that lifts and lowers the water's own vertices
/// (<see cref="WaterSwell"/>), and ripples drawn per pixel over it
/// (<c>RecompOne.Runtime.WaterWaves</c>, 0078), which push the texture and shade it
/// by a moving slope, so the one 64x64 image every water tile repeats stops reading
/// as a grid.
///
///     KF2_WAVES=1          on (off by default: the picture has not been judged)
///     KF2_WAVES_PROBE=1    a line every 2 s: rects, halves and vertices moved, batches rippled
///
/// Everything else is a slider under Video ▸ Experimental ▸ *Water waves*, saved.
///
/// Both halves run on one clock, the world's own: it advances with the logic ticks
/// and the phase between them, so it stands still when the world does (the map, the
/// editor) and stays smooth at any frame rate. Speed scales the clock's rate, not
/// its value, so moving the slider does not jump the water.
///
/// Water is what the reflections call water: a semi-transparent face in an averaging
/// blend whose texture lies in a scrolling texture's dest rect. See "Water waves" in
/// docs/RENDERING.md.
/// </summary>
public static class Waves
{
    public const string OnKey = "kf2.waves.on";
    public const string SwellKey = "kf2.waves.swell", SwellSizeKey = "kf2.waves.swellsize";
    public const string RippleKey = "kf2.waves.ripple", RippleSizeKey = "kf2.waves.ripplesize";
    public const string ShadeKey = "kf2.waves.shade", SpeedKey = "kf2.waves.speed";

    public const float DefaultSwell = 96f, DefaultSwellSize = 12000f;
    public const float DefaultRipple = 48f, DefaultRippleSize = 700f, DefaultShade = 0.25f, DefaultSpeed = 1f;

    const uint Slots = 0x80192D58;
    const int SlotCount = 8;
    const uint SlotStride = 0x18;

    static bool? _forced;
    static bool _probe;

    public static bool Enabled { get; private set; }

    /// <summary>The swell's height at its crest, in world units (a tile is 2048, a
    /// height step 128), and the length of its longest wave.</summary>
    public static float Swell = DefaultSwell, SwellSize = DefaultSwellSize;

    /// <summary>How fast both move; 1 as authored.</summary>
    public static float Speed = DefaultSpeed;

    /// <summary>The field's clock, seconds at the authored speed.</summary>
    public static double Time { get; private set; }

    public static void Configure(string? on, string? probe)
    {
        if (!string.IsNullOrWhiteSpace(on)) _forced = on != "0";
        _probe = probe is not (null or "" or "0");
    }

    public static void Install()
    {
        Enabled = _forced ?? false;
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            var view = RecompOne.Runtime.Runtime.View;
            Enabled = _forced ?? view.GetBool(OnKey, false);
            Swell = Math.Clamp(view.GetFloat(SwellKey, DefaultSwell), 0f, 512f);
            SwellSize = Math.Clamp(view.GetFloat(SwellSizeKey, DefaultSwellSize), 2048f, 65536f);
            WaterWaves.Distort = Math.Clamp(view.GetFloat(RippleKey, DefaultRipple), 0f, 200f);
            WaterWaves.Scale = Math.Clamp(view.GetFloat(RippleSizeKey, DefaultRippleSize), 100f, 4096f);
            WaterWaves.Shade = Math.Clamp(view.GetFloat(ShadeKey, DefaultShade), 0f, 1f);
            Speed = Math.Clamp(view.GetFloat(SpeedKey, DefaultSpeed), 0f, 4f);
            Console.WriteLine($"[KF2] water waves: {(Enabled ? $"on, swell {Swell:F0} over {SwellSize:F0}, " +
                                                               $"ripples {WaterWaves.Distort:F0} over {WaterWaves.Scale:F0}, " +
                                                               $"shade {WaterWaves.Shade:F2}, speed {Speed:F2}" : "off")}");
        });
        Event.AddListener<OverlayLoadedEvent>(_ =>
        {
            WaterWaves.RectN = 0;
            WaterSwell.Forget();
        });
    }

    public static void SetEnabled(bool on)
    {
        Enabled = on;
        if (!on) WaterWaves.Enabled = false;
    }

    /// <summary>A slider moved: the backend sends the uniforms again.</summary>
    public static void Changed() => WaterWaves.Generation++;

    // ---- once a walk ------------------------------------------------------------

    static long _lastFrame, _ticks;
    static double _lastWorld = -1.0;

    /// <summary>From the start of <see cref="TileWalk"/>'s sweep, on the frame's own walk:
    /// the water's rects, the clock, and the camera the walk is about to draw with,
    /// which is the one the ripples take a fragment back to the world by.</summary>
    public static void AtWalk(CpuContext c, PSMemory mem)
    {
        WaterWaves.Enabled = Enabled;
        if (!Enabled) return;

        ReadRects(mem);
        Tick();
        var v = RetainedMap.ReadView(mem);
        var r = WaterWaves.R;
        r[0] = v.R00; r[1] = v.R01; r[2] = v.R02;
        r[3] = v.R10; r[4] = v.R11; r[5] = v.R12;
        r[6] = v.R20; r[7] = v.R21; r[8] = v.R22;
        WaterWaves.CamX = (float)v.CamX; WaterWaves.CamY = (float)v.CamY; WaterWaves.CamZ = (float)v.CamZ;
        WaterWaves.Tx = v.Tx; WaterWaves.Ty = v.Ty; WaterWaves.Tz = v.Tz;
        WaterWaves.Time = (float)(Time % 100000.0);
        WaterWaves.Generation++;

        WaterSwell.AtWalk(mem);
        if (_probe) Report();
    }

    /// <summary>The world's own time: ticks taken plus the phase between them. The
    /// field's clock takes its steps scaled by the speed.</summary>
    static void Tick()
    {
        if (FramePacing.FirstWalkOfTick(ref _lastFrame)) _ticks++;
        double world = (_ticks + FramePacing.LogicPhase) / Math.Max(1.0, FramePacing.LogicHz);
        if (_lastWorld >= 0.0 && world > _lastWorld) Time += (world - _lastWorld) * Speed;
        _lastWorld = world;
    }

    /// <summary>The scrolling textures' dest rects, as <see cref="Reflections"/> reads them.</summary>
    static void ReadRects(PSMemory mem)
    {
        int n = 0;
        var rects = WaterWaves.Rects;
        for (int i = 0; i < SlotCount && n < WaterWaves.MaxRects; i++)
        {
            uint rec = Slots + (uint)i * SlotStride;
            if (mem.ReadU8(rec) != 1) continue;
            int w = (short)mem.ReadU16(rec + 0xAu), h = (short)mem.ReadU16(rec + 0xCu);
            if (w <= 0 || h <= 0) continue;
            rects[n * 4] = (short)mem.ReadU16(rec + 6u);
            rects[n * 4 + 1] = (short)mem.ReadU16(rec + 8u);
            rects[n * 4 + 2] = w;
            rects[n * 4 + 3] = h;
            n++;
        }
        WaterWaves.RectN = n;
    }

    /// <summary>Whether a face's texture lies in one of the water's rects: the texel
    /// columns its UVs reach, in VRAM halfwords.</summary>
    public static bool InRect(uint tpage, int u0, int v0, int u1, int v1)
    {
        int mode = (int)(tpage >> 7) & 3;
        int div = mode == 0 ? 4 : mode == 1 ? 2 : 1;
        int x0 = (int)(tpage & 0xF) * 64 + u0 / div, x1 = (int)(tpage & 0xF) * 64 + u1 / div + 1;
        int y0 = (int)((tpage >> 4) & 1) * 256 + v0, y1 = (int)((tpage >> 4) & 1) * 256 + v1 + 1;
        var rects = WaterWaves.Rects;
        for (int i = 0; i < WaterWaves.RectN; i++)
        {
            float rx = rects[i * 4], ry = rects[i * 4 + 1], rw = rects[i * 4 + 2], rh = rects[i * 4 + 3];
            if (x0 >= rx + rw || x1 <= rx || y0 >= ry + rh || y1 <= ry) continue;
            return true;
        }
        return false;
    }

    // ---- the shell ------------------------------------------------------------------

    /// <summary>The <c>waves</c> verb: the state, the switch, or one setting, live and
    /// not saved.</summary>
    public static string Shell(string arg)
    {
        var parts = arg.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        var ci = System.Globalization.CultureInfo.InvariantCulture;
        if (parts.Length == 1 && parts[0] is "on" or "off") SetEnabled(parts[0] == "on");
        else if (parts.Length == 2 && float.TryParse(parts[1], System.Globalization.NumberStyles.Float, ci, out float v))
        {
            switch (parts[0])
            {
                case "swell": Swell = Math.Clamp(v, 0f, 512f); break;
                case "swellsize": SwellSize = Math.Clamp(v, 2048f, 65536f); break;
                case "ripple": WaterWaves.Distort = Math.Clamp(v, 0f, 200f); break;
                case "ripplesize": WaterWaves.Scale = Math.Clamp(v, 100f, 4096f); break;
                case "shade": WaterWaves.Shade = Math.Clamp(v, 0f, 1f); break;
                case "speed": Speed = Math.Clamp(v, 0f, 4f); break;
                default: return $"{{\"ok\":false,\"error\":\"waves: unknown setting '{parts[0]}'\"}}";
            }
            Changed();
        }
        else if (parts.Length != 0) return "{\"ok\":false,\"error\":\"waves: expected on, off, or a setting and a value\"}";
        return string.Create(ci, $"{{\"ok\":true,\"cmd\":\"waves\",\"on\":{(Enabled ? "true" : "false")}," +
                                 $"\"swell\":{Swell},\"swellsize\":{SwellSize},\"ripple\":{WaterWaves.Distort}," +
                                 $"\"ripplesize\":{WaterWaves.Scale},\"shade\":{WaterWaves.Shade},\"speed\":{Speed}," +
                                 $"\"supported\":{(WaterWaves.Supported ? "true" : "false")},\"rects\":{WaterWaves.RectN}}}");
    }

    // ---- the probe ----------------------------------------------------------------

    static readonly Stopwatch _clock = Stopwatch.StartNew();
    static double _reportedAt;
    static long _batchesAt;

    static void Report()
    {
        double now = _clock.Elapsed.TotalSeconds;
        if (now - _reportedAt < 2.0) return;
        double dt = now - _reportedAt;
        _reportedAt = now;
        long batches = WaterWaves.Batches - _batchesAt;
        _batchesAt = WaterWaves.Batches;
        Console.WriteLine($"[KF2] waves: {WaterWaves.RectN} rect(s), clock {Time:F2}s, " +
                          $"ripples {(WaterWaves.Supported ? $"{batches / dt:F0} batches/s" : "NOT supported")}; " +
                          WaterSwell.Report(dt));
    }
}
