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
        "kf1testing.tickrate.reset": { "en": "Reset to 20 Hz", "pt-BR": "Restaurar 20 Hz", "es-419": "Restablecer 20 Hz" },
        "kf1testing.tickrate.tip": {
          "en": "World updates per second. The original is 20 Hz; changing this changes gameplay speed. Requires frame pacing. Saved for the next launch; KF1_TICKRATE overrides it at boot.",
          "pt-BR": "Atualizações do mundo por segundo. O original é 20 Hz; alterar isso muda a velocidade do jogo. Requer controle de quadros. Salvo para a próxima execução; KF1_TICKRATE tem prioridade ao iniciar.",
          "es-419": "Actualizaciones del mundo por segundo. La frecuencia original es 20 Hz; cambiarla cambia la velocidad del juego. Requiere control de cuadros. Se guarda para el próximo inicio; KF1_TICKRATE tiene prioridad al iniciar."
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
    }

    static bool Unset(string env) => string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable(env));

    static void Keep(string key, int value)
    {
        Rt.View.SetInt(key, value);
        Rt.SaveView();
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

        Toggle("Perspective-correct textures", K("kf1.perspective"),
            "Textures follow each corner's depth instead of warping across a polygon.");
        Toggle("Sub-pixel vertices", K("kf1.subpixel"),
            "Corners keep the fraction of a pixel the GTE dropped, so edges stop jittering as the view moves.");
        Toggle("Z-buffer", K("kf1.zbuffer"),
            "Per-pixel occlusion from the depths the address map recovers; a triangle it missed keeps painter's order.");

        ImGui.SeparatorText("Mouse");
        Toggle("Mouse look", K(Mouse.OnKey),
            "Steers with the mouse and presses pad buttons with its buttons; Escape captures the pointer.");
        Toggle("Instant mouse look", K(Mouse.LeadKey),
            "Turns the view the frame you move the mouse, instead of on the game's next tick.");

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
        bool zp = ZBuffer.ProbeOn;
        if (ImGui.Checkbox("Z-buffer (KF1_ZBUFFER_PROBE)", ref zp)) ZBuffer.ProbeOn = zp;
    }
}
