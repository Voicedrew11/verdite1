using ImGuiNET;

namespace Kf2.Settings;

/// <summary>
/// <see cref="Mouse.Lead"/>, under Gameplay: whether mouse look reaches the
/// picture the frame the hand moves, or waits for the game's next 20 Hz tick.
/// See "The mouse leads the tick" in docs/INPUT.md.
/// </summary>
public sealed class MouseLeadPage : IPatchPage
{
    public string Id => "mouselead";

    public string Title => "";

    public int Order => 30;

    public void Draw()
    {
        // Dimmed rather than hidden while mouse look is off, as AutoReloadPage
        // dims its slot.
        ImGui.BeginDisabled(!Mouse.Enabled);

        bool on = Mouse.Lead;
        if (ImGui.Checkbox("Instant mouse look", ref on))
        {
            Mouse.Lead = on;
            PatchSettings.Set(Mouse.LeadKey, on);
        }

        if (ImGui.IsItemHovered(ImGuiHoveredFlags.AllowWhenDisabled))
            ImGui.SetTooltip("Turns the view the frame you move the mouse, instead of on the game's next 20 Hz tick.");

        ImGui.EndDisabled();
    }
}
