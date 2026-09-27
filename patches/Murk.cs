using RecompOne.Runtime;
using RecompOne.Runtime.Events;

namespace Kf2;

/// <summary>
/// Murky water: water darkens with the distance the view ray runs through it to the
/// floor, so a shallow edge stays clear and a deep pool goes dark.
///
///     KF2_MURK=1              on (off by default)
///     KF2_MURK_DISTANCE=2654  the distance through water that takes 63% of the way to the murk
///
/// The distance and the colour are also sliders under the checkbox, saved.
///
/// Composited by the reflection pass (<see cref="WaterMurk"/>), which runs for it on
/// its own: no reflection needs to be on. The water is found as the reflections find
/// it, from <see cref="Reflections"/>' rectangles. See "Murky water" in
/// docs/RENDERING.md.
/// </summary>
public static class Murk
{
    public const string OnKey = "kf2.murk.on";
    public const string DistanceKey = "kf2.murk.distance";
    public const string RKey = "kf2.murk.r", GKey = "kf2.murk.g", BKey = "kf2.murk.b";

    public const float DefaultDistance = 2654f;
    public const bool DefaultOn = false;
    public const float DefaultR = 0.03f, DefaultG = 0.05f, DefaultB = 0.06f;

    static bool? _forced;
    static float? _forcedDistance;

    public static bool Enabled => WaterMurk.Enabled;

    public static void Configure(string? on, string? distance)
    {
        if (!string.IsNullOrWhiteSpace(on)) _forced = on != "0";
        if (float.TryParse(distance, out float d) && d > 0f) _forcedDistance = d;
    }

    public static void Install()
    {
        WaterMurk.Enabled = _forced ?? DefaultOn;
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            var view = RecompOne.Runtime.Runtime.View;
            WaterMurk.Enabled = _forced ?? view.GetBool(OnKey, DefaultOn);
            WaterMurk.Distance = _forcedDistance ?? view.GetFloat(DistanceKey, DefaultDistance);
            WaterMurk.R = Math.Clamp(view.GetFloat(RKey, DefaultR), 0f, 1f);
            WaterMurk.G = Math.Clamp(view.GetFloat(GKey, DefaultG), 0f, 1f);
            WaterMurk.B = Math.Clamp(view.GetFloat(BKey, DefaultB), 0f, 1f);
            Console.WriteLine($"[KF2] murky water: {(Enabled ? $"on, {WaterMurk.Distance:F0} units to " +
                                                              $"{WaterMurk.R:F2},{WaterMurk.G:F2},{WaterMurk.B:F2}" : "off")}");
        });
        Event.AddListener<OverlayLoadedEvent>(_ => MurkFloor.Forget());
    }

    public static void SetEnabled(bool on)
    {
        WaterMurk.Enabled = on;
        if (!GteDepth.Reflections) SurfaceMaterial.RectN = 0;
    }
}
