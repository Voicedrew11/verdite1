using ImGuiNET;
using RecompOne.Runtime.Config;
using RecompOne.Runtime.Host;
using RecompOne.Runtime.Host.Window;
using Silk.NET.Input;

namespace Verdite.Core;

/// <summary>
/// The sixteen pad buttons and what presses them: the Input pane's binding table.
/// The middle column, what each button does in the game, is the port's to hand in,
/// as button name to text (<c>"Cross"</c> to its description). A button the map
/// does not name gets an empty cell. The names are the labels in the first column:
/// Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, Start, Select, Up, Down,
/// Left, Right.
///
/// **It is a copy of the runtime's table rather than a call into it**, because
/// <c>InputSettingsSection</c> is <c>internal</c>: the port replaces that section by
/// id, so the runtime's own body is unreachable and would not run anyway. Copied
/// from <c>RecompOne.Runtime/Host/Window/Settings/Sections/InputSettingsSection.cs</c>:
/// the row tuples, the per-row capture, and <see cref="PadLabel"/>'s SDL indices.
/// **That last one is the only number the copy duplicates rather than derives**. It
/// is <c>InputManager</c>'s encoding, and a change to it there is silent here and
/// shows as a mislabelled binding. A pin bump should diff that file.
///
/// Pad 2 is not offered, because the game has one controller. This stops the port
/// *offering* pad 2 and does not stop the runtime *having* one:
/// <c>InputManager.Poll</c> still fills <c>Controller.State2</c>, and those are never
/// read or written here, so a settings file that already carries them keeps them.
/// </summary>
public static class BindingTable
{
    // Label, the two binding accessors for each device. Order is the PSX pad's own,
    // matching the runtime's, so a player comparing this pane with any other is not
    // reading a reshuffled list. What each button does is the port's (the map handed
    // to Draw), not written here.
    static readonly (string Label,
                     Func<KeyBindings, string> GetKey, Action<KeyBindings, string> SetKey,
                     Func<GamepadBindings, int[]> GetPad, Action<GamepadBindings, int[]> SetPad)[] _rows =
    [
        ("Cross",    b => b.Cross,    (b,v) => b.Cross = v,    p => p.Cross,    (p,v) => p.Cross = v),
        ("Circle",   b => b.Circle,   (b,v) => b.Circle = v,   p => p.Circle,   (p,v) => p.Circle = v),
        ("Square",   b => b.Square,   (b,v) => b.Square = v,   p => p.Square,   (p,v) => p.Square = v),
        ("Triangle", b => b.Triangle, (b,v) => b.Triangle = v, p => p.Triangle, (p,v) => p.Triangle = v),
        ("L1",       b => b.L1,       (b,v) => b.L1 = v,       p => p.L1,       (p,v) => p.L1 = v),
        ("R1",       b => b.R1,       (b,v) => b.R1 = v,       p => p.R1,       (p,v) => p.R1 = v),
        ("L2",       b => b.L2,       (b,v) => b.L2 = v,       p => p.L2,       (p,v) => p.L2 = v),
        ("R2",       b => b.R2,       (b,v) => b.R2 = v,       p => p.R2,       (p,v) => p.R2 = v),
        ("L3",       b => b.L3,       (b,v) => b.L3 = v,       p => p.L3,       (p,v) => p.L3 = v),
        ("R3",       b => b.R3,       (b,v) => b.R3 = v,       p => p.R3,       (p,v) => p.R3 = v),
        ("Start",    b => b.Start,    (b,v) => b.Start = v,    p => p.Start,    (p,v) => p.Start = v),
        ("Select",   b => b.Select,   (b,v) => b.Select = v,   p => p.Select,   (p,v) => p.Select = v),
        ("Up",       b => b.Up,       (b,v) => b.Up = v,       p => p.Up,       (p,v) => p.Up = v),
        ("Down",     b => b.Down,     (b,v) => b.Down = v,     p => p.Down,     (p,v) => p.Down = v),
        ("Left",     b => b.Left,     (b,v) => b.Left = v,     p => p.Left,     (p,v) => p.Left = v),
        ("Right",    b => b.Right,    (b,v) => b.Right = v,    p => p.Right,    (p,v) => p.Right = v),
    ];

    /// <summary>Every key the capture scans. Cached because the runtime's version
    /// calls <c>Enum.GetValues</c> inside the per-frame poll, which allocates a
    /// fresh array on every frame a row is waiting for a key.</summary>
    static readonly Key[] _keys = [.. Enum.GetValues<Key>()];

    /// <summary>
    /// Draw the table for one device. <paramref name="actionColumn"/> is the heading of
    /// the middle column (the port's name for the game); <paramref name="actions"/> is
    /// its text, button name to description, and a missing name draws an empty cell.
    ///
    /// The capture row is the caller's, not a field here, because the port keeps one
    /// per tab: two fields make switching tab mid-capture a non-event by construction
    /// rather than by remembering.
    /// </summary>
    public static void Draw(bool gamepad, ref int remapRow, ref bool remapAdd,
        string actionColumn, IReadOnlyDictionary<string, string> actions)
    {
        // Three columns rather than two and a SameLine: the binding cell is a
        // full-width button, so dimmed text after it cannot align, and sixteen
        // rows of unaligned text is a list rather than a column. A column also
        // earns a header, which is where "by default" gets said once instead of
        // sixteen times.
        if (!ImGui.BeginTable($"##{Game.Id}-bindings", 3,
                ImGuiTableFlags.Borders | ImGuiTableFlags.RowBg | ImGuiTableFlags.SizingStretchProp))
            return;

        ImGui.TableSetupColumn(Localization.T("settings.input.button"), ImGuiTableColumnFlags.WidthFixed, 78f);
        // English, and so is the column under it, while its two siblings come from
        // the runtime's own three-language table. That is not an oversight: every
        // string the port writes is English, and a verb mistranslated in a column a
        // player reads as measurement is worse than one they can see is the port's.
        // It also keeps this off Localization.Merge.
        ImGui.TableSetupColumn(actionColumn, ImGuiTableColumnFlags.WidthStretch, 0.85f);
        ImGui.TableSetupColumn(Localization.T(gamepad ? "settings.input.gamepad" : "settings.input.keyboard"),
            ImGuiTableColumnFlags.WidthStretch, 1.00f);
        ImGui.TableHeadersRow();

        var keys = ConfigManager.Game.Keys;
        var pad = ConfigManager.Game.Pad;

        for (int i = 0; i < _rows.Length; i++)
        {
            var row = _rows[i];
            ImGui.TableNextRow();

            ImGui.TableSetColumnIndex(0);
            ImGui.TextUnformatted(row.Label);

            // Single-line and clipped by the column, not TextWrapped: sixteen rows of
            // uneven height would cost more of the pane than the whole column is
            // worth, and every string here is short enough to fit.
            ImGui.TableSetColumnIndex(1);
            Dim(actions.TryGetValue(row.Label, out var action) ? action : "");

            ImGui.TableSetColumnIndex(2);
            if (gamepad) PadCell(i, row, pad, ref remapRow, ref remapAdd);
            else KeyCell(i, row, keys, ref remapRow);
        }

        ImGui.EndTable();
    }

    static void KeyCell(int i,
        (string Label, Func<KeyBindings, string> GetKey, Action<KeyBindings, string> SetKey,
         Func<GamepadBindings, int[]> GetPad, Action<GamepadBindings, int[]> SetPad) row,
        KeyBindings keys, ref int remapRow)
    {
        bool awaiting = remapRow == i;
        var key = row.GetKey(keys);
        var text = awaiting
            ? Localization.T("settings.input.press_key")
            : $"{(key.Length == 0 ? Localization.T("settings.input.unbound") : key)}##k{i}";

        if (ImGui.Button(text, new System.Numerics.Vector2(-1, 0))) remapRow = i;
        if (!awaiting) return;

        if (GetPressedKey() is { } pressed)
        {
            row.SetKey(keys, pressed);
            remapRow = -1;
            ConfigManager.SaveGame();
        }
    }

    static void PadCell(int i,
        (string Label, Func<KeyBindings, string> GetKey, Action<KeyBindings, string> SetKey,
         Func<GamepadBindings, int[]> GetPad, Action<GamepadBindings, int[]> SetPad) row,
        GamepadBindings pad, ref int remapRow, ref bool remapAdd)
    {
        bool awaiting = remapRow == i;
        var bindings = row.GetPad(pad);
        string text = awaiting
            ? Localization.T(remapAdd ? "settings.input.press_button_add" : "settings.input.press_button")
            : bindings.Length == 0 ? Localization.T("settings.input.unbound")
                                   : string.Join(" | ", bindings.Select(PadLabel));

        float plusW = ImGui.GetFrameHeight();
        float spacing = ImGui.GetStyle().ItemSpacing.X;
        if (ImGui.Button($"{text}##p{i}", new System.Numerics.Vector2(-plusW - spacing, 0)))
        {
            remapRow = i;
            remapAdd = false;
        }
        ImGui.SameLine();
        if (ImGui.Button($"+##add{i}", new System.Numerics.Vector2(plusW, 0)))
        {
            remapRow = i;
            remapAdd = true;
        }

        if (!awaiting) return;

        // InputManager is internal, so this and IsPadConnected are reached through
        // HostWindow, the same way the mouse is.
        if (HostWindow.GetFirstPressedPadButton(0) is { } p)
        {
            if (remapAdd)
            {
                if (!bindings.Contains(p)) row.SetPad(pad, [.. bindings, p]);
            }
            else row.SetPad(pad, [p]);

            remapRow = -1;
            ConfigManager.SaveGame();
        }
    }

    static string? GetPressedKey()
    {
        foreach (var k in _keys)
        {
            if (k is Key.Unknown or Key.Menu) continue;
            if (HostWindow.IsKeyDown(k)) return k.ToString();
        }
        return null;
    }

    /// <summary>InputManager's own encoding: face and shoulder buttons are their
    /// SDL index, the triggers are 100/101, the stick directions 102-109.</summary>
    static string PadLabel(int b) => b switch
    {
        0 => "Cross (A)",
        1 => "Circle (B)",
        2 => "Square (X)",
        3 => "Triangle (Y)",
        4 => "Select (Back)",
        5 => "Guide",
        6 => "Start",
        7 => "L3 (LStick)",
        8 => "R3 (RStick)",
        9 => "L1 (LBumper)",
        10 => "R1 (RBumper)",
        11 => "D-Up",
        12 => "D-Down",
        13 => "D-Left",
        14 => "D-Right",
        100 => "L2 (LTrigger)",
        101 => "R2 (RTrigger)",
        102 => "LStick Left",
        103 => "LStick Right",
        104 => "LStick Up",
        105 => "LStick Down",
        106 => "RStick Left",
        107 => "RStick Right",
        108 => "RStick Up",
        109 => "RStick Down",
        _ => $"Btn {b}",
    };

    /// <summary>ImGuiEx.TextDisabled is internal to the runtime, so this spells it
    /// out, the same three lines every page of the port carries.</summary>
    static void Dim(string text)
    {
        if (text.Length == 0) return;
        ImGui.PushStyleColor(ImGuiCol.Text, ImGui.GetStyle().Colors[(int)ImGuiCol.TextDisabled]);
        ImGui.TextUnformatted(text);
        ImGui.PopStyleColor();
    }
}
