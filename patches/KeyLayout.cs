using RecompOne.Runtime.Config;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hardware;
using RecompOne.Runtime.Host;
using Silk.NET.Input;

namespace Kf2;

/// <summary>
/// The keyboard layout the port ships, which is not the one RecompOne ships.
///
///     KF2_KEYS=fps      force this layout on (the default for a fresh install)
///     KF2_KEYS=stock    leave RecompOne's Z X A S Q W E R F G alone
///
/// RecompOne's defaults are a *console's* defaults spelled on a keyboard — the
/// face buttons on Z X A S, the shoulders on Q W E R, the D-pad on the arrows.
/// That is the right generic answer for a machine that has to run any PS1 game,
/// and it is the wrong answer for this one, because the D-pad in King's Field
/// walks *and turns*: the arrows alone are a tank control, and a mouse in the
/// other hand has nothing sensible to do.
///
/// Read through the game's own action-mask table (see "The action-mask table" in
/// NOTES.md), what the pad buttons mean here is:
///
///     Up / Down     walk forward / back        L1 / R1   strafe left / right
///     Left / Right  turn                       L2 / R2   look up / down
///     Square attack                            Circle    the in-game menu
///     Cross  the action button: use, open      Triangle  cast
///
/// which rearranges into the layout every first-person game has used since
/// Quake:
///
///     W A S D   walk and strafe          mouse   turn and look
///     arrows    walk and turn
///     Space     attack        F  use     Q  cast     Tab  menu
///     mouse     left attacks, right casts, middle uses
///
/// The arrows keep walking as well as turning, and that is not nostalgia: the
/// game's menus are navigated with the same D-pad bits, so an arrow key that no
/// longer presses Up would leave the in-game menu going up and down on W and S.
/// The runtime's binding table holds one key per button, so the second binding
/// cannot live there — see <see cref="Extras"/>.
///
/// Two things about how this is applied are deliberate.
///
/// **A fresh install gets it as a default, not as an override.**
/// <see cref="Configure"/> runs from Program.cs, *before* ConfigManager.Load, and
/// Load writes the object it finds in memory when there is no settings.json to
/// read. So on a first run this is simply what the port's defaults are, and on
/// every run after it the player's own file wins — the same relationship
/// RecompOne's defaults have with it.
///
/// **An existing install is migrated once, and only from stock.** Anyone who
/// already ran the port has a settings.json full of RecompOne's defaults, and a
/// default that only reaches new installs is not much of a default.
/// <see cref="Install"/> therefore rewrites those bindings once — but only if
/// they are *exactly* the stock ones, so a single key someone chose for
/// themselves stops it, and it records that it has run so that deliberately
/// going back to stock is not undone on the next launch.
///
/// The runtime's own "Reset to defaults" button under Input still resets to
/// RecompOne's scheme; <see cref="Kf2.Settings.KeyLayoutPage"/> is the button
/// that puts this one back.
///
/// See "The keyboard layout" in NOTES.md.
/// </summary>
public static class KeyLayout
{
    /// <summary>Which version of the layout the config has been migrated to. Kept
    /// in interface.ini rather than in settings.json, because settings.json is the
    /// thing being migrated and a marker inside it would need the runtime's
    /// schema to grow a field.</summary>
    public const string AppliedKey = "kf2.keys.layout";

    const int Version = 2;

    /// <summary>
    /// Layouts this port has shipped before and has since changed its mind about.
    ///
    /// The migration below only rewrites bindings it recognises — stock, or one of
    /// these — because anything else is a choice someone made. That means a change
    /// to <see cref="Layout()"/> after release reaches nobody unless the layout it
    /// replaces is recorded here and <see cref="Version"/> is bumped: without both,
    /// an existing config reads as customised and is left alone forever.
    ///
    /// Version 1 is here because it shipped with attack and use the wrong way
    /// round — Space on Cross, F on Square. The static read of `func_8002957C`
    /// named the buttons correctly and then guessed at what their branches did;
    /// playing it settled the opposite, which is the same lesson the analog patch
    /// learned on the pitch sign.
    /// </summary>
    static readonly KeyBindings[] Superseded =
    [
        new()
        {
            Up = "W", Down = "S", L1 = "A", R1 = "D",
            Left = "Left", Right = "Right", L2 = "R", R2 = "F",
            Cross = "Space", Square = "E", Triangle = "Q", Circle = "Tab",
            Select = "ShiftRight", Start = "Enter", L3 = "", R3 = "",
        },
    ];

    /// <summary>Off means the player asked for RecompOne's own scheme with
    /// KF2_KEYS=stock; nothing is written in that case.</summary>
    static bool _wanted = true;
    static bool _forced;

    /// <summary>
    /// W A S D and the rest. Only the sixteen pad buttons exist, so this says
    /// which *key* presses each one; what the button then does is the game's own
    /// control configuration, exactly as it is for a pad.
    /// </summary>
    public static KeyBindings Layout() => new()
    {
        // Move. The strafes are on the shoulder buttons in this game, which is
        // what lets A and D strafe rather than turn.
        Up = "W",
        Down = "S",
        L1 = "A",
        R1 = "D",

        // Turn. Left and Right stay on the arrows, where they have always been,
        // and the arrows go on walking too (see Extras).
        Left = "Left",
        Right = "Right",

        // Pitch is the mouse's, and only the mouse's. A keyboard pair for it
        // exists -- the game looks up and down on L2/R2 and a pad still does --
        // but two more keys to learn buy a worse version of something the mouse
        // does continuously, so the keyboard does not carry them.
        L2 = "",
        R2 = "",

        // Act. Square swings, so it gets the thumb; Cross is the action button --
        // doors, levers, the things in front of you -- so it gets F, where thirty
        // years of first-person games have put "use". Q casts.
        Square = "Space",
        Cross = "F",
        Triangle = "Q",
        Circle = "Tab",
        Select = "ShiftRight",
        Start = "Enter",

        // The game reads neither.
        L3 = "",
        R3 = "",
    };

    /// <summary>
    /// The bindings the runtime's table cannot hold: a *second* key for a button
    /// that already has one.
    ///
    /// <c>KeyBindings</c> is one key per button — a string, not a list — so
    /// binding W to Up takes the up arrow off it, and the up arrow is how the
    /// in-game menu moves. The port adds them back the same way the mouse buttons
    /// arrive: by ORing them into the word inside PAD_dr, where a bit is a bit and
    /// nothing asks which device set it.
    /// </summary>
    static readonly (Key Key, ushort Bit)[] Extras =
    [
        (Key.Up, Controller.Up),
        (Key.Down, Controller.Down),
    ];

    // Recomputing this on every PAD_dr would mean polling the keyboard hundreds of
    // thousands of times a second, so it is refreshed at most once a millisecond
    // -- still four times finer than the pad buffer the BIOS fills on hardware --
    // and every call in between ANDs the cached mask.
    static ushort _extra;
    static long _extraAt;

    static readonly Action<PadReadEvent> _secondary = e =>
    {
        if (e.Port != 0) return;

        long now = Environment.TickCount64;
        if (now != _extraAt)
        {
            _extraAt = now;
            _extra = 0;

            // Only while the port's own layout is the one in place: a player who
            // went back to stock, or who bound the arrows to something else
            // themselves, has said what those keys do.
            if (IsApplied())
                foreach (var (key, bit) in Extras)
                    if (HostWindow.IsKeyDown(key)) _extra |= bit;
        }

        if (_extra == 0) return;
        e.Buttons &= (ushort)~(ushort)((_extra >> 8) | (_extra << 8));
    };

    /// <summary>
    /// Install this as the port's default bindings. **Must be called before
    /// ConfigManager.Load**, i.e. from Program.cs: Load either overwrites this
    /// object from settings.json or, when there is no such file, saves it — which
    /// is precisely the behaviour a default wants.
    /// </summary>
    public static void Configure()
    {
        string? v = Environment.GetEnvironmentVariable("KF2_KEYS");
        if (!string.IsNullOrWhiteSpace(v))
        {
            _forced = true;
            _wanted = v.Trim().ToLowerInvariant() switch
            {
                "fps" or "wasd" or "1" or "on" => true,
                "stock" or "recompone" or "0" or "off" => false,
                _ => throw new ArgumentException($"KF2_KEYS: expected fps or stock, got '{v}'"),
            };
        }

        // Only a default: when settings.json already exists it holds the
        // player's bindings. Under Verdite2.Launcher the runtime is initialised --
        // and settings.json loaded -- before this runs, and the runtime does not
        // load it again, so writing the layout unconditionally replaced saved
        // bindings on every launch, and the close saved the replacement.
        if (_wanted && !System.IO.File.Exists("settings.json")) ConfigManager.Game.Keys = Layout();
    }

    /// <summary>
    /// Migrate an existing settings.json, once, and only if nothing in it was
    /// chosen by hand.
    /// </summary>
    public static void Install()
    {
        if (_wanted) Event.AddListener(_secondary);

        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            if (!_wanted) return;

            bool applied = Settings.PatchSettings.Get(AppliedKey, 0) >= Version;
            var keys = ConfigManager.Game.Keys;

            // Already this layout: a fresh install, where Configure's defaults are
            // what Load saved. Record it so a later return to stock stands.
            if (Matches(keys, Layout()))
            {
                if (!applied) Settings.PatchSettings.Set(AppliedKey, Version);
                return;
            }

            // KF2_KEYS=fps is an instruction, so it overrides both the marker and
            // a customised file for the run it is set in.
            bool recognised = Matches(keys, new KeyBindings()) ||
                              Superseded.Any(old => Matches(keys, old));
            if (!_forced && (applied || !recognised)) return;

            Apply();
            Console.WriteLine("[KF2] keys: WASD layout applied " +
                              "(W/S walk, A/D strafe, arrows walk and turn, " +
                              "Space attack, F use, Q cast, Tab menu). " +
                              "Input settings has both layouts.");
        });
    }

    /// <summary>Write the layout and save it. What the settings button calls.</summary>
    public static void Apply()
    {
        ConfigManager.Game.Keys = Layout();
        ConfigManager.SaveGame();
        Settings.PatchSettings.Set(AppliedKey, Version);
    }

    /// <summary>Back to RecompOne's own scheme, and remember that it was asked
    /// for, so the migration above does not undo it on the next launch.</summary>
    public static void ApplyStock()
    {
        ConfigManager.Game.Keys = new KeyBindings();
        ConfigManager.SaveGame();
        Settings.PatchSettings.Set(AppliedKey, Version);
    }

    public static bool IsApplied() => Matches(ConfigManager.Game.Keys, Layout());

    static bool Matches(KeyBindings a, KeyBindings b) =>
        a.Cross == b.Cross && a.Circle == b.Circle && a.Square == b.Square &&
        a.Triangle == b.Triangle && a.L1 == b.L1 && a.R1 == b.R1 &&
        a.L2 == b.L2 && a.R2 == b.R2 && a.L3 == b.L3 && a.R3 == b.R3 &&
        a.Start == b.Start && a.Select == b.Select &&
        a.Up == b.Up && a.Down == b.Down && a.Left == b.Left && a.Right == b.Right;
}
