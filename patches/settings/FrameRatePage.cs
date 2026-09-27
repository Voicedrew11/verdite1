using ImGuiNET;

namespace Kf2.Settings;

/// <summary>
/// King's Field's frame rate and view smoothing, under Video. The same slider
/// King's Field II's <see cref="FramePacingPage"/> has -- positions that are the
/// panels people own, a note that the game's speed does not change -- driving
/// <see cref="GateRedraw"/> and <see cref="ViewCarry"/> instead. 20 is the game's
/// own rate and redraws nothing; smoothing is dimmed there, since a picture drawn
/// only at the tick has nothing to carry.
/// </summary>
public sealed class FrameRatePage : IPatchPage
{
    public string Id => "kf1-framerate";
    public string Title => "Frame pacing";
    public int Order => 10;

    static readonly double[] Pins = [20, 30, 40, 50, 60, 75, 90, 100, 120, 144, 165, 180, 240];

    static int _index;
    static bool _dragging;

    public void Draw()
    {
        double live = GateRedraw.TargetFps;
        if (!_dragging) _index = Nearest(live);

        if (ImGui.SliderInt("Frame rate", ref _index, 0, Pins.Length - 1,
                            $"{Pins[_index]:0} fps",
                            ImGuiSliderFlags.AlwaysClamp | ImGuiSliderFlags.NoInput))
        {
            GateRedraw.SetTargetFps(Pins[_index]);
            PatchSettings.Set(GateRedraw.FpsKey, (float)Pins[_index]);
        }
        _dragging = ImGui.IsItemActive();

        if (!_dragging && Math.Abs(live - Pins[_index]) > 0.5)
            PatchSettings.Note($"Running at {live:0.#} fps, set outside this menu.");
        PatchSettings.Note(GateRedraw.Measured > 0.0
            ? $"Measured: {GateRedraw.Measured:F1} fps"
            : "Measured: waiting for the first second of frames");

        bool smooth = ViewCarry.Enabled;
        ImGui.BeginDisabled(!GateRedraw.Redrawing);
        if (ImGui.Checkbox("Smooth the view between game frames", ref smooth))
        {
            ViewCarry.SetEnabled(smooth);
            PatchSettings.Set(ViewCarry.OnKey, smooth);
        }
        ImGui.EndDisabled();

        ImGui.Spacing();
        PatchSettings.Note("Picture only: the game runs at its own 20 frames a second whatever this is set to. " +
                           "Enemies and objects still move at 20.");
    }

    static int Nearest(double rate)
    {
        int best = 0;
        for (int i = 1; i < Pins.Length; i++)
            if (Math.Abs(Pins[i] - rate) < Math.Abs(Pins[best] - rate)) best = i;
        return best;
    }
}
