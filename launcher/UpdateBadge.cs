using System.Diagnostics;
using System.Numerics;
using ImGuiNET;
using RecompOne.Runtime.Config;
using RecompOne.Runtime.Host.Window;

namespace Verdite.Launcher;

/// <summary>
/// A gold "Update available!" badge at the right of the menu bar, left of the FPS
/// counter, once <see cref="UpdateCheck"/> has found a newer release. A click opens a
/// small menu: the download page, skip this version, or hide it for the session.
/// </summary>
static class UpdateBadge
{
    const string MenuId = "##verdite-update";

    static readonly Vector4 Gold = new(0.83f, 0.69f, 0.22f, 1f);
    static readonly Vector4 Ink = new(0.08f, 0.07f, 0.02f, 1f);

    static bool _hidden;

    public static void Install()
    {
        PopupManager.Register(new UpdatePopup());
        MainMenuBar.AddRightItem(Width, Draw);
        SettingsRegistry.Extend("interface", DrawSetting);
    }

    static UpdateCheck.Release? Shown => _hidden ? null : UpdateCheck.Available;

    static string Label => Localization.T("verdite.update.badge");

    static float Width() =>
        Shown is null ? 0f : ImGui.CalcTextSize(Label).X + ImGui.GetStyle().FramePadding.X * 2f;

    static void Draw()
    {
        if (Shown is not { } release) return;

        PushGold();
        ImGui.PushStyleVar(ImGuiStyleVar.FrameRounding, 0f);
        if (ImGui.Button(Label)) ImGui.OpenPopup(MenuId);
        ImGui.PopStyleVar();
        PopGold();

        if (!ImGui.BeginPopup(MenuId)) return;

        ImGui.TextDisabled(string.Format(Localization.T("verdite.update.versions"), release.Tag.TrimStart('v'), Ver.Number));
        ImGui.Separator();
        if (ImGui.MenuItem(Localization.T("verdite.update.open"))) OpenUrl(release.Url);
        if (ImGui.MenuItem(Localization.T("verdite.update.skip"))) Skip(release);
        if (ImGui.MenuItem(Localization.T("verdite.update.hide"))) _hidden = true;

        ImGui.EndPopup();
    }

    /// <summary>The badge's colours, shared with the startup popup's main button.</summary>
    public static void PushGold()
    {
        ImGui.PushStyleColor(ImGuiCol.Button, Gold);
        ImGui.PushStyleColor(ImGuiCol.ButtonHovered, Vector4.Lerp(Gold, Vector4.One, 0.2f));
        ImGui.PushStyleColor(ImGuiCol.ButtonActive, Vector4.Lerp(Gold, Vector4.One, 0.35f));
        ImGui.PushStyleColor(ImGuiCol.Text, Ink);
    }

    public static void PopGold()
    {
        ImGui.PopStyleColor(4);
    }

    /// <summary>Never announce this release again, badge included.</summary>
    public static void Skip(UpdateCheck.Release release)
    {
        UpdateCheck.Skip(release.Tag);
        _hidden = true;
    }

    public static void OpenUrl(string url)
    {
        try
        {
            Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"{Launcher.Tag} could not open {url}: {e.Message}");
        }
    }

    static void DrawSetting()
    {
        ImGui.Spacing();
        var on = UpdateCheck.Enabled;
        if (ImGui.Checkbox(Localization.T("verdite.update.setting"), ref on))
        {
            UpdateCheck.Enabled = on;
            ConfigManager.SaveView(PanelManager.Panels);
        }

        if (ImGui.IsItemHovered()) ImGui.SetTooltip(Localization.T("verdite.update.setting_hint"));
    }

    /// <summary>All three of the runtime's languages, as for BuildProgressPopup.</summary>
    public const string Strings = """
    {
      "strings": {
        "verdite.update.badge": {
          "en": "Update available!",
          "pt-BR": "Atualização disponível!",
          "es-419": "¡Actualización disponible!"
        },
        "verdite.update.title": {
          "en": "Update available",
          "pt-BR": "Atualização disponível",
          "es-419": "Actualización disponible"
        },
        "verdite.update.available": {
          "en": "%NAME% {0} is available.",
          "pt-BR": "O %NAME% {0} está disponível.",
          "es-419": "%NAME% {0} está disponible."
        },
        "verdite.update.running": {
          "en": "You have {0}.",
          "pt-BR": "Você tem a versão {0}.",
          "es-419": "Tienes la versión {0}."
        },
        "verdite.update.download": {
          "en": "Download page",
          "pt-BR": "Página de download",
          "es-419": "Página de descarga"
        },
        "verdite.update.later": {
          "en": "Later",
          "pt-BR": "Mais tarde",
          "es-419": "Más tarde"
        },
        "verdite.update.versions": {
          "en": "%NAME% {0} (you have {1})",
          "pt-BR": "%NAME% {0} (você tem a {1})",
          "es-419": "%NAME% {0} (tienes la {1})"
        },
        "verdite.update.open": {
          "en": "Open download page",
          "pt-BR": "Abrir página de download",
          "es-419": "Abrir página de descarga"
        },
        "verdite.update.skip": {
          "en": "Skip this version",
          "pt-BR": "Pular esta versão",
          "es-419": "Omitir esta versión"
        },
        "verdite.update.hide": {
          "en": "Hide until next launch",
          "pt-BR": "Ocultar até a próxima execução",
          "es-419": "Ocultar hasta el próximo inicio"
        },
        "verdite.update.setting": {
          "en": "Check for updates at launch",
          "pt-BR": "Procurar atualizações ao iniciar",
          "es-419": "Buscar actualizaciones al iniciar"
        },
        "verdite.update.setting_hint": {
          "en": "Asks GitHub for the latest release at most once an hour. Nothing is downloaded.",
          "pt-BR": "Consulta o GitHub pela versão mais recente no máximo uma vez por hora. Nada é baixado.",
          "es-419": "Consulta a GitHub por la versión más reciente como máximo una vez por hora. No se descarga nada."
        }
      }
    }
    """;
}
