using System.Diagnostics;
using System.Numerics;
using ImGuiNET;

namespace RecompOne.Runtime.Host.Window;

//0107. The window a crash leaves up (Runtime.HoldAfterCrash): what happened, where
//the report is, a button to its folder and one to quit.
public sealed class CrashPopup : Popup
{
    private static volatile bool _wanted;
    private static string? _report;

    protected override string TitleKey => "crash.title";
    protected override Vector2 Size => new(560f, 0f);
    protected override bool Closable => false;

    public static void Show(string? report)
    {
        _report = report == null ? null : Path.GetFullPath(report);
        _wanted = true;
    }

    protected internal override void Update()
    {
        if (_wanted && !IsOpen) Open();
    }

    protected override void DrawContent()
    {
        UiText.CenteredWrapped(Localization.T("crash.body"));
        ImGui.Spacing();
        if (_report != null)
        {
            UiText.CenteredWrapped(Localization.T("crash.saved"));
            ImGui.Spacing();
            ImGui.TextWrapped(_report);
        }
        else
        {
            UiText.CenteredWrapped(Localization.T("crash.no_report"));
        }

        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        var width = (ImGui.GetContentRegionAvail().X - ImGui.GetStyle().ItemSpacing.X) * 0.5f;
        if (_report != null)
        {
            if (ImGui.Button(Localization.T("crash.open_folder"), new Vector2(width, 0f))) OpenFolder();
            ImGui.SameLine();
        }
        else
        {
            width = -1f;
        }

        if (ImGui.Button(Localization.T("crash.quit"), new Vector2(width, 0f)))
        {
            try
            {
                Runtime.Shutdown();
            }
            catch
            {
            }

            Environment.Exit(1);
        }
    }

    private static void OpenFolder()
    {
        try
        {
            var folder = Path.GetDirectoryName(_report);
            if (folder != null) Process.Start(new ProcessStartInfo(folder) { UseShellExecute = true });
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[Runtime] could not open the report's folder: {e.Message}");
        }
    }
}
