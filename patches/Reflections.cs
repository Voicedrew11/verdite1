using System.Diagnostics;
using System.Reflection;
using RecompOne.Runtime;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf2;

/// <summary>
/// Screen-space reflections on water.
///
///     KF2_SSR=1              on (off by default: the picture has not been judged)
///     KF2_SSR_STRENGTH=0.6   the most of the scene water reflects, at a grazing angle
///     KF2_SSR_F0=0.12        the share it reflects looking straight down
///     KF2_SSR_DISTANCE=16384 how far a ray is marched, in world units (a tile is 2048);
///                            unset marches to where the game's fog turns black
///     KF2_SSR_STEPS=32       steps along the ray
///     KF2_SSR_THICKNESS=256  how far behind a depth a ray may be and still hit it
///     KF2_SSR_SKY=1          a miss takes the background it crossed at this weight; 0 none
///     KF2_SSR_RESOLUTION=2   the pass at this multiple of the game's pixels; 0 the render scale
///     KF2_SSR_FOGCURVE=2     the depth-cue curve a reflection's longer path is fogged on:
///                            0 none, 1 offset, 2 knee (the near tiles'), 3 half, 4 linear
///     KF2_SSR_PROBE=1        passes, water triangles, and a readback of what each
///                            reflective pixel found
///
/// The work is in the runtime (<c>patches/recompone/0067</c>): a surface buffer beside
/// the occlusion pass's normals, holding the last surface drawn at each pixel with
/// its normal, depth and material, and a pass at present that marches each
/// reflective pixel's reflected ray through the depth buffer. This patch is the
/// switch, the material table and the one fact the runtime cannot know: **which VRAM
/// rectangles hold water.** The rectangles are published whenever the pass runs,
/// which is for any of its terms: this march, <see cref="Murk"/>, <see cref="PlanarWalk"/>
/// or <see cref="RetainedMap"/>, each on its own switch.
///
/// The game keeps its scrolling textures in eight slots at <c>0x80192D58</c>
/// (<see cref="FluidSmoothing"/>), each re-uploaded every tick into a fixed dest
/// rect. They are published here as <see cref="SurfaceMaterial.Water"/>, translucent
/// only: the same slots hold the main-hall fire, which is additive, and the
/// creatures' skins, which are opaque, and the runtime refuses both on the blend.
/// Published every frame whether or not the smoothing is on, because the smoothing
/// publishes nothing at or below the tick rate.
///
/// A material is kept per pixel so that lighting later has one to read: an authored
/// material belongs in <c>GtePacketDepth.Rec.Material</c>, set where the port builds
/// the packet, and wins over the texture rule. See "Screen-space reflections" in
/// docs/RENDERING.md.
/// </summary>
public static class Reflections
{
    const uint Slots = 0x80192D58;
    const int Count = 8;
    const uint Stride = 0x18;
    const uint DrawOTag = 0x80060818;

    public const string OnKey = "kf2.ssr.on";

    static bool? _forced;
    static bool _probe;
    static readonly Stopwatch _clock = Stopwatch.StartNew();
    static double _reportedAt;
    static long _frames;

    static readonly ModInfo _self = new()
    {
        Id = "kf2.ssr",
        Name = "Screen-space reflections",
        Version = "1.0",
        Description = "Reflects the scene in water, from the depth the GTE throws away.",
    };

    public static bool Enabled => ScreenReflections.Enabled;

    /// <summary>Whether anything reflects: the march or either planar source. The
    /// murk alone runs the pass and reflects nothing.</summary>
    public static bool AnySource => ScreenReflections.Enabled || PlanarReflections.Enabled || RetainedScene.Enabled;

    /// <summary>Whether this patch prints the readback's map itself.</summary>
    public static bool Probing => _probe;

    public static void Configure(string? on, string? strength, string? f0, string? distance, string? steps,
                                 string? thickness, string? sky, string? resolution, string? probe,
                                 string? fogCurve = null)
    {
        if (!string.IsNullOrWhiteSpace(on)) _forced = on != "0";

        float s = 0.6f, f = 0.12f;
        if (float.TryParse(strength, out float st) && st >= 0f) s = Math.Clamp(st, 0f, 1f);
        if (float.TryParse(f0, out float ff) && ff >= 0f) f = Math.Clamp(ff, 0f, 1f);
        SurfaceMaterial.Reflectivity[SurfaceMaterial.Water] = s;
        SurfaceMaterial.F0[SurfaceMaterial.Water] = f;
        SurfaceMaterial.Changed();

        if (float.TryParse(distance, out float d) && d > 0f) ScreenReflections.MaxDistance = d;
        if (int.TryParse(steps, out int n) && n > 0) ScreenReflections.Steps = Math.Clamp(n, 1, 128);
        if (float.TryParse(thickness, out float t) && t > 0f) ScreenReflections.Thickness = t;
        if (float.TryParse(sky, out float k) && k >= 0f) ScreenReflections.Sky = Math.Clamp(k, 0f, 1f);
        if (int.TryParse(resolution, out int r) && r >= 0) ScreenReflections.Resolution = r;
        if (int.TryParse(fogCurve, out int fc) && fc is >= 0 and <= 4) ScreenReflections.FogCurve = fc;

        _probe = !string.IsNullOrWhiteSpace(probe) && probe != "0";
        ScreenReflections.Probe = _probe;
    }

    public static void Install()
    {
        ScreenReflections.Enabled = _forced ?? false;

        // The saved key only exists once HostWindow.Initialize has loaded the
        // config; see AmbientOcclusion.Install.
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            ScreenReflections.Enabled = _forced ?? RecompOne.Runtime.Runtime.View.GetBool(OnKey, false);
            Console.WriteLine($"[KF2] reflections: {(Enabled ? "on" : "off")}" +
                              (Enabled ? $", water {SurfaceMaterial.Reflectivity[SurfaceMaterial.Water]:F2} " +
                                         $"(F0 {SurfaceMaterial.F0[SurfaceMaterial.Water]:F2}), " +
                                         $"{ScreenReflections.Steps} steps over " +
                                         (ScreenReflections.MaxDistance > 0 ? $"{ScreenReflections.MaxDistance:F0}, " : "the fog's depth, ") +
                                         (ScreenReflections.Resolution == 0 ? "at the render scale"
                                             : $"at most {ScreenReflections.Resolution}x") : ""));
        });

        Event.AddListener<OverlayLoadedEvent>(_ => SurfaceMaterial.RectN = 0);

        HookAttach.OnOverlayLoad("reflections", Attach);
    }

    public static void SetEnabled(bool on)
    {
        ScreenReflections.Enabled = on;
        if (!GteDepth.Reflections) SurfaceMaterial.RectN = 0;
    }

    static bool Attach()
    {
        SymbolRegistry.Build();
        var target = SymbolRegistry.Resolve("game", null, DrawOTag);
        if (target == null) return false;

        HookManager.AddPre(_self, target,
            typeof(Reflections).GetMethod(nameof(BeforeDrawOTag), BindingFlags.Public | BindingFlags.Static)!);
        HookManager.Commit();
        if (!HookAttach.Installed(target))
        {
            Console.Error.WriteLine("[KF2] reflections: DrawOTag pre did not attach; water will not be found.");
            return false;
        }
        Console.WriteLine($"[KF2] reflections: hooked DrawOTag at 0x{DrawOTag:X8}");
        return true;
    }

    /// <summary>The water's dest rects, before the walk that draws with them.</summary>
    public static void BeforeDrawOTag(CpuContext c, IMemory m)
    {
        if (!GteDepth.Reflections) return;

        int n = 0;
        for (int i = 0; i < Count && n < SurfaceMaterial.RectSlots; i++)
        {
            uint rec = Slots + (uint)i * Stride;
            if (m.ReadU8(rec) != 1) continue;
            int w = (short)m.ReadU16(rec + 0xAu), h = (short)m.ReadU16(rec + 0xCu);
            if (w <= 0 || h <= 0) continue;
            ref var r = ref SurfaceMaterial.Rects[n++];
            r.X = (short)m.ReadU16(rec + 6u);
            r.Y = (short)m.ReadU16(rec + 8u);
            r.W = w;
            r.H = h;
            r.Material = SurfaceMaterial.Water;
            r.TranslucentOnly = true;
        }
        SurfaceMaterial.RectN = n;

        if (_probe) Report();
    }

    /// <summary>SsrFs's fogKeep, for the probe line.</summary>
    static double FogKeep(double z)
    {
        int curve = ScreenReflections.FogCurve;
        if (curve == 0) return 1.0;
        double q = Math.Min(GteDepth.ProjH * 65536.0 / Math.Max(z, 1.0), 131071.0);
        double ir0 = Math.Clamp((GteDepth.ProjDqa * q + GteDepth.ProjDqb) / 4096.0, 0.0, 4096.0);
        double w = curve == 1 ? Math.Max(ir0 - 800.0, 0.0) * 2.0
                 : curve == 2 ? (ir0 < 2800.0 ? ir0 : 3.0 * ir0 - 5600.0)
                 : curve == 3 ? ir0 * 0.5
                 : ir0;
        return Math.Clamp(1.0 - w / 4096.0, 0.0, 1.0);
    }

    static void Report()
    {
        _frames++;
        double now = _clock.Elapsed.TotalSeconds;
        double dt = now - _reportedAt;
        if (dt < 2.0) return;
        _reportedAt = now;

        var refused = SurfaceMaterial.RefusedByBlend;
        Console.WriteLine($"[KF2] reflections: {ScreenReflections.Passes / dt:F1} passes/s, " +
                          $"{ScreenReflections.NoTarget / dt:F1} no target/s over {_frames / dt:F0} walks/s; " +
                          $"{SurfaceMaterial.RectN} water rect(s), " +
                          $"{SurfaceMaterial.ByMaterial[SurfaceMaterial.Water] / dt:F0} water tris/s " +
                          $"of {SurfaceMaterial.Blended / dt:F0} blended with a depth, " +
                          $"refused by blend {refused[0] / dt:F0}/{refused[1] / dt:F0}/{refused[2] / dt:F0}/{refused[3] / dt:F0}; " +
                          $"{AoGeometry.Triangles / dt:F0} surface tris/s, {SurfaceMaterial.Overlays / dt:F0} of them 2D overlay, " +
                          $"{SurfaceMaterial.Veils / dt:F0} 2D veil ({SurfaceMaterial.TexturedVeils / dt:F0} textured)");
        Console.WriteLine($"[KF2] reflections: fog curve {ScreenReflections.FogCurve}, DQA {GteDepth.ProjDqa}, DQB {GteDepth.ProjDqb}, H {GteDepth.ProjH:F0}; " +
                          $"black at {ScreenReflections.FogBlackDepth():F0}, marching {ScreenReflections.March():F0}; " +
                          $"a colour keeps {FogKeep(2048):F2} at one tile, {FogKeep(4096):F2} at two, {FogKeep(8192):F2} at four, {FogKeep(16384):F2} at eight");
        for (int i = 0; i < SurfaceMaterial.RectN; i++)
        {
            ref var r = ref SurfaceMaterial.Rects[i];
            Console.WriteLine($"[KF2] reflections:   water rect ({r.X},{r.Y}) {r.W}x{r.H}");
        }

        // The readback is the one number that separates "the pass ran" from "the
        // pass reflected something": every line above reads the same if the shader
        // returns nothing.
        Console.WriteLine($"[KF2] reflections: last readback {ScreenReflections.ReflectivePct:F1}% of the picture reflective, " +
                          $"{ScreenReflections.HitPct:F1}% of that hit a surface, {ScreenReflections.SkyPct:F1}% took the sky, " +
                          $"{ScreenReflections.OverlayPct:F1}% hit under the HUD and were refused, " +
                          $"{ScreenReflections.PassedPct:F1}% passed behind something on the way, " +
                          $"mean weight {ScreenReflections.MeanWeight:F3}; sky taken {ScreenReflections.SkyColour}, " +
                          $"hits {ScreenReflections.HitLuma:F0} bright after the path fog kept {ScreenReflections.HitKeep:F2} " +
                          $"({ScreenReflections.HitFoggedPct:F1}% more than halved)");
        if (ScreenReflections.Map is { } map) Console.Write(map);
        ScreenReflections.WantMap = true;

        // AmbientOcclusion's probe resets AoGeometry's counters on its own window;
        // with it off, this is the only reader.
        if (!GteDepth.AoProbe) AoGeometry.ResetCounters();
        SurfaceMaterial.ResetCounters();
        ScreenReflections.ResetCounters();
        _frames = 0;
    }
}
