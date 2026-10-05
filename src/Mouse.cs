using RecompOne.Runtime.Events;
using RecompOne.Runtime.Hardware;
using RecompOne.Runtime.Host;
using RecompOne.Runtime.Host.Window;
using RecompOne.Runtime.Memory;
using Silk.NET.Input;
// Both namespaces above define a MouseButton; the host's input one is the one
// HostWindow.IsMouseButtonDown takes.
using MouseButton = Silk.NET.Input.MouseButton;

namespace Verdite.Core;

/// <summary>
/// The game's half of <see cref="Mouse"/>: the values core may not know. The
/// angles and step cap are in the game's own units and addresses;
/// <paramref name="TextEditing"/> gates the capture key while a text field has
/// focus; <paramref name="Frames"/> and <paramref name="LogicHz"/> are the
/// frame clock the stale-motion test keys off; <paramref name="Paused"/> is a
/// game that drops motion while its world is stopped.
/// </summary>
public sealed record MouseGame(
    float UnitsPerDegree,
    float DegreesPerPixel,
    int StepCap,
    int PitchLimit,
    uint YawAddress,
    uint PitchAddress,
    int DefaultLeftButton,
    int DefaultRightButton,
    int DefaultMiddleButton,
    Func<bool> TextEditing,
    Func<long> Frames,
    Func<double> LogicHz,
    Func<bool>? Paused = null);

/// <summary>
/// Mouse look, and the mouse buttons, as a third source the game's own control
/// velocities can be driven from. The game hands its values to
/// <see cref="Configure"/>; the design reasoning lives in each game's docs.
///
///     {Tag}_MOUSE=1                            on; on by default
///     {Tag}_MOUSE_TURN=1.0 {Tag}_MOUSE_LOOK=1.0  sensitivities
///     {Tag}_MOUSE_INVERTY=1                    look-Y inversion
///     {Tag}_MOUSE_LEAD=0                       show motion when the tick spends it
///     {Tag}_MOUSE_BUTTONS=Triangle,Square,...  left, right, middle, as pad buttons
///     {Tag}_MOUSE_KEY=Escape                   the key that captures and releases
///
/// **The look half is not its own hook.** The game's own look routine spends
/// <see cref="TakeLook"/> and reports what it asked for through
/// <see cref="NoteSpent"/>. The buttons ride the event the BIOS fires when the
/// game reads the pad, so a held button is ORed into the word the game is about
/// to read. Capture is what makes motion mean anything: the pointer is locked to
/// the window with <see cref="CaptureKey"/>, and any popup takes it back.
/// </summary>
public static class Mouse
{
    /// <summary>Motion older than this is thrown away rather than applied: the
    /// accumulator fills while the pointer moves, but the routine that spends it
    /// only runs while the game is walking around.</summary>
    const long StaleMs = 250;

    static MouseGame _game = null!;

    // ---- settings ---------------------------------------------------------------
    //
    // Plain public fields: every one of them is a widget's `ref` argument.

    public static string OnKey        => Game.Id + ".mouse.on";
    public static string TurnKey      => Game.Id + ".mouse.turn";
    public static string LookKey      => Game.Id + ".mouse.look";
    public static string InvertKey    => Game.Id + ".mouse.inverty";
    public static string LeftKey      => Game.Id + ".mouse.left";
    public static string RightKey     => Game.Id + ".mouse.right";
    public static string MiddleKey    => Game.Id + ".mouse.middle";
    public static string CaptureKeyKey => Game.Id + ".mouse.capturekey";
    public static string LeadKey      => Game.Id + ".mouse.lead";

    /// <summary>On by default; capture is still Escape, so a pointer does not
    /// disappear into the game until the player asks.</summary>
    public static bool Enabled = true;

    /// <summary>Show mouse look the frame it happens rather than when the next
    /// tick spends it. On by default.</summary>
    public static bool Lead = true;

    public static float TurnSens = 1.0f;
    public static float LookSens = 1.0f;
    public static bool InvertY;

    /// <summary>Indices into <see cref="PadButtons"/>, in the order left, right,
    /// middle; the defaults are the game's.</summary>
    public static int LeftButton;
    public static int RightButton;
    public static int MiddleButton;

    /// <summary>
    /// What each mouse button presses, as a pad button rather than as an action.
    /// The names are the pad's, not the game's: this presses a button and the
    /// game's control-config screen decides what the button does.
    /// </summary>
    internal static readonly (string Name, ushort Bit)[] PadButtons =
    [
        ("None",     0),
        ("Cross",    Controller.Cross),
        ("Circle",   Controller.Circle),
        ("Square",   Controller.Square),
        ("Triangle", Controller.Triangle),
        ("L1",       Controller.L1),
        ("R1",       Controller.R1),
        ("L2",       Controller.L2),
        ("R2",       Controller.R2),
        ("Start",    Controller.Start),
        ("Select",   Controller.Select),
    ];

    /// <summary>The key that locks the pointer to the window and gives it back.
    /// Escape by default: a popup closes on Escape, so while one is open this
    /// leaves the key alone.</summary>
    public static Key CaptureKey = Key.Escape;

    /// <summary>The keys the settings page offers, since it has no key-capture
    /// widget of its own.</summary>
    internal static readonly Key[] CaptureKeys =
        [Key.Escape, Key.Tab, Key.GraveAccent, Key.F9, Key.F10, Key.F12];

    /// <summary>Whether the pointer is locked to the window right now. The host
    /// is the authority — a platform that refuses the mode leaves this false.</summary>
    public static bool Captured { get; private set; }

    /// <summary>The step cap and the pitch limit, in the game's units.</summary>
    internal static int StepCap => _game.StepCap;
    internal static int PitchLimit => _game.PitchLimit;

    /// <summary>The base pitch, for readers outside a hook (the view's lead).</summary>
    internal static uint PitchAddress => _game.PitchAddress;

    static long _taken;
    static long _checked;
    static bool _hinted;

    static readonly HashSet<string> _fromEnv = new(StringComparer.Ordinal);

    public static void Configure(MouseGame game)
    {
        _game = game;

        LeftButton = game.DefaultLeftButton;
        RightButton = game.DefaultRightButton;
        MiddleButton = game.DefaultMiddleButton;

        Kept.Env(Game.EnvPrefix + "MOUSE", OnKey, ref Enabled, _fromEnv);
        Kept.Env(Game.EnvPrefix + "MOUSE_TURN", TurnKey, ref TurnSens, _fromEnv);
        Kept.Env(Game.EnvPrefix + "MOUSE_LOOK", LookKey, ref LookSens, _fromEnv);
        Kept.Env(Game.EnvPrefix + "MOUSE_INVERTY", InvertKey, ref InvertY, _fromEnv);
        Kept.Env(Game.EnvPrefix + "MOUSE_LEAD", LeadKey, ref Lead, _fromEnv);

        // One variable for the three buttons rather than three: they are set
        // together or not at all.
        string? buttons = Game.Env("MOUSE_BUTTONS");
        if (!string.IsNullOrWhiteSpace(buttons))
        {
            var names = buttons.Split(',', StringSplitOptions.TrimEntries);
            for (int i = 0; i < names.Length && i < 3; i++)
            {
                int index = Array.FindIndex(PadButtons,
                    b => string.Equals(b.Name, names[i], StringComparison.OrdinalIgnoreCase));
                if (index < 0)
                    throw new ArgumentException(
                        $"{Game.EnvPrefix}MOUSE_BUTTONS: no pad button '{names[i]}'; one of " +
                        string.Join(", ", PadButtons.Select(b => b.Name)));

                if (i == 0) { LeftButton = index; _fromEnv.Add(LeftKey); }
                else if (i == 1) { RightButton = index; _fromEnv.Add(RightKey); }
                else { MiddleButton = index; _fromEnv.Add(MiddleKey); }
            }
        }

        string? key = Game.Env("MOUSE_KEY");
        if (!string.IsNullOrWhiteSpace(key))
        {
            if (!Enum.TryParse<Key>(key.Trim(), true, out var parsed))
                throw new ArgumentException($"{Game.EnvPrefix}MOUSE_KEY: no key '{key}' (a Silk.NET key name)");
            CaptureKey = parsed;
            _fromEnv.Add(CaptureKeyKey);
        }
    }

    /// <summary>
    /// Listeners, and no hook of its own: the look half is spent by the game's
    /// own look routine, and the button half rides on the bus the BIOS fires when
    /// the game reads the pad.
    /// </summary>
    public static void Install()
    {
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            // Registered here rather than by the game so it lands after the
            // runtime's own panels: PanelManager draws in registration order.
            PanelManager.Register(MouseIndicator.Instance);

            Kept.Saved(OnKey, ref Enabled, _fromEnv);
            Kept.Saved(TurnKey, ref TurnSens, _fromEnv);
            Kept.Saved(LookKey, ref LookSens, _fromEnv);
            Kept.Saved(InvertKey, ref InvertY, _fromEnv);
            Kept.Saved(LeadKey, ref Lead, _fromEnv);
            Kept.Saved(LeftKey, ref LeftButton, _fromEnv);
            Kept.Saved(RightKey, ref RightButton, _fromEnv);
            Kept.Saved(MiddleKey, ref MiddleButton, _fromEnv);

            int key = (int)CaptureKey;
            Kept.Saved(CaptureKeyKey, ref key, _fromEnv);
            if (Enum.IsDefined(typeof(Key), key)) CaptureKey = (Key)key;

            LeftButton = Clamp(LeftButton);
            RightButton = Clamp(RightButton);
            MiddleButton = Clamp(MiddleButton);

            if (Enabled)
                Console.WriteLine($"[{Game.Tag}] mouse: on, {CaptureKey} captures the pointer " +
                                  $"(turn x{TurnSens:0.##}, look x{LookSens:0.##}; " +
                                  $"{PadButtons[LeftButton].Name}/{PadButtons[RightButton].Name}/" +
                                  $"{PadButtons[MiddleButton].Name} on left/right/middle)");
        });

        // The capture key comes off the event bus rather than being polled, so a
        // pointer captured and then swallowed by the in-game menu can be released
        // from inside it.
        Event.AddListener<KeyboardEvent>(e =>
        {
            if (!e.Pressed || e.Key != (int)CaptureKey) return;
            if (!Enabled) return;

            // A popup is drawn over the game and closes on Escape itself, so the
            // key belongs to it while one is open. Releasing is still allowed.
            if (!Captured && PopupManager.AnyOpen) return;
            if (!Captured && TextEditing) return;

            SetCaptured(!Captured);
        });

        // The buttons are not listened for here. PAD_dr is the busiest call in
        // the game, so the listener is attached when the pointer is captured and
        // dropped when it is let go -- the window in which a button means anything.
    }

    /// <summary>Whether a text field has focus, so a letter hotkey waits.</summary>
    static bool TextEditing => _game.TextEditing();

    /// <summary>
    /// The mouse buttons, ORed into the word the game is about to read. Attached
    /// only while the pointer is captured (see <see cref="Install"/>).
    /// </summary>
    static readonly Action<PadReadEvent> _buttons = e =>
    {
        if (e.Port != 0 || !Enabled || !Captured) return;

        // A popup is drawn over a running game and wants a cursor, so opening one
        // takes the pointer back. This is the only state change nothing else can
        // announce, and PAD_dr is the one thing the game keeps doing wherever it is.
        Watch();
        if (!Captured) return;

        ushort press = 0;
        if (HostWindow.IsMouseButtonDown(MouseButton.Left)) press |= PadButtons[LeftButton].Bit;
        if (HostWindow.IsMouseButtonDown(MouseButton.Right)) press |= PadButtons[RightButton].Bit;
        if (HostWindow.IsMouseButtonDown(MouseButton.Middle)) press |= PadButtons[MiddleButton].Bit;
        if (press == 0) return;

        // The buffer PAD_dr fills is active low and carries the two button bytes
        // the other way round from Controller's layout, so clearing the swapped
        // bit is what the game reads back as "pressed".
        e.Buttons &= (ushort)~(ushort)((press >> 8) | (press << 8));
    };

    // Motion drained from the host but not yet spent by a tick, in game units.
    // Poll drains it every drawn frame so a lead can show it before the tick lands;
    // the look routine spends it (TakeLook).
    static float _pendTurn, _pendPitch;
    static long _polled;

    // What the last tick asked for, and the base angles before the game applied
    // it, so a lead can measure what the game actually turned by.
    static long _takenFrame = -1;
    static float _spentTurn, _spentPitch;
    static float _shareTurn, _sharePitch;
    static ushort _yawBefore, _pitchBefore;

    /// <summary>
    /// Drain the host's motion into the pending sum. Called every drawn frame from
    /// the stage that carries the view, and again by <see cref="TakeLook"/>. Motion
    /// is dropped rather than kept while the look routine is not running.
    /// </summary>
    internal static void Poll()
    {
        // Pumped here, so every frame's motion is current to this point; only
        // while captured, so it costs nothing otherwise.
        if (Enabled && Captured) HostWindow.PumpInput(0.0);

        long now = Environment.TickCount64;
        var (dx, dy) = HostWindow.TakeMouseMotion();
        long gap = now - _polled;
        _polled = now;

        if (!Enabled || !Captured || gap > StaleMs || !Live(now))
        {
            _pendTurn = _pendPitch = 0f;
            return;
        }

        float units = DegreesPerPixel * UnitsPerDegree;
        _pendTurn += -dx * units * TurnSens;
        _pendPitch += dy * units * LookSens * (InvertY ? -1f : 1f);
    }

    /// <summary>Whether the look routine has spent the mouse recently enough that
    /// motion now will be spent too: three ticks, or <see cref="StaleMs"/> if that
    /// is shorter.</summary>
    static bool Live(long now) =>
        _game.Paused?.Invoke() != true &&
        now - _taken <= Math.Min(StaleMs, (long)(3000.0 / Math.Max(1.0, _game.LogicHz())));

    /// <summary>
    /// Motion drawn but not yet spent, capped as the look routine will cap it:
    /// what the next tick will turn by. Zero unless the look routine is live.
    /// </summary>
    internal static (float Turn, float Pitch) Pending
    {
        get
        {
            if (!Enabled || !Captured || !Live(Environment.TickCount64)) return (0f, 0f);
            return (Math.Clamp(_pendTurn, -StepCap, StepCap),
                    Math.Clamp(_pendPitch, -StepCap, StepCap));
        }
    }

    /// <summary>
    /// The motion since the last tick, in the game's own angle units and sign
    /// convention. Both zero unless the pointer is captured.
    ///
    /// Called once a tick from the game's look hook, and it empties the pending
    /// sum whether or not it is going to use it — motion collected while the mouse
    /// was doing something else is not a turn anybody asked for.
    /// </summary>
    internal static (float Turn, float Pitch) TakeLook()
    {
        if (!Captured && Enabled && !_hinted)
        {
            // Said once, and here rather than at boot: reaching this means the
            // player is walking around with mouse look on and a pointer that is
            // still a pointer.
            _hinted = true;
            MouseIndicator.Show(false);
        }

        Poll();
        var take = (_pendTurn, _pendPitch);
        _pendTurn = _pendPitch = 0f;
        _taken = Environment.TickCount64;
        _takenFrame = _game.Frames();
        _spentTurn = _spentPitch = 0f;
        _shareTurn = _sharePitch = 0f;
        return take;
    }

    /// <summary>
    /// What the game's look hook asked the game for this tick: the base angles
    /// before, and the mouse's share of each axis's step. The rest was a stick's.
    /// </summary>
    internal static void NoteSpent(IMemory m, float turn, float stickTurn, float pitch, float stickPitch)
    {
        _yawBefore = m.ReadU16(_game.YawAddress);
        _pitchBefore = m.ReadU16(_game.PitchAddress);
        _spentTurn = turn;
        _spentPitch = pitch;
        _shareTurn = Share(turn, stickTurn);
        _sharePitch = Share(pitch, stickPitch);
    }

    static int Delta12(ushort to, ushort from)
    {
        int d = (to - from) & 0xFFF;
        return d >= 0x800 ? d - 0x1000 : d;
    }

    static float Share(float mouse, float stick) =>
        mouse == 0f ? 0f : stick == 0f ? 1f : Math.Clamp(mouse / (mouse + stick), 0f, 1f);

    /// <summary>
    /// What the game turned by for the mouse on the tick spent in frame
    /// <see cref="MouseGame.Frames"/>, measured off the base angles rather than
    /// assumed, so the pitch limit and anything else the game did are in it. Null
    /// when the look routine did not spend the mouse on this frame. Also returns
    /// what was asked for, which a probe can compare.
    /// </summary>
    internal static (int Yaw, int Pitch, float AskedYaw, float AskedPitch)? SpentThisFrame(IMemory m)
    {
        if (_takenFrame != _game.Frames() || (_shareTurn == 0f && _sharePitch == 0f)) return null;

        // Both are 12-bit angles, pitch included: the look routine stores it
        // `& 0xFFF`, so looking just above level reads 0x0FFx.
        int dYaw = Delta12(m.ReadU16(_game.YawAddress), _yawBefore);
        int dPitch = Delta12(m.ReadU16(_game.PitchAddress), _pitchBefore);

        return ((int)MathF.Round(dYaw * _shareTurn), (int)MathF.Round(dPitch * _sharePitch),
                _spentTurn, _spentPitch);
    }

    /// <summary>
    /// Lock or release, and say so on screen through <see cref="MouseIndicator"/>.
    /// Reads the host back rather than trusting the write: no mouse, or a platform
    /// without the cursor mode, and the answer is no.
    /// </summary>
    public static void SetCaptured(bool on)
    {
        if (on == Captured) return;

        HostWindow.MouseCaptured = on;
        Captured = HostWindow.MouseCaptured;

        if (Captured) Event.AddListener(_buttons);
        else Event.RemoveListener(_buttons);

        if (on && !Captured)
        {
            Console.Error.WriteLine($"[{Game.Tag}] mouse: the host will not lock the pointer; mouse look is inert");
            ToastNotifications.ShowText("Mouse look", "This display cannot lock the pointer");
            return;
        }

        // Taking the pointer back has to clear the accumulator too: the jump from
        // wherever the cursor was left is not motion.
        HostWindow.TakeMouseMotion();
        _pendTurn = _pendPitch = 0f;
        _taken = _polled = Environment.TickCount64;

        MouseIndicator.Show(Captured);
    }

    /// <summary>
    /// The one state change nothing announces: a popup opening while the pointer
    /// is captured. Throttled to once a millisecond, because its caller is PAD_dr.
    /// </summary>
    static void Watch()
    {
        long now = Environment.TickCount64;
        if (now == _checked) return;
        _checked = now;

        if (Captured && PopupManager.AnyOpen) SetCaptured(false);
    }

    static int Clamp(int index) => index < 0 || index >= PadButtons.Length ? 0 : index;

    static float UnitsPerDegree => _game.UnitsPerDegree;
    static float DegreesPerPixel => _game.DegreesPerPixel;
}
