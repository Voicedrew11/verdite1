using RecompOne.Runtime.Config;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Host;

namespace Verdite.Core;

/// <summary>
/// The game stands still while its window does not have the desktop's focus, as a
/// desktop game does: click away and the world waits, and the sound goes quiet.
///
///     {Tag}_FOCUS_PAUSE=0     keep running in the background (on by default)
///
/// **Core decides when, the game decides how.** Holding a world still is the
/// game's own mechanism (a stage gate held shut, say), so the game asks
/// <see cref="Active"/> from wherever its pause is taken; core owns the setting,
/// the focus and the sound. The sound is the SPU's two gains set to nothing and
/// put back from the runtime's audio settings, so the volume a player chose is
/// never the thing that moves. A game whose pause cannot hold a screen (an intro
/// that is not its world) is still quiet there.
///
/// A run driven by a program (a command channel, scripted input) has no one to
/// give the window focus, so the game passes <c>driven</c> to
/// <see cref="Configure"/> and the pause is off unless the variable asks for it;
/// the saved key is not read for that run.
/// </summary>
public static class FocusPause
{
    public static string OnKey => Game.Id + ".focuspause.on";

    public static bool Enabled = true;

    static readonly HashSet<string> _fromEnv = [];
    static bool _away, _quiet;

    /// <summary>Whether the game should stand still now: on, and the window without
    /// focus. Read once a frame by the game's own pause.</summary>
    public static bool Active => Enabled && _away;

    public static void Configure(bool driven = false)
    {
        if (driven)
        {
            Enabled = false;
            _fromEnv.Add(OnKey);
        }
        Kept.Env(Game.EnvPrefix + "FOCUS_PAUSE", OnKey, ref Enabled, _fromEnv);
    }

    public static void Install()
    {
        Event.AddListener<RuntimeReadyEvent>(_ =>
        {
            Kept.Saved(OnKey, ref Enabled, _fromEnv);
            _away = !HostWindow.Focused;
            Quiet(Active);
        });
        HostWindow.FocusChanged += focused =>
        {
            _away = !focused;
            Quiet(Active);
            if (Enabled) Console.WriteLine($"[{Game.Tag}] focus {(focused ? "back: resumed" : "lost: paused")}");
        };
    }

    public static void SetEnabled(bool on)
    {
        Enabled = on;
        Quiet(Active);
    }

    /// <summary>Silence the SPU's voices and CD audio, or put back the runtime's own
    /// levels. Only a change is written, so a volume set while the game is heard is
    /// left alone.</summary>
    static void Quiet(bool on)
    {
        if (on == _quiet) return;
        var spu = RecompOne.Runtime.Runtime.Spu;
        if (spu == null) return;
        _quiet = on;
        var game = ConfigManager.Game;
        spu.VoiceGain = on ? 0f : game.SpuVolume;
        spu.XaGain = on ? 0f : game.XaVolume;
    }
}
