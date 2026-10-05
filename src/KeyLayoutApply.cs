using RecompOne.Runtime.Config;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hardware;
using RecompOne.Runtime.Host;
using Silk.NET.Input;

namespace Verdite.Core;

/// <summary>
/// The port's keyboard layout, applied as a default and migrated once. A game
/// supplies its table, version, superseded layouts, the line announcing it and
/// where the applied version is kept through
/// <see cref="Configure"/>; the migration reasoning lives in the game's docs.
///
///     {Tag}_KEYS=fps      force this layout on (the default for a fresh install)
///     {Tag}_KEYS=stock    leave RecompOne's Z X A S Q W E R F G alone
/// </summary>
public static class KeyLayoutApply
{
    static Func<KeyBindings> _layout = () => new();
    static KeyBindings[] _superseded = [];
    static int _version;
    static string _announce = "";
    static Func<int> _getApplied = () => 0;
    static Action<int> _setApplied = _ => { };

    /// <summary>Off means the player asked for RecompOne's own scheme with
    /// {Tag}_KEYS=stock; nothing is written in that case.</summary>
    static bool _wanted = true;
    static bool _forced;

    /// <summary>
    /// Install this as the port's default bindings. **Must be called before
    /// ConfigManager.Load**: Load either overwrites the defaults from
    /// settings.json or, when there is no such file, saves them.
    /// </summary>
    public static void Configure(
        Func<KeyBindings> layout,
        int version,
        KeyBindings[] superseded,
        string announce,
        Func<int> getApplied,
        Action<int> setApplied)
    {
        _layout = layout;
        _version = version;
        _superseded = superseded;
        _announce = announce;
        _getApplied = getApplied;
        _setApplied = setApplied;

        string? v = Game.Env("KEYS");
        if (!string.IsNullOrWhiteSpace(v))
        {
            _forced = true;
            _wanted = v.Trim().ToLowerInvariant() switch
            {
                "fps" or "wasd" or "1" or "on" => true,
                "stock" or "recompone" or "0" or "off" => false,
                _ => throw new ArgumentException($"{Game.EnvPrefix}KEYS: expected fps or stock, got '{v}'"),
            };
        }

        // Only a default: when settings.json already exists it holds the
        // player's bindings, so writing the layout here would replace them.
        if (_wanted && !System.IO.File.Exists("settings.json")) ConfigManager.Game.Keys = layout();
    }

    /// <summary>Register the second-key listener and the one-shot migration.</summary>
    public static void Install()
    {
        if (_wanted) Event.AddListener(_secondary);

        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            if (!_wanted) return;

            bool applied = _getApplied() >= _version;
            var keys = ConfigManager.Game.Keys;

            // Already this layout: a fresh install, where Configure's defaults
            // are what Load saved. Record it so a later return to stock stands.
            if (Matches(keys, _layout()))
            {
                if (!applied) _setApplied(_version);
                return;
            }

            // {Tag}_KEYS=fps is an instruction, so it overrides both the marker
            // and a customised file for the run it is set in.
            bool recognised = Matches(keys, new KeyBindings()) ||
                              _superseded.Any(old => Matches(keys, old));
            if (!_forced && (applied || !recognised)) return;

            Apply();
            Console.WriteLine($"[{Game.Tag}] keys: {_announce}");
        });
    }

    /// <summary>Write the layout and save it. What a settings button calls.</summary>
    public static void Apply()
    {
        ConfigManager.Game.Keys = _layout();
        ConfigManager.SaveGame();
        _setApplied(_version);
    }

    /// <summary>Back to RecompOne's own scheme, and remember that it was asked
    /// for, so the migration above does not undo it on the next launch.</summary>
    public static void ApplyStock()
    {
        ConfigManager.Game.Keys = new KeyBindings();
        ConfigManager.SaveGame();
        _setApplied(_version);
    }

    public static bool IsApplied() => Matches(ConfigManager.Game.Keys, _layout());

    /// <summary>
    /// The second key for a button the runtime's table cannot hold. Binding W to
    /// Up takes the up arrow off it, and the arrow is how the in-game menu moves,
    /// so the port clears the swapped bit inside PAD_dr instead.
    /// </summary>
    static readonly (Key Key, ushort Bit)[] Extras =
    [
        (Key.Up, Controller.Up),
        (Key.Down, Controller.Down),
    ];

    // Polling the keyboard on every PAD_dr is too much, so refresh at most once
    // a millisecond and AND the cached mask in between.
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

            // Only while the port's own layout is in place: a player who went
            // back to stock, or bound the arrows themselves, has said so.
            if (IsApplied())
                foreach (var (key, bit) in Extras)
                    if (HostWindow.IsKeyDown(key)) _extra |= bit;
        }

        if (_extra == 0) return;
        e.Buttons &= (ushort)~(ushort)((_extra >> 8) | (_extra << 8));
    };

    static bool Matches(KeyBindings a, KeyBindings b) =>
        a.Cross == b.Cross && a.Circle == b.Circle && a.Square == b.Square &&
        a.Triangle == b.Triangle && a.L1 == b.L1 && a.R1 == b.R1 &&
        a.L2 == b.L2 && a.R2 == b.R2 && a.L3 == b.L3 && a.R3 == b.R3 &&
        a.Start == b.Start && a.Select == b.Select &&
        a.Up == b.Up && a.Down == b.Down && a.Left == b.Left && a.Right == b.Right;
}
