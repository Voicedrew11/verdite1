using RecompOne.Runtime.Config;
using Rt = RecompOne.Runtime.Runtime;

namespace Kf1;

/// <summary>
/// The keyboard layout the port ships, which is not the one RecompOne ships.
/// Verdite3's KeyLayout, with this game's actions.
///
///     KF1_KEYS=fps      force this layout on (the default for a fresh install)
///     KF1_KEYS=stock    leave RecompOne's Z X A S Q W E R F G alone
///
/// RecompOne's defaults are a *console's* defaults spelled on a keyboard — the
/// face buttons on Z X A S, the shoulders on Q W E R, the D-pad on the arrows.
/// That is the right generic answer for a machine that has to run any PS1 game,
/// and it is the wrong answer for a game whose D-pad walks *and turns*.
///
/// Two things about how this is applied are deliberate.
///
/// **A fresh install gets it as a default, not as an override.**
/// <see cref="Configure"/> runs from Program.cs, *before* ConfigManager.Load, and
/// Load writes the object it finds in memory when there is no settings.json to
/// read. So on a first run this is simply what the port's defaults are, and on
/// every run after it the player's own file wins.
///
/// **An existing install is migrated once, and only from stock.** Anyone who
/// already ran the port has a settings.json full of RecompOne's defaults, and a
/// default that only reaches new installs is not much of a default.
/// <see cref="Install"/> therefore rewrites those bindings once — but only if
/// they are *exactly* the stock ones, so a single key someone chose for
/// themselves stops it, and it records that it has run so that deliberately
/// going back to stock is not undone on the next launch.
///
/// The marker lives in interface.ini rather than in settings.json, because
/// settings.json is the thing being migrated and a marker inside it would need
/// the runtime's schema to grow a field.
/// </summary>
public static class KeyLayout
{
    /// <summary>Which version of the layout the config has been migrated to.</summary>
    public const string AppliedKey = "kf1.keys.layout";

    const int Version = 1;

    /// <summary>
    /// Layouts this port has shipped before and has since changed its mind about.
    ///
    /// The migration only rewrites bindings it recognises — stock, or one of
    /// these — because anything else is a choice someone made. That means a change
    /// to <see cref="Layout"/> after release reaches nobody unless the layout it
    /// replaces is recorded here and <see cref="Version"/> is bumped: without both,
    /// an existing config reads as customised and is left alone forever.
    /// </summary>
    static readonly KeyBindings[] Superseded = [];

    /// <summary>
    /// W A S D and the rest. Only the sixteen pad buttons exist, so this says
    /// which *key* presses each one; what the button then does is the game's own
    /// control configuration, exactly as it is for a pad.
    /// </summary>
    // The actions are stage A's own pad tests: see "The pad" in docs/INPUT.md.
    public static KeyBindings Layout() => new()
    {
        // Move. The strafes are on L1 and R1 in this game, which is what lets A
        // and D strafe rather than turn.
        Up = "W",
        Down = "S",
        L1 = "A",
        R1 = "D",

        // Turn. Left and Right stay on the arrows.
        Left = "Left",
        Right = "Right",

        // Pitch is the mouse's, and only the mouse's.
        L2 = "",
        R2 = "",

        // Act: attack, examine, the held item or spell, the menu, Start's message,
        // and Select, which the game reads as the menu too.
        Triangle = "Space",
        Circle = "F",
        Square = "Q",
        Cross = "Tab",
        Start = "Enter",
        Select = "ShiftRight",

        // The game reads neither.
        L3 = "",
        R3 = "",
    };

    /// <summary>
    /// Install this as the port's default bindings. **Must be called before
    /// ConfigManager.Load**, i.e. from Program.cs: Load either overwrites this
    /// object from settings.json or, when there is no such file, saves it — which
    /// is precisely the behaviour a default wants.
    /// </summary>
    public static void Configure() =>
        KeyLayoutApply.Configure(Layout, Version, Superseded, Announce, GetApplied, SetApplied);

    const string Announce = "WASD layout applied (W/S walk, A/D strafe, arrows turn, " +
                            "Space attack, F examine, Q item or magic, Tab menu). Input settings has both layouts.";

    /// <summary>
    /// Migrate an existing settings.json, once, and only if nothing in it was
    /// chosen by hand.
    /// </summary>
    public static void Install() => KeyLayoutApply.Install();

    /// <summary>Write the layout and save it. What a settings button calls.</summary>
    public static void Apply() => KeyLayoutApply.Apply();

    /// <summary>Back to RecompOne's own scheme, and remember that it was asked
    /// for, so the migration above does not undo it on the next launch.</summary>
    public static void ApplyStock() => KeyLayoutApply.ApplyStock();

    public static bool IsApplied() => KeyLayoutApply.IsApplied();

    static int GetApplied() => Rt.View.GetInt(AppliedKey, 0);

    static void SetApplied(int version)
    {
        Rt.View.SetInt(AppliedKey, version);
        Rt.SaveView();
    }
}
