using ImGuiNET;
using RecompOne.Runtime;

namespace Kf2.Settings;

/// <summary>Interpolation and reverb, under Audio. See <see cref="AudioQuality"/>.</summary>
public sealed class AudioPage : IPatchPage
{
    public string Id => "audio-quality";
    public string Title => "Quality";

    static readonly string[] InterpLabels = ["Gaussian (original)", "Cubic", "Sinc"];
    static readonly string[] ReverbLabels = ["Original", "Enhanced"];
    static readonly string[] PositionalLabels = ["Original", "Speakers", "Headphones"];

    public void Draw()
    {
        var interp = (int)Spu.Interpolation;
        if (ImGui.Combo("Interpolation", ref interp, InterpLabels, InterpLabels.Length))
        {
            AudioQuality.SetInterpolation((SpuInterpolation)interp);
            PatchSettings.Set(AudioQuality.InterpKey, interp);
        }

        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("How music and sound effects are resampled: the console's soft filter, or sharper and brighter.");

        var reverb = Spu.ReverbMode == SpuReverbMode.Enhanced ? 1 : 0;
        if (ImGui.Combo("Reverb", ref reverb, ReverbLabels, ReverbLabels.Length))
        {
            AudioQuality.SetEnhancedReverb(reverb == 1);
            PatchSettings.Set(AudioQuality.ReverbKey, reverb);
        }

        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("The console's reverb, or a denser, smoother one sized from the same room settings.");

        if (!PositionalAudio.Installed) return;

        var positional = (int)PositionalAudio.Current;
        if (ImGui.Combo("Positional audio", ref positional, PositionalLabels, PositionalLabels.Length))
        {
            PositionalAudio.Set((PositionalAudio.Mode)positional);
            PatchSettings.Set(PositionalAudio.ModeKey, positional);
        }

        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("Sound effects stay where they come from as you move and turn. Headphones adds front and back.");
    }
}
