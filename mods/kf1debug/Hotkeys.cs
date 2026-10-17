// ModCompiler compiles mods with no implicit usings, so every namespace the
// file needs must be named here -- including System.
using System;
using System.Collections.Generic;
// HostWindow is in RecompOne.Runtime.Host, not .Host.Window -- the file lives in
// the Window folder but declares the parent namespace. ToastNotifications is in
// .Window.
using RecompOne.Runtime.Host;
using RecompOne.Runtime.Host.Window;
using Silk.NET.Input;

namespace Kf1.Mods.Debug;

/// <summary>
/// Keyboard hotkeys, and the pad button the game itself does not use, polled
/// once a tick from the stage-A hook and edge-detected here.
///
/// The port's shipped layout (patches/KeyLayout.cs) binds W A S D, the arrows,
/// Space (attack), F, Q, Tab, Enter and Right Shift, so the flight keys are
/// Page Up, Page Down and Left Shift, and V is film mode. The toggles are F2..F8,
/// which nothing in the port binds.
/// </summary>
internal static class Hotkeys
{
    internal static bool Enabled = true;

    internal static Key TogglePanel   = Key.F2;
    internal static Key ToggleNoclip  = Key.F3;
    internal static Key ToggleGodMode = Key.F4;
    internal static Key SaveBookmark  = Key.F5;
    internal static Key LoadBookmark  = Key.F6;
    internal static Key SnapToFloor   = Key.F7;
    internal static Key ReturnToEntry = Key.F8;
    internal static Key FilmMode      = Key.V;
    internal static Key FlyUp         = Key.PageUp;
    internal static Key FlyDown       = Key.PageDown;
    internal static Key FlyFastKey    = Key.ShiftLeft;

    // SDL's GameControllerButton indices, which HostWindow.IsPadButtonDown
    // takes. Misc1 is a pad's extra button (a DualSense's mute key), which
    // nothing in the game is bound to.
    internal const int PadMute        = 15;   // Misc1
    internal const int PadLeftStick   = 7;    // L3
    internal const int PadRightStick  = 8;    // R3
    internal const int PadLShoulder   = 9;    // L1
    internal const int PadRShoulder   = 10;   // R1

    static readonly HashSet<Key> _held = [];
    static readonly HashSet<int> _padHeld = [];

    /// <summary>
    /// True on the frame the key goes down, false while it stays down. Polled
    /// once a tick, so a tap shorter than a tick is missed.
    /// </summary>
    static bool Pressed(Key key)
    {
        if (!Down(key))
        {
            _held.Remove(key);
            return false;
        }
        return _held.Add(key);
    }

    internal static bool Down(Key key) => Enabled && HostWindow.IsKeyDown(key);

    /// <summary>The pad's own edge detect, the same shape as Pressed(Key).</summary>
    static bool PadPressed(int button)
    {
        if (!HostWindow.IsPadButtonDown(button))
        {
            _padHeld.Remove(button);
            return false;
        }
        return _padHeld.Add(button);
    }

    internal static bool PadDown(int button) => Enabled && HostWindow.IsPadButtonDown(button);

    internal static bool FlyFast() => Enabled && Down(FlyFastKey);

    /// <summary>
    /// +1 for up, -1 for down, 0 for neither.
    ///
    /// Keyboard only: the pad's R2/L2 are read in Noclip.
    /// </summary>
    internal static float FlyVertical()
    {
        if (!Enabled) return 0f;
        float v = 0f;
        if (Down(FlyUp)) v += 1f;
        if (Down(FlyDown)) v -= 1f;
        return v;
    }

    /// <summary>
    /// Run the toggles. Called once a tick from the mod's stage-A hook.
    /// </summary>
    internal static void Poll()
    {
        if (!Enabled) return;

        if (Pressed(TogglePanel))
            DebugPanel.Instance.IsOpen = !DebugPanel.Instance.IsOpen;

        if (Pressed(ToggleNoclip))
        {
            Noclip.Enabled = !Noclip.Enabled;
            Notify("Noclip", Noclip.Enabled);
        }

        if (Pressed(ToggleGodMode))
        {
            Cheats.Invincible = !Cheats.Invincible;
            Notify("Invincibility", Cheats.Invincible);
        }

        if (Pressed(SaveBookmark)) Warp.Save(0);
        if (Pressed(LoadBookmark)) Warp.Restore(0);
        if (Pressed(SnapToFloor)) Noclip.SnapToFloor();
        if (Pressed(ReturnToEntry)) Noclip.ReturnToEntry();

        // Film mode: all three on, or all three off when they already are. No
        // toast either way -- it is for filming, and one would land in the shot.
        // V, or the pad's spare button -- the DualSense's mute key -- does the same.
        if (Pressed(FilmMode) || PadPressed(PadMute))
        {
            bool on = !(Noclip.Cinematic && Cheats.Peaceful && Noclip.Enabled);
            Noclip.Cinematic = on;
            Cheats.Peaceful = on;
            Noclip.Enabled = on;
        }
    }

    /// <summary>
    /// A toast, so a hotkey press is visible without opening the panel. There is
    /// no way to draw text into the emulated 320x240 picture short of building
    /// GPU primitives, so this sits on top of it instead.
    /// </summary>
    static void Notify(string what, bool on)
    {
        // The cinematic camera is for filming a flythrough: nothing may fade in
        // over the picture while it is on, and that includes these.
        if (Noclip.Cinematic) return;
        ToastNotifications.ShowText("Debug Tools", $"{what} {(on ? "on" : "off")}");
    }

    internal static void Reset()
    {
        _held.Clear();
        _padHeld.Clear();
    }
}
