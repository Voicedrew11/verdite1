using System.Globalization;

namespace Verdite.Core;

/// <summary>How a setting's value is kept in interface.ini: the text
/// <c>ViewConfig.SetInt</c> or <c>SetFloat</c> writes, so a key keeps the format it
/// had before the model existed and no player's file needs migrating.</summary>
public enum Stored { Int, Float }

/// <summary>How the Settings window draws a setting; <see cref="None"/> leaves it
/// to a section's own code (a compound control, or a developer tab).</summary>
public enum Ui { None, Checkbox, Combo, SliderInt, SliderFloat }

/// <summary>
/// One port setting, declared once for both screens: the Settings window (ImGui)
/// and the page drawn with the game's own menu routines. See docs/SETTINGS.md in a port.
///
/// Every value is a double. A switch is 0/1, a choice is the chosen value itself
/// (an aspect ratio, a frame rate, a slot), so the key keeps what it held before.
/// <see cref="Steps"/> is what Left/Right in the game's menu moves through; a live
/// value that is none of them (a slider's 137 fps, a variable's 1.85:1) is shown as
/// it is and never rounded onto a step until the player moves it.
/// </summary>
public sealed class PortSetting
{
    public required string Key { get; init; }

    /// <summary>The <c>{Tag}_*</c> variables that set this at boot. One set locks the
    /// setting in game: a saved change would be overridden again at the next boot.</summary>
    public string[] Envs { get; init; } = [];

    /// <summary>The Settings window's label: a localization key when
    /// <see cref="Localized"/>, else the text itself.</summary>
    public required string Label { get; init; }
    public bool Localized { get; init; }
    public string? Tip { get; init; }

    /// <summary>The in-game page and row, in <see cref="MenuFont"/>'s characters;
    /// no page keeps a setting to the Settings window.</summary>
    public string? Page { get; init; }
    public string? MenuLabel { get; init; }

    public required double[] Steps { get; init; }

    /// <summary>A value as the game's font shows it.</summary>
    public required Func<double, string> MenuValue { get; init; }

    /// <summary>The Settings window's names of <see cref="Steps"/>, for a combo.</summary>
    public string[]? Names { get; init; }

    /// <summary>What the setting is with no key kept and no variable set. Checked
    /// against the live value at boot (the port's <c>PortSettings</c>).</summary>
    public required double Default { get; init; }

    public required Func<double> Live { get; init; }
    public required Action<double> Apply { get; init; }

    public Stored Stored { get; init; } = Stored.Int;
    public Ui Ui { get; init; } = Ui.Checkbox;
    public double Min { get; init; }
    public double Max { get; init; }

    /// <summary>A slider's text for its value; ImGui's own format when null.</summary>
    public Func<double, string>? SliderText { get; init; }

    /// <summary>Takes effect at the next boot: a change is kept but not applied.</summary>
    public bool AtBoot { get; init; }

    /// <summary>Dims the setting (another one it needs is off); null is always usable.</summary>
    public Func<bool>? Usable { get; init; }

    public bool IsUsable => Usable?.Invoke() ?? true;

    /// <summary>A row of the game's page that stands for several settings: no key of
    /// its own, its value <see cref="Join"/> of theirs (each read through the getter it
    /// is handed, the live or the staged value), and a step stages each part with the
    /// value <see cref="Split"/> gives it (NaN: as it was when the page opened). Built by
    /// the port's <c>PortSettings.Combine</c>.</summary>
    public PortSetting[]? Parts { get; init; }
    public Func<Func<PortSetting, double>, double>? Join { get; init; }
    public Func<double, Func<PortSetting, double>, double[]>? Split { get; init; }

    /// <summary>The keys and text a value is kept as, null text removing the key; one
    /// key in <see cref="Encode"/>'s text when null. A null value is the default.</summary>
    public Func<double?, (string Key, string? Text)[]>? Keys { get; init; }

    public (string Key, string? Text)[] Texts(double? value) =>
        Keys?.Invoke(value) ?? [(Key, value is double v ? Encode(v) : null)];

    public string? LockedBy =>
        Envs.FirstOrDefault(e => !string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable(e)));

    public static bool Same(double a, double b) => Math.Abs(a - b) < 1e-4;

    /// <summary>A value as the port settings page writes it: at most two decimals.</summary>
    public static string Number(double v) => v.ToString("0.##", CultureInfo.InvariantCulture);

    /// <summary>A value's name from a list of steps: the name of the step it equals, else its number.</summary>
    public static Func<double, string> Named(double[] steps, params string[] names) => v =>
        Array.FindIndex(steps, s => Same(s, v)) is >= 0 and var i ? names[i] : Number(v);

    public int StepIndex(double value)
    {
        for (int i = 0; i < Steps.Length; i++)
            if (Same(Steps[i], value)) return i;
        return -1;
    }

    /// <summary>Left (-1) or Right (+1) from <paramref name="value"/>, round the ends:
    /// Right from the last step is the first, Left from the first the last. A switch
    /// flips either way. A value off the steps goes to the nearest one on that side,
    /// or round to the far end when there is none.</summary>
    public double Next(double value, int dir)
    {
        int n = Steps.Length;
        int i = StepIndex(value);
        if (n == 2 && i >= 0) return Steps[1 - i];
        if (i >= 0) return Steps[((i + Math.Sign(dir)) % n + n) % n];
        var side = dir > 0 ? Steps.Where(s => s > value) : Steps.Where(s => s < value);
        if (side.Any()) return dir > 0 ? side.Min() : side.Max();
        return n == 0 ? value : dir > 0 ? Steps.Min() : Steps.Max();
    }

    public string Encode(double value) => Stored switch
    {
        Stored.Float => ((float)value).ToString(CultureInfo.InvariantCulture),
        _ => ((int)Math.Round(value)).ToString(CultureInfo.InvariantCulture),
    };

    public string ImGuiLabel => Localized ? RecompOne.Runtime.Host.Window.Localization.T(Label) : Label;
}
