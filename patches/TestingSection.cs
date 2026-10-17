using ImGuiNET;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Host.Window;
using Rt = RecompOne.Runtime.Runtime;

namespace Kf1;

/// <summary>
/// A settings tab of the port's own, **Testing**: every switch the port has, live,
/// so a change can be compared without a restart. Each control writes the same
/// state its `KF1_*` variable sets at boot.
///
/// The on/off choices, frame rate and tick rate are kept in interface.ini (`kf3.*`) and
/// put back at the next boot, **unless the variable is set**, which wins. The
/// routines' recompiled/C#/verify choice is not kept: verify is a comparison for
/// one session. See "The Testing tab" in docs/DEVELOPMENT.md.
/// </summary>
public sealed class TestingSection : ISettingsSection
{
    public string Id => "kf1testing";
    public string TitleKey => "settings.kf1testing";

    // After Audio (10), before Paths (20).
    public int Order => 15;

    const string Names = """
    {
      "strings": {
        "settings.kf1testing": { "en": "Testing", "pt-BR": "Testes", "es-419": "Pruebas" },
        "kf1testing.pacing.tip": {
          "en": "Runs the world at the tick rate below and draws at the chosen frame rate.",
          "pt-BR": "Atualiza o mundo na taxa de ticks abaixo e desenha na taxa de quadros escolhida.",
          "es-419": "Actualiza el mundo a la frecuencia de ticks de abajo y dibuja a la frecuencia de cuadros elegida."
        },
        "kf1testing.tickrate": { "en": "Tick rate", "pt-BR": "Taxa de ticks", "es-419": "Frecuencia de ticks" },
        "kf1testing.tickrate.reset": { "en": "Reset to 15 Hz", "pt-BR": "Restaurar 15 Hz", "es-419": "Restablecer 15 Hz" },
        "kf1testing.tickrate.tip": {
          "en": "World updates per second. The default is 15 Hz; the frame gate's own is 20 Hz. Changing this changes gameplay speed. Requires frame pacing. Saved for the next launch; KF1_TICKRATE overrides it at boot.",
          "pt-BR": "Atualizações do mundo por segundo. O padrão é 15 Hz; o do próprio jogo é 20 Hz. Alterar isso muda a velocidade do jogo. Requer controle de quadros. Salvo para a próxima execução; KF1_TICKRATE tem prioridade ao iniciar.",
          "es-419": "Actualizaciones del mundo por segundo. La predeterminada es 15 Hz; la del propio juego es 20 Hz. Cambiarla cambia la velocidad del juego. Requiere control de cuadros. Se guarda para el próximo inicio; KF1_TICKRATE tiene prioridad al iniciar."
        },
        "kf1testing.widescreen.aspect": { "en": "Aspect", "pt-BR": "Proporção", "es-419": "Relación de aspecto" },
        "kf1testing.widescreen.tip": {
          "en": "How wide the picture is. Wider shows more of the room, not a stretched 4:3.",
          "pt-BR": "A largura da imagem. Mais largo mostra mais do ambiente, não um 4:3 esticado.",
          "es-419": "Qué tan ancha es la imagen. Más ancho muestra más del entorno, no un 4:3 estirado."
        }
      }
    }
    """;

    static bool _installed;

    // A kept setting: its key, its variable, how to read it and how to apply it.
    sealed record Kept(string Key, string Env, Func<bool> Get, Action<bool> Set);

    static readonly Kept[] Switches =
    [
        new("kf1.pacing", "KF1_FPS", () => FramePacing.Enabled, FramePacing.SetEnabled),
        new("kf1.vblank_hold", "KF1_VBLANKPACING", () => VBlankPacing.Enabled, v => VBlankPacing.Enabled = v),
        new("kf1.smooth", "KF1_SMOOTH", () => ViewSmoothing.Enabled, v => ViewSmoothing.Enabled = v),
        new("kf1.smooth_models", "KF1_SMOOTH_MODELS", () => ModelSmoothing.Enabled, v => ModelSmoothing.Enabled = v),
        new("kf1.perspective", "KF1_PERSPECTIVE", () => Perspective.Enabled, v => Perspective.Enabled = v),
        new("kf1.subpixel", "KF1_SUBPIXEL", () => Subpixel.Enabled, v => Subpixel.Enabled = v),
        new("kf1.zbuffer", "KF1_ZBUFFER", () => ZBuffer.Enabled, v => ZBuffer.Enabled = v),
        new(Mouse.OnKey, "KF1_MOUSE", () => Mouse.Enabled, v => Mouse.Enabled = v),
        new(Mouse.LeadKey, "KF1_MOUSE_LEAD", () => Mouse.Lead, v => Mouse.Lead = v),
    ];

    const string FpsKey = "kf1.fps", TickRateKey = "kf1.tickrate", ShadingKey = "kf1.shading";

    public static void Install()
    {
        if (_installed) return;
        _installed = true;
        Event.AddListener<RuntimeReadyEvent>(_ => Ready());
    }

    /// <summary>The config is loaded inside the host window's start-up, after Program.cs,
    /// so kept values go back here; a set variable is left as it chose.</summary>
    static void Ready()
    {
        Localization.Merge(Names);
        SettingsRegistry.Register(new TestingSection());

        if (Unset("KF1_FPS"))
        {
            int fps = Rt.View.GetInt(FpsKey, -1);
            FramePacing.SetTarget(fps >= 0 ? fps : FramePacing.DefaultFps);
        }
        if (Unset("KF1_TICKRATE"))
        {
            float hz = Rt.View.GetFloat(TickRateKey, (float)FramePacing.DefaultTickRate);
            FramePacing.SetTickRate(float.IsFinite(hz) ? hz : FramePacing.DefaultTickRate);
        }
        foreach (var k in Switches)
            if (Unset(k.Env) && Rt.View.GetInt(k.Key, -1) is >= 0 and var v) k.Set(v != 0);
        if (Unset("KF1_TRUECOLOR") && Unset("KF1_NODITHER") && Rt.View.GetInt(ShadingKey, -1) is >= 0 and var sh)
            SetShading(sh);
        SceneFeatures.Ready();
        Analog.Ready();
    }

    static bool Unset(string env) => string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable(env));

    static void Keep(string key, int value)
    {
        Rt.View.SetInt(key, value);
        Rt.SaveView();
    }

    static void KeepFloat(string key, float value)
    {
        Rt.View.SetFloat(key, value);
        Rt.SaveView();
    }

    static void Check(string label, ref bool v, string key, string tip)
    {
        if (ImGui.Checkbox(label, ref v)) Keep(key, v ? 1 : 0);
        if (ImGui.IsItemHovered()) ImGui.SetTooltip(tip);
    }

    static void Slider(string label, ref float v, float min, float max, string format, string key, string tip)
    {
        ImGui.SetNextItemWidth(240);
        ImGui.SliderFloat(label, ref v, min, max, format, ImGuiSliderFlags.AlwaysClamp);
        if (ImGui.IsItemDeactivatedAfterEdit()) KeepFloat(key, v);
        if (ImGui.IsItemHovered()) ImGui.SetTooltip(tip);
    }

    static void AnalogSticks()
    {
        ImGui.SeparatorText("Analog sticks");
        Check("Twin-stick control", ref Analog.Enabled, Analog.OnKey,
            "Off hands the sticks back to the pad bindings, where the left stick is the D-pad.");
        ImGui.BeginDisabled(!Analog.Enabled);
        string[] controls = ["2D (twin sticks)", "3D (one stick)"];
        int c = Analog.Tank ? 1 : 0;
        ImGui.SetNextItemWidth(200);
        if (ImGui.Combo("Controls", ref c, controls, controls.Length)) { Analog.Tank = c == 1; Keep(Analog.ControlsKey, c); }
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("2D: the left stick walks and strafes, the right stick turns and looks.\n" +
                             "3D: the left stick walks and turns, as the D-pad does; L1/R1 strafe, L2/R2 look.");
        Check("Right stick turns and looks", ref Analog.AnalogLook, Analog.LookKey, "2D only.");
        Check("Left stick walks", ref Analog.AnalogMove, Analog.MoveKey, "Off, the left stick is the D-pad again.");
        Slider("Turn sensitivity", ref Analog.TurnSens, 0.1f, 3f, "x%.2f", Analog.TurnSensKey,
            "x1 is the D-pad's turning speed at full deflection.");
        Slider("Look sensitivity", ref Analog.PitchSens, 0.1f, 3f, "x%.2f", Analog.PitchSensKey,
            "x1 is L2/R2's looking speed at full deflection.");
        Slider("Move sensitivity", ref Analog.MoveSens, 0.1f, 1.5f, "x%.2f", Analog.MoveSensKey,
            "x1 is the game's walking speed at full deflection.");
        if (ImGui.TreeNode("Fine tuning"))
        {
            Slider("Look deadzone", ref Analog.LookDeadzone, 0f, 0.5f, "%.2f", Analog.LookDeadKey, "The right stick's centre that does nothing.");
            Slider("Move deadzone", ref Analog.MoveDeadzone, 0f, 0.5f, "%.2f", Analog.MoveDeadKey, "The left stick's centre that does nothing.");
            Slider("Look curve", ref Analog.LookCurve, 1f, 3f, "%.2f", Analog.LookCurveKey, "Higher is finer near the centre.");
            Slider("Move curve", ref Analog.MoveCurve, 1f, 3f, "%.2f", Analog.MoveCurveKey, "Higher is finer near the centre.");
            Check("Stick eases in and out", ref Analog.StickEase, Analog.EaseKey,
                "The turn and look build up and wind down by the game's own step, as a held D-pad does. Off is direct, at the same top speed.");
            Check("Camera stops on release", ref Analog.CameraInstantStop, Analog.StopKey,
                "Stops the turn and look dead when the stick is let go, instead of the game's wind-down.");
            Check("Look acceleration", ref Analog.LookAccel, Analog.AccelKey,
                "Held far out, the right stick speeds up past the D-pad's rate, up to the most below.");
            ImGui.BeginDisabled(!Analog.LookAccel);
            Slider("Acceleration, most", ref Analog.LookAccelMax, 1f, 4f, "x%.1f", Analog.AccelMaxKey, "The fastest the ramp turns, as a multiple of the D-pad's rate.");
            Slider("Acceleration, time", ref Analog.LookAccelTime, 0.1f, 2f, "%.1f s", Analog.AccelTimeKey, "How long the ramp takes to reach it.");
            ImGui.EndDisabled();
            Check("Invert look Y", ref Analog.InvertPitch, Analog.InvertPitchKey, "Push up to look down.");
            Check("Invert turn", ref Analog.InvertTurn, Analog.InvertTurnKey, "Push right to turn left.");
            Check("Invert strafe", ref Analog.InvertStrafe, Analog.InvertStrafeKey, "Push right to strafe left.");
            Check("Invert forward", ref Analog.InvertForward, Analog.InvertFwdKey, "Push up to walk back.");
            ImGui.TreePop();
        }
        ImGui.EndDisabled();
        var (lx, ly, rx, ry) = Analog.Sticks;
        Note($"Sticks: L ({lx:+0.00;-0.00}, {ly:+0.00;-0.00})  R ({rx:+0.00;-0.00}, {ry:+0.00;-0.00})");
    }

    static void KeepTickRate(double hz)
    {
        FramePacing.SetTickRate(hz);
        Rt.View.SetFloat(TickRateKey, (float)FramePacing.LogicHz);
        Rt.SaveView();
    }

    static void Toggle(string label, Kept k, string tip)
    {
        bool v = k.Get();
        if (ImGui.Checkbox(label, ref v))
        {
            k.Set(v);
            Keep(k.Key, k.Get() ? 1 : 0);
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip(tip);
    }

    // Dither (the console), None (15-bit, no crosshatch), Smooth (24-bit, no crosshatch).
    static int Shading => TrueColor.Enabled ? 2 : NoDither.Enabled ? 1 : 0;

    static void SetShading(int v)
    {
        NoDither.Enabled = v != 0;
        TrueColor.Enabled = v == 2;
    }

    static Kept K(string key) => Switches.First(s => s.Key == key);

    static readonly string[] Routine = ["Recompiled", "C#", "Verify"];

    static void RoutineCombo(string label, Func<int> get, Action<int> set, string tip)
    {
        int v = get();
        ImGui.SetNextItemWidth(160);
        if (ImGui.Combo(label, ref v, Routine, Routine.Length)) set(v);
        if (ImGui.IsItemHovered()) ImGui.SetTooltip(tip);
    }

    // The readout, measured from the frame and tick counters every half second.
    static long _frames0, _ticks0;
    static double _at0 = -1.0, _fps, _tps;

    static void Rates()
    {
        double now = Environment.TickCount64 / 1000.0;
        if (_at0 < 0.0 || now - _at0 > 2.0) { _at0 = now; _frames0 = FramePacing.Frames; _ticks0 = FramePacing.Ticks; return; }
        if (now - _at0 < 0.5) return;
        _fps = (FramePacing.Frames - _frames0) / (now - _at0);
        _tps = (FramePacing.Ticks - _ticks0) / (now - _at0);
        _at0 = now; _frames0 = FramePacing.Frames; _ticks0 = FramePacing.Ticks;
    }

    static void Note(string text)
    {
        ImGui.PushStyleColor(ImGuiCol.Text, ImGui.GetStyle().Colors[(int)ImGuiCol.TextDisabled]);
        ImGui.TextWrapped(text);
        ImGui.PopStyleColor();
    }

    public void Draw()
    {
        ImGui.SeparatorText("Frame pacing");
        Toggle("Frame pacing", K("kf1.pacing"),
            Localization.T("kf1testing.pacing.tip"));

        bool uncapped = FramePacing.TargetFps <= 0.0;
        if (!FramePacing.Enabled) ImGui.BeginDisabled();
        int fps = uncapped ? (int)FramePacing.DefaultFps : (int)FramePacing.TargetFps;
        if (uncapped) ImGui.BeginDisabled();
        ImGui.SetNextItemWidth(240);
        if (ImGui.SliderInt("Frame rate", ref fps, 30, 360))
        {
            FramePacing.SetTarget(fps);
            Keep(FpsKey, fps);
        }
        if (uncapped) ImGui.EndDisabled();
        ImGui.SameLine();
        if (ImGui.Checkbox("Uncapped", ref uncapped))
        {
            FramePacing.SetTarget(uncapped ? 0 : fps);
            Keep(FpsKey, uncapped ? 0 : fps);
        }
        float hz = (float)FramePacing.LogicHz;
        ImGui.SetNextItemWidth(240);
        if (ImGui.SliderFloat(Localization.T("kf1testing.tickrate"), ref hz, 5f, 60f, "%.1f Hz", ImGuiSliderFlags.AlwaysClamp))
            KeepTickRate(hz);
        if (ImGui.IsItemHovered()) ImGui.SetTooltip(Localization.T("kf1testing.tickrate.tip"));
        ImGui.SameLine();
        if (ImGui.Button(Localization.T("kf1testing.tickrate.reset")))
            KeepTickRate(FramePacing.DefaultTickRate);
        if (FramePacing.Enabled) { Rates(); Note($"Drawing {_fps:0.0} fps at {_tps:0.0} ticks a second."); }
        if (!FramePacing.Enabled) ImGui.EndDisabled();

        Toggle("Hold menus and loading screens to the vblank", K("kf1.vblank_hold"),
            "Waits a real vblank for every VSync call outside the renderer, as the console did.");

        ImGui.SeparatorText("Smoothing");
        if (!FramePacing.Enabled)
        {
            Note("Everything here acts only with frame pacing on.");
            ImGui.BeginDisabled();
        }
        Toggle("Camera", K("kf1.smooth"),
            "Draws the view between the world's ticks.");
        Toggle("Creatures, objects and the arm", K("kf1.smooth_models"),
            "Draws creatures, objects, the arm and their animation between the world's ticks.");
        if (!FramePacing.Enabled) ImGui.EndDisabled();

        ImGui.SeparatorText("Picture");
        Note("Not judged yet: each ships off until it has been looked at.");
        string[] shading = ["Dither (the console)", "None", "Smooth (24-bit)"];
        int sh = Shading;
        ImGui.SetNextItemWidth(200);
        if (ImGui.Combo("Shading", ref sh, shading, shading.Length))
        {
            SetShading(sh);
            Keep(ShadingKey, sh);
        }
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("None drops the crosshatch and keeps 15-bit bands; Smooth keeps 8 bits a channel instead.");

        // The aspect is the one picture setting whose value is not a tick, so it is
        // a combo, and it writes kf1.widescreen.aspect on change. A value that is
        // none of the presets -- from KF1_WIDESCREEN -- keeps its own entry rather
        // than being rounded onto one.
        var widescreen = Widescreen.Presets;
        int aspectIndex = -1;
        for (int i = 0; i < widescreen.Length; i++)
            if (Math.Abs(Widescreen.Aspect - widescreen[i].Ratio) < 0.001f) { aspectIndex = i; break; }
        string[] aspectItems = aspectIndex >= 0
            ? [.. widescreen.Select(p => p.Name)]
            : [.. widescreen.Select(p => p.Name), $"Custom ({Widescreen.Aspect:0.###}:1)"];
        if (aspectIndex < 0) aspectIndex = aspectItems.Length - 1;
        ImGui.SetNextItemWidth(200);
        if (ImGui.Combo(Localization.T("kf1testing.widescreen.aspect"), ref aspectIndex, aspectItems, aspectItems.Length)
            && aspectIndex < widescreen.Length)
        {
            Widescreen.SetAspect(widescreen[aspectIndex].Ratio);
            Rt.View.SetFloat(Widescreen.AspectKey, Widescreen.Aspect);
            Rt.SaveView();
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip(Localization.T("kf1testing.widescreen.tip"));
        Toggle("Perspective-correct textures", K("kf1.perspective"),
            "Textures follow each corner's depth instead of warping across a polygon.");
        Toggle("Sub-pixel vertices", K("kf1.subpixel"),
            "Corners keep the fraction of a pixel the GTE dropped, so edges stop jittering as the view moves.");
        Toggle("Z-buffer", K("kf1.zbuffer"),
            "Per-pixel occlusion from the depths the address map recovers; a triangle it missed keeps painter's order.");

        SceneFeatures.Draw();

        ImGui.SeparatorText("Mouse");
        Toggle("Mouse look", K(Mouse.OnKey),
            "Steers with the mouse and presses pad buttons with its buttons; Escape captures the pointer.");
        Toggle("Instant mouse look", K(Mouse.LeadKey),
            "Turns the view the frame you move the mouse, instead of on the game's next tick.");

        AnalogSticks();

        ImGui.SeparatorText("Routines in C#");
        Note("Verify runs both versions every call and prints mismatches to the console; it is slow.");
        RoutineCombo("Renderer", () => Renderer.Setting, v => Renderer.Setting = v,
            "Stage I, which draws the whole frame.");
        RoutineCombo("Camera block", () => CameraBlock.Setting, v => CameraBlock.Setting = v,
            "The view matrices from the camera.");
        foreach (var r in Literal.All)
            RoutineCombo(r.Name, () => r.Setting, v => r.Setting = v, r.Tip);

        ImGui.SeparatorText("Console probes");
        bool p = FramePacing.ProbeOn;
        if (ImGui.Checkbox("Pacing (KF1_FPS_PROBE)", ref p)) FramePacing.ProbeOn = p;
        bool s = ViewSmoothing.ProbeOn;
        if (ImGui.Checkbox("Smoothing (KF1_SMOOTH_PROBE)", ref s)) { ViewSmoothing.ProbeOn = s; ModelSmoothing.ProbeOn = s; }
        bool d = NoDither.ProbeOn;
        if (ImGui.Checkbox("Dither (KF1_NODITHER_PROBE)", ref d)) NoDither.ProbeOn = d;
        bool pp = Perspective.ProbeOn;
        if (ImGui.Checkbox("Address map (KF1_PERSPECTIVE_PROBE)", ref pp)) Perspective.ProbeOn = pp;
        bool sp = Subpixel.ProbeOn;
        if (ImGui.Checkbox("Sub-pixel (KF1_SUBPIXEL_PROBE)", ref sp)) Subpixel.ProbeOn = sp;
        ImGui.Checkbox("Analog (KF1_ANALOG_PROBE)", ref Analog.ProbeOn);
        bool zp = ZBuffer.ProbeOn;
        if (ImGui.Checkbox("Z-buffer (KF1_ZBUFFER_PROBE)", ref zp)) ZBuffer.ProbeOn = zp;
    }
}
