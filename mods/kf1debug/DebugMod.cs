// ModCompiler compiles mods with no implicit usings, so every namespace the
// file needs must be named here -- including System.
using System;
using ImGuiNET;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Host.Window;
using RecompOne.Runtime.Memory;
using RecompOne.Runtime.Modding;

namespace Kf1.Mods.Debug;

/// <summary>
/// Debug tools: noclip flight, invincibility, infinite MP, enemies that ignore
/// you, a speed multiplier, position bookmarks, area warp, editors for the
/// character, the inventory and equipment and the spell book, with a live
/// readout of the player state.
///
/// Verdite3's mods/kf3debug, ported: the structure and conventions are its, every
/// address is this disc's (docs/GAME_INTERNALS.md, docs/MODS.md). Everything is
/// off until it is turned on, and nothing acts without a character in an area.
/// </summary>
public sealed class DebugMod : IMod
{
    const string NoclipSpeedKey = "kf1.debug.noclip.speed";
    const string NoclipFastKey  = "kf1.debug.noclip.fast";
    const string InvertYKey     = "kf1.debug.noclip.inverty";
    const string InvertPitchKey = "kf1.debug.noclip.invertpitch";
    const string InvertSKey     = "kf1.debug.noclip.invertstrafe";
    const string SpeedMultKey   = "kf1.debug.speed.multiplier";
    internal const string FlyAfterWarpKey = "kf1.debug.warp.fly";
    const string HotkeysKey     = "kf1.debug.hotkeys";

    // The panel has to be registered from the UI thread; OnLoad can run on a
    // worker at first boot, so registration waits for the first frame.
    static bool _panelRegistered;
    static Action<VSyncEvent>? _onVSync;

    public void OnLoad()
    {
        var view = RecompOne.Runtime.Runtime.View;

        Noclip.Speed           = view.GetFloat(NoclipSpeedKey, 6000f);
        Noclip.FastMultiplier  = view.GetFloat(NoclipFastKey, 4f);
        Noclip.InvertVertical  = view.GetBool(InvertYKey, false);
        Noclip.InvertPitch     = view.GetBool(InvertPitchKey, false);
        Noclip.InvertStrafe    = view.GetBool(InvertSKey, false);
        Cheats.SpeedMultiplier = view.GetFloat(SpeedMultKey, 2f);
        Hotkeys.Enabled        = view.GetBool(HotkeysKey, true);
        Warp.FlyAfterWarp      = view.GetBool(FlyAfterWarpKey, true);

        ReadEnv("KF1_DEBUG_NOCLIP", ref Noclip.Enabled);
        ReadEnv("KF1_DEBUG_GODMODE", ref Cheats.Invincible);
        ReadEnv("KF1_DEBUG_INFINITEMP", ref Cheats.InfiniteMp);
        ReadEnv("KF1_DEBUG_PEACEFUL", ref Cheats.Peaceful);
        ReadEnv("KF1_DEBUG_HOTKEYS", ref Hotkeys.Enabled);
        ReadEnv("KF1_DEBUG_NOCLIP_SPEED", ref Noclip.Speed);
        ReadEnv("KF1_DEBUG_SPEED", ref Cheats.SpeedMultiplier);
        if (Environment.GetEnvironmentVariable("KF1_DEBUG_SPEED") != null) Cheats.SpeedEnabled = true;

        Warp.LoadPersisted();

        _onVSync = _ => RegisterPanel();
        Event.AddListener(_onVSync);

        Console.WriteLine("[kf1debug] loaded; F2 opens the panel, F3 noclip, F4 invincibility");
    }

    public void OnUnload()
    {
        if (_onVSync != null)
        {
            Event.RemoveListener(_onVSync);
            _onVSync = null;
        }

        Noclip.Reset();
        Cheats.Reset();
        Hotkeys.Reset();
        Warp.Reset();
        Attributes.Reset();
        Items.Reset();
        Magic.Reset();

        DebugPanel.Instance.IsOpen = false;
        _panelRegistered = false;

        Console.WriteLine("[kf1debug] unloaded");
    }

    static void RegisterPanel()
    {
        if (_panelRegistered) return;
        _panelRegistered = true;

        foreach (var p in PanelManager.Panels)
            if (ReferenceEquals(p, DebugPanel.Instance)) return;

        PanelManager.Register(DebugPanel.Instance);
        MenuRegistry.Menu("menu.debug")
                    .Panel<DebugPanel>("KF1 Debug")
                    .End();
    }

    /// <summary>One poll a tick for the hotkeys, at the end of stage A.</summary>
    [PostHook("game", Address = GameState.PlayerStage)]
    static void AfterPlayerStage(CpuContext c, IMemory m) => Hotkeys.Poll();

    public void DrawSettings()
    {
        ImGui.TextWrapped("Noclip flight, invincibility, infinite MP, enemies that ignore you, a "
                        + "speed multiplier, position bookmarks, area warp and editors for the "
                        + "character, the inventory and equipment and the spell book, plus a live "
                        + "readout of the player state. Everything is off until you switch it on.");
        ImGui.Separator();

        if (ImGui.Button("Open the debug panel"))
        {
            RegisterPanel();
            DebugPanel.Instance.IsOpen = true;
        }
        ImGui.SameLine();
        ImGui.TextDisabled("or press F2");

        ImGui.Separator();

        if (ImGui.SliderFloat("Noclip speed", ref Noclip.Speed, 200f, 30000f, "%.0f"))
            Persist(NoclipSpeedKey, Noclip.Speed);
        if (ImGui.SliderFloat("Noclip fast multiplier", ref Noclip.FastMultiplier, 1f, 10f, "x%.1f"))
            Persist(NoclipFastKey, Noclip.FastMultiplier);
        if (ImGui.SliderFloat("Speed multiplier", ref Cheats.SpeedMultiplier, 0.25f, 8f, "x%.2f"))
            Persist(SpeedMultKey, Cheats.SpeedMultiplier);

        bool invY = Noclip.InvertVertical;
        if (ImGui.Checkbox("Invert noclip up/down", ref invY)) { Noclip.InvertVertical = invY; Persist(InvertYKey, invY); }

        bool invP = Noclip.InvertPitch;
        if (ImGui.Checkbox("Invert noclip dive", ref invP)) { Noclip.InvertPitch = invP; Persist(InvertPitchKey, invP); }

        bool invS = Noclip.InvertStrafe;
        if (ImGui.Checkbox("Invert noclip strafe", ref invS)) { Noclip.InvertStrafe = invS; Persist(InvertSKey, invS); }

        bool keys = Hotkeys.Enabled;
        if (ImGui.Checkbox("Hotkeys enabled", ref keys)) { Hotkeys.Enabled = keys; Persist(HotkeysKey, keys); }
    }

    static void Persist(string key, float value)
    {
        RecompOne.Runtime.Runtime.View.SetFloat(key, value);
        RecompOne.Runtime.Runtime.SaveView();
    }

    internal static void Persist(string key, bool value)
    {
        RecompOne.Runtime.Runtime.View.SetBool(key, value);
        RecompOne.Runtime.Runtime.SaveView();
    }

    static void ReadEnv(string name, ref bool value)
    {
        string? v = Environment.GetEnvironmentVariable(name);
        if (string.IsNullOrWhiteSpace(v)) return;
        v = v.Trim().ToLowerInvariant();
        value = v is "1" or "on" or "true" or "yes";
    }

    static void ReadEnv(string name, ref float value)
    {
        string? v = Environment.GetEnvironmentVariable(name);
        if (float.TryParse(v, System.Globalization.NumberStyles.Float,
                           System.Globalization.CultureInfo.InvariantCulture, out float f))
            value = f;
    }
}
