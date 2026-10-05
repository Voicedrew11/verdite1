using ImGuiNET;
using RecompOne.Runtime.Config;

namespace RecompOne.Runtime.Host.Window;

internal sealed class DisplaySettingsSection : ISettingsSection
{
    public string Id => "display";
    public string TitleKey => "settings.display";
    public int Order => 5;

    private static readonly string[] Backends = ["auto", "gl45", "gl33", "gl21"];

    public void Draw()
    {
        // Windowed, fullscreen, borderless: Fullscreen says "covers the screen" and
        // Borderless how, so F11 and the menu bar's toggle keep their meaning.
        var modes = new[]
        {
            Localization.T("settings.display.mode.windowed"),
            Localization.T("settings.display.fullscreen"),
            Localization.T("settings.display.mode.borderless")
        };
        var mode = !ConfigManager.View.Fullscreen ? 0 : ConfigManager.View.Borderless ? 2 : 1;
        if (ImGui.Combo(Localization.T("settings.display.mode"), ref mode, modes, modes.Length))
        {
            ConfigManager.View.Fullscreen = mode != 0;
            if (mode != 0) ConfigManager.View.Borderless = mode == 2;
            HostWindow.SetFullscreen(mode != 0);
            ConfigManager.SaveView(PanelManager.Panels);
        }

        if (ImGui.IsItemHovered()) ImGui.SetTooltip(Localization.T("settings.display.mode_hint"));

        var vsync = ConfigManager.View.VSync;
        if (ImGui.Checkbox(Localization.T("settings.display.vsync"), ref vsync))
        {
            ConfigManager.View.VSync = vsync;
            HostWindow.SetVSync(vsync);
            ConfigManager.SaveView(PanelManager.Panels);
        }

        if (ImGui.IsItemHovered()) ImGui.SetTooltip(Localization.T("settings.display.vsync_hint"));

        var scale = ConfigManager.View.RenderScale;
        if (ImGui.SliderInt(Localization.T("settings.display.render_scale"), ref scale, 1, 8, "%dx"))
        {
            ConfigManager.View.RenderScale = scale;
            ConfigManager.SaveView(PanelManager.Panels);
            NoticePopup.Show(Localization.T("common.restart_required"));
        }

        if (ImGui.IsItemHovered()) ImGui.SetTooltip(Localization.T("settings.display.render_scale_hint"));

        var lines = Hle.GpuHle.LastDisplayH;
        var width = Hle.GpuHle.LastDisplayW;
        if (lines > 0)
            ImGui.TextDisabled(Localization.T("settings.display.render_scale_lines",
                width, lines, width * scale, lines * scale, scale));

        if (scale != Hle.GlVram.Scale)
            ImGui.TextDisabled(Localization.T("settings.display.restart_pending"));

        // How big the picture is drawn, and how wide it is presented, are the same
        // kind of choice, so a port offering an aspect ratio has somewhere to put it
        // that is not a group appended below the whole section.
        SettingsRegistry.DrawSlot("display.render_scale");

        ImGui.Separator();

        var index = Array.IndexOf(Backends, ConfigManager.View.GpuBackend);
        if (index < 0) index = 0;
        if (ImGui.Combo(Localization.T("settings.display.backend"), ref index, Backends, Backends.Length))
        {
            ConfigManager.View.GpuBackend = Backends[index];
            ConfigManager.SaveView(PanelManager.Panels);
            NoticePopup.Show(Localization.T("common.restart_required"));
        }

        ImGui.TextDisabled(Localization.T("settings.display.backend_running", Hle.GpuBackendFactory.Selected));

        // Upstream draws a frame-rate slider and a PGXP block here; this port draws
        // neither, and a future merge should not take them back.
        //
        // The frame rate is upstream's *interpolated* one -- it writes Interp's key
        // and is disabled unless PGXP is on -- and this port never enters
        // PresentLoop, so Interp.Backend stays null and the slider changes nothing
        // it claims to. The port's own rate is patches/settings/FramePacingPage.cs,
        // under Video, and two frame-rate controls in one pane is one of them lying.
        //
        // PGXP is a mechanism nobody has judged the picture of: it buys no coverage
        // in this game (92-97% either way) and costs a fifth of the frame rate, so
        // it is a comparison rather than a setting and lives on the console under
        // KF2_PGXP*. See "PGXP" in docs/RENDERING.md.
    }
}
