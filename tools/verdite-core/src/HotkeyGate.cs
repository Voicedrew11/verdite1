using ImGuiNET;

namespace Verdite.Core;

/// <summary>
/// Whether a letter hotkey should stand down for text. A key typed into a text field
/// reaches the <c>KeyboardEvent</c> bus as well, so every letter hotkey waits while
/// ImGui has one focused. A port adds what else stands its own hotkeys down at the
/// call site (a remaster editor, say), since core cannot name the port's types.
/// </summary>
public static class HotkeyGate
{
    public static bool Typing
        => ImGui.GetCurrentContext() != nint.Zero && ImGui.GetIO().WantTextInput;
}
