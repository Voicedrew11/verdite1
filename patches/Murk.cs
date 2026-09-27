using RecompOne.Runtime;
using RecompOne.Runtime.Events;

namespace Kf2;

/// <summary>
/// Murky water: what is under the water fades to the murk with the distance the view
/// ray runs through the water to it, and the water itself is hazed with the murk.
///
///     KF2_MURK=1              on (off by default)
///     KF2_MURK_DISTANCE=200   the distance through water that takes 63% of the way to the murk
///     KF2_MURK_HAZE=2654      the haze on the water itself, the same measure over a fixed thickness; 0 none
///
/// The distances and the colour are also sliders under the checkbox, saved.
///
/// Composited by the reflection pass (<see cref="WaterMurk"/>), which runs for it on
/// its own: no reflection needs to be on. The water is found as the reflections find
/// it, from <see cref="Reflections"/>' rectangles. See "Murky water" in
/// docs/RENDERING.md.
/// </summary>
public static class Murk
{
    public const string OnKey = "kf2.murk.on";
    public const string DistanceKey = "kf2.murk.depth";
    // The first model's distance, which hazed the water itself: the same measure.
    public const string HazeKey = "kf2.murk.distance";
    public const string RKey = "kf2.murk.r", GKey = "kf2.murk.g", BKey = "kf2.murk.b";

    public const float DefaultDistance = 200f;
    public const float DefaultHaze = 2654f;
    public const bool DefaultOn = false;
    public const float DefaultR = 0.03f, DefaultG = 0.05f, DefaultB = 0.06f;

    static bool? _forced;
    static float? _forcedDistance, _forcedHaze;

    public static bool Enabled => WaterMurk.Enabled;

    public static void Configure(string? on, string? distance, string? haze)
    {
        if (!string.IsNullOrWhiteSpace(on)) _forced = on != "0";
        if (float.TryParse(distance, out float d) && d > 0f) _forcedDistance = d;
        if (float.TryParse(haze, out float h) && h >= 0f) _forcedHaze = h;
    }

    public static void Install()
    {
        WaterMurk.Enabled = _forced ?? DefaultOn;
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            var view = RecompOne.Runtime.Runtime.View;
            WaterMurk.Enabled = _forced ?? view.GetBool(OnKey, DefaultOn);
            WaterMurk.Distance = _forcedDistance ?? view.GetFloat(DistanceKey, DefaultDistance);
            WaterMurk.Haze = _forcedHaze ?? view.GetFloat(HazeKey, DefaultHaze);
            WaterMurk.R = Math.Clamp(view.GetFloat(RKey, DefaultR), 0f, 1f);
            WaterMurk.G = Math.Clamp(view.GetFloat(GKey, DefaultG), 0f, 1f);
            WaterMurk.B = Math.Clamp(view.GetFloat(BKey, DefaultB), 0f, 1f);
            Console.WriteLine($"[KF2] murky water: {(Enabled ? $"on, {WaterMurk.Distance:F0} units, haze {WaterMurk.Haze:F0}, to " +
                                                              $"{WaterMurk.R:F2},{WaterMurk.G:F2},{WaterMurk.B:F2}" : "off")}");
        });
        Event.AddListener<OverlayLoadedEvent>(_ => MurkLevel.Forget());
    }

    public static void SetEnabled(bool on)
    {
        WaterMurk.Enabled = on;
        if (!GteDepth.Reflections) SurfaceMaterial.RectN = 0;
    }
}
