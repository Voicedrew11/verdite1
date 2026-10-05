using System.Numerics;
using ImGuiNET;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Host.Window;

namespace Verdite.Launcher;

/// <summary>
/// The startup notice: once per launch, when <see cref="UpdateCheck"/> finds a newer
/// release, only while no other popup is open, so it never lands on the disc
/// picker or the build, and only outside play, since it is modal: once an overlay
/// the game says starts play has loaded (<see cref="LauncherGame.PlayAfter"/>), it
/// waits for the next title. <see cref="UpdateBadge"/> stays in the menu bar
/// either way.
/// </summary>
sealed class UpdatePopup : Popup
{
    protected override string TitleKey => "verdite.update.title";
    protected override Vector2 Size => new(420f, 0f);

    UpdateCheck.Release? _shown;
    volatile bool _inPlay;

    public UpdatePopup()
    {
        // Which overlays start play and which are the title is the game's to say.
        Event.AddListener<OverlayLoadedEvent>(e =>
        {
            if (Launcher.Game.PlayAfter(e.Name) is { } play) _inPlay = play;
        });
    }

    protected override void Update()
    {
        if (_shown is not null || IsOpen || _inPlay || PopupManager.AnyOpen) return;
        if (UpdateCheck.Available is not { } release) return;
        _shown = release;
        Open();
    }

    protected override void DrawContent()
    {
        if (_shown is not { } release) return;

        ImGui.TextWrapped(string.Format(Localization.T("verdite.update.available"), release.Tag.TrimStart('v')));
        ImGui.PushStyleColor(ImGuiCol.Text, ImGui.GetStyle().Colors[(int)ImGuiCol.TextDisabled]);
        ImGui.TextWrapped(string.Format(Localization.T("verdite.update.running"), Ver.Number));
        ImGui.PopStyleColor();
        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        string[] labels =
        [
            Localization.T("verdite.update.download"),
            Localization.T("verdite.update.skip"),
            Localization.T("verdite.update.later"),
        ];

        // One row of equal buttons when the widest label fits a third of the
        // popup, and a column of full-width ones when it does not (pt-BR, es-419).
        var style = ImGui.GetStyle();
        var avail = ImGui.GetContentRegionAvail().X;
        var widest = labels.Max(l => ImGui.CalcTextSize(l).X) + style.FramePadding.X * 2f;
        var row = widest * 3f + style.ItemSpacing.X * 2f <= avail;
        var size = new Vector2(row ? (avail - style.ItemSpacing.X * 2f) / 3f : avail, 0f);

        UpdateBadge.PushGold();
        var open = ImGui.Button(labels[0], size);
        UpdateBadge.PopGold();
        if (ImGui.IsItemHovered()) ImGui.SetTooltip(release.Url);
        if (open)
        {
            UpdateBadge.OpenUrl(release.Url);
            Close();
        }

        if (row) ImGui.SameLine();
        if (ImGui.Button(labels[1], size))
        {
            UpdateBadge.Skip(release);
            Close();
        }

        if (row) ImGui.SameLine();
        if (ImGui.Button(labels[2], size)) Close();
    }
}
