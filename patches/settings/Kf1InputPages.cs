using ImGuiNET;

namespace Kf2.Settings;

/// <summary>King's Field's mouse look, under Input ▸ Mouse. See <see cref="MouseLook"/>.</summary>
public sealed class MouseLookPage : IPatchPage
{
    public string Id => "kf1-mouse";
    public string Title => "Mouse look";
    public int Order => 10;

    public void Draw()
    {
        bool on = MouseLook.Enabled;
        if (ImGui.Checkbox("Mouse look", ref on))
        {
            MouseLook.Enabled = on;
            if (!on) MouseLook.SetCaptured(false);
            PatchSettings.Set(MouseLook.OnKey, on);
        }
        PatchSettings.Note($"{MouseLook.CaptureKey} captures the pointer and lets it go.");

        ImGui.BeginDisabled(!MouseLook.Enabled);
        float turn = MouseLook.TurnSens;
        if (ImGui.SliderFloat("Turn speed", ref turn, 0.1f, 4f, "%.2f"))
        {
            MouseLook.TurnSens = turn;
            PatchSettings.Set(MouseLook.TurnKey, turn);
        }
        float look = MouseLook.LookSens;
        if (ImGui.SliderFloat("Look speed", ref look, 0.1f, 4f, "%.2f"))
        {
            MouseLook.LookSens = look;
            PatchSettings.Set(MouseLook.LookKey, look);
        }
        bool invert = MouseLook.InvertY;
        if (ImGui.Checkbox("Invert vertical", ref invert))
        {
            MouseLook.InvertY = invert;
            PatchSettings.Set(MouseLook.InvertKey, invert);
        }
        bool lead = MouseLook.Lead;
        if (ImGui.Checkbox("Instant mouse look", ref lead))
        {
            MouseLook.Lead = lead;
            PatchSettings.Set(MouseLook.LeadKey, lead);
        }
        if (ImGui.IsItemHovered(ImGuiHoveredFlags.AllowWhenDisabled))
            ImGui.SetTooltip("Turns the view the frame you move the mouse, instead of on the game's next 20 Hz tick.");
        ImGui.EndDisabled();
    }
}

/// <summary>King's Field's twin-stick control, under Input ▸ Gamepad. See <see cref="TwinStick"/>.</summary>
public sealed class TwinStickPage : IPatchPage
{
    public string Id => "kf1-analog";
    public string Title => "Twin-stick";
    public int Order => 10;

    public void Draw()
    {
        bool on = TwinStick.Enabled;
        if (ImGui.Checkbox("Twin-stick control", ref on))
        {
            TwinStick.Enabled = on;
            PatchSettings.Set(TwinStick.OnKey, on);
        }
        PatchSettings.Note("Left stick walks and strafes; right stick turns and looks.");

        ImGui.BeginDisabled(!TwinStick.Enabled);
        float turn = TwinStick.TurnSens;
        if (ImGui.SliderFloat("Turn speed", ref turn, 0.2f, 3f, "%.2f"))
        {
            TwinStick.TurnSens = turn;
            PatchSettings.Set(TwinStick.TurnKey, turn);
        }
        float look = TwinStick.LookSens;
        if (ImGui.SliderFloat("Look speed", ref look, 0.2f, 3f, "%.2f"))
        {
            TwinStick.LookSens = look;
            PatchSettings.Set(TwinStick.LookKey, look);
        }
        float dz = TwinStick.Deadzone;
        if (ImGui.SliderFloat("Dead zone", ref dz, 0f, 0.5f, "%.2f"))
        {
            TwinStick.Deadzone = dz;
            PatchSettings.Set(TwinStick.DeadzoneKey, dz);
        }
        bool invert = TwinStick.InvertY;
        if (ImGui.Checkbox("Invert vertical", ref invert))
        {
            TwinStick.InvertY = invert;
            PatchSettings.Set(TwinStick.InvertKey, invert);
        }
        ImGui.EndDisabled();
    }
}
