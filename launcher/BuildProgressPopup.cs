using System.Numerics;
using ImGuiNET;
using RecompOne.Runtime.Host.Window;

namespace Verdite.Launcher;

/// <summary>
/// What the player looks at while the game is being built.
///
/// Popup and PopupManager.Register are public, so this needed nothing from the
/// runtime except a way to keep pumping the window while the work runs on another
/// thread -- Runtime.Pump, added by tools/RecompOne/patches/0030.
///
/// Not closable: there is no game to fall back to yet. A failure replaces the
/// progress line with the compiler's own message and leaves the popup up, which is
/// the only place a player can be told what went wrong before a window exists to
/// tell them in.
/// </summary>
sealed class BuildProgressPopup : Popup
{
    protected override string TitleKey => "verdite.build.title";
    protected override Vector2 Size => new(560f, 0f);
    protected override bool Closable => false;

    public string Status = "";
    public string? Error;
    public int Step;
    public int Steps = 3;

    protected override void DrawContent()
    {
        if (Error is { } error)
        {
            ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.45f, 0.4f, 1f));
            ImGui.TextWrapped(Localization.T("verdite.build.failed"));
            ImGui.PopStyleColor();
            ImGui.Spacing();
            ImGui.Separator();
            ImGui.Spacing();
            ImGui.TextWrapped(error);
            ImGui.Spacing();
            ImGui.Separator();
            ImGui.Spacing();
            ImGui.TextWrapped(Localization.T("verdite.build.log"));
            return;
        }

        ImGui.TextWrapped(Localization.T("verdite.build.explain"));
        ImGui.Spacing();
        ImGui.Separator();
        ImGui.Spacing();

        ImGui.Text($"{Localization.T(Status)}{Dots()}");
        ImGui.Spacing();
        ImGui.ProgressBar(Steps <= 0 ? 0f : Math.Clamp(Step / (float)Steps, 0f, 1f), new Vector2(-1f, 0f), $"{Step}/{Steps}");
    }

    /// <summary>
    /// An animated ellipsis, because every step here finishes in seconds and none
    /// of them can report a fraction of itself -- so a bar that only moves three
    /// times needs something beside it that says the process is alive.
    /// </summary>
    static string Dots() => new('.', 1 + (int)(ImGui.GetTime() * 2.0) % 3);

    /// <summary>
    /// The three languages the runtime ships. A new key has to supply all of them,
    /// unlike an override of an existing one, or the missing ones fall back to the
    /// key itself and the popup shows "verdite.build.title" to those players.
    /// </summary>
    public const string Strings = """
    {
      "strings": {
        "verdite.build.title": {
          "en": "Preparing %GAME%",
          "pt-BR": "Preparando %GAME%",
          "es-419": "Preparando %GAME%"
        },
        "verdite.build.explain": {
          "en": "%NAME% is building the game from your disc. This happens once; later launches start straight away.",
          "pt-BR": "O %NAME% está construindo o jogo a partir do seu disco. Isso acontece uma vez; as próximas execuções iniciam direto.",
          "es-419": "%NAME% está construyendo el juego desde tu disco. Esto ocurre una sola vez; los próximos inicios serán directos."
        },
        "verdite.build.reading": {
          "en": "Reading the disc",
          "pt-BR": "Lendo o disco",
          "es-419": "Leyendo el disco"
        },
        "verdite.build.translating": {
          "en": "Translating the game code",
          "pt-BR": "Traduzindo o código do jogo",
          "es-419": "Traduciendo el código del juego"
        },
        "verdite.build.compiling": {
          "en": "Compiling",
          "pt-BR": "Compilando",
          "es-419": "Compilando"
        },
        "verdite.build.failed": {
          "en": "The game could not be built.",
          "pt-BR": "Não foi possível construir o jogo.",
          "es-419": "No se pudo construir el juego."
        },
        "verdite.build.log": {
          "en": "The full report was written to build.log in the %NAME% data folder.",
          "pt-BR": "O relatório completo foi salvo em build.log na pasta de dados do %NAME%.",
          "es-419": "El informe completo se guardó en build.log en la carpeta de datos de %NAME%."
        }
      }
    }
    """;
}
