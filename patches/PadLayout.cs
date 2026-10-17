using RecompOne.Runtime.Config;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Host;
using Silk.NET.Input;
using Rt = RecompOne.Runtime.Runtime;

namespace Kf1;

/// <summary>
/// The controller's face buttons, in the Western King's Field layout or this disc's own.
///
///     KF1_PAD_LAYOUT=west   Square attacks, Cross examines and confirms, Circle opens
///                           the menu and backs out, Triangle uses the item or spell
///                           (the default)
///     KF1_PAD_LAYOUT=jp     the game's own: Triangle, Circle, Cross, Square
///
/// The Western layout is the US King's Field's and King's Field II's New Game
/// default (Verdite2's mask table, Verdite3's preset 3). Applied in PAD_dr to the
/// controller's buttons only, so every overlay and menu sees it. The keyboard layout
/// binds keys to the game's own buttons, and the mouse buttons and the command
/// channel press them, so they are left as they are. See "The button layout" in
/// docs/INPUT.md.
/// </summary>
public static class PadLayout
{
    public const string Key = "kf1.pad.layout";

    public static bool Western = true;

    // PAD_dr's buffer is active-low with its two button bytes swapped against
    // Controller's layout: the faces are 0x10 Triangle, 0x20 Circle, 0x40 Cross, 0x80 Square.
    const ushort Triangle = 0x10, Circle = 0x20, Cross = 0x40, Square = 0x80;
    const ushort Faces = Triangle | Circle | Cross | Square;

    public static void Configure(string? mode)
    {
        if (Parse(mode) is { } w) Western = w;
    }

    static bool? Parse(string? mode) => mode?.Trim().ToLowerInvariant() switch
    {
        "west" or "us" or "eu" or "1" => true,
        "jp" or "japan" or "0" => false,
        _ => null,
    };

    /// <summary>Before any listener that presses buttons of its own: the command
    /// channel, the auto-start, the mouse.</summary>
    public static void Install()
    {
        Event.AddListener<PadReadEvent>(e =>
        {
            if (e.Port != 0) return;
            ushort pad = AgentServer.PadBits;
            if (pad != 0) e.Buttons &= (ushort)~(ushort)((pad >> 8) | (pad << 8));
            if (!Western) return;
            ushort held = (ushort)(~e.Buttons & Faces & ~Keyboard());
            if (held == 0) return;
            e.Buttons |= held;
            e.Buttons &= (ushort)~Remap(held);
        });
        Console.WriteLine($"[KF1] pad layout: {(Western ? "west" : "jp")}");
    }

    /// <summary>The kept choice, unless KF1_PAD_LAYOUT set one.</summary>
    public static void Ready()
    {
        if (!string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable("KF1_PAD_LAYOUT"))) return;
        if (Rt.View.GetInt(Key, -1) is >= 0 and var v) Western = v != 0;
    }

    /// <summary>A controller's button, as it sits on the pad, to the game's button for that verb.</summary>
    static ushort Remap(ushort held)
    {
        ushort to = 0;
        if ((held & Square) != 0) to |= Triangle;   // attack
        if ((held & Cross) != 0) to |= Circle;      // examine, confirm
        if ((held & Circle) != 0) to |= Cross;      // the menu, back
        if ((held & Triangle) != 0) to |= Square;   // the item or spell
        return to;
    }

    // The face buttons the keyboard holds, which InputManager has already merged
    // into the word. Refreshed at most once a millisecond, as KeyLayoutApply does.
    static ushort _keys;
    static long _keysAt;

    static ushort Keyboard()
    {
        long now = Environment.TickCount64;
        if (now == _keysAt) return _keys;
        _keysAt = now;
        var k = ConfigManager.Game.Keys;
        _keys = (ushort)(Down(k.Triangle, Triangle) | Down(k.Circle, Circle) | Down(k.Cross, Cross) | Down(k.Square, Square));
        return _keys;
    }

    static ushort Down(string name, ushort bit) =>
        Enum.TryParse<Key>(name, out var key) && HostWindow.IsKeyDown(key) ? bit : (ushort)0;
}
