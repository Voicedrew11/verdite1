using System.Globalization;
using ImGuiNET;
using RecompOne.Runtime;
using Rt = RecompOne.Runtime.Runtime;

namespace Kf1;

/// <summary>
/// The world's picture on the retained renderer: lighting, fog, occlusion, texture
/// filtering and the distances the world is drawn at, with the renderer's own
/// choice beside them. Each has the `KF1_*` variable that sets it at boot and a
/// kept value in interface.ini (`kf1.*`), put back at the next boot unless the
/// variable is set, which wins.
///
///     KF1_PERPIXEL=0|1               per-pixel lighting (on)
///     KF1_FOG=0|corners|1|depth|2|distance|radial
///                                    fog at the corners, by pixel depth, or by distance from the eye (distance)
///     KF1_AO=0|1                     ambient occlusion (on)
///     KF1_AO_NORMALS=0|1            the occlusion's normals from the geometry (on)
///     KF1_AO_QUALITY=low|medium|high occlusion quality (medium)
///     KF1_ANISO=1..16               anisotropic filtering (16; 1 is off)
///     KF1_MIPMAPS=0|1               mipmaps (on)
///     KF1_ENHANCEDIST=cells         enhance out to this many cells; 0 or unset, at every distance
///     KF1_RENDERDIST=cells          draw this many cells out (16); 0, the game's
///     KF1_RENDERDIST_FADE=cells     fade in over this band at the edge (3); 0, none
///
/// Step 4 of docs/RENDERER_PLAN.md; see "The picture on the retained scene" in
/// docs/GPU_RENDERER.md. Defaults are Verdite3's judged ones.
/// </summary>
public static class SceneFeatures
{
    // The map's cell, in the game's units.
    const float Cell = 2000f;

    const string FogKey = "kf1.fog", AoQualityKey = "kf1.ao.quality", AnisoKey = "kf1.aniso";
    const string GpuWorldKey = "kf1.gpuworld";
    const string EnhanceKey = "kf1.enhancedistance", RenderKey = "kf1.renderdistance", FadeKey = "kf1.renderdistance.fade";
    const int DefaultFog = 2, DefaultAniso = 16;
    const float DefaultRender = 16f, DefaultFade = 3f;

    static int _quality = 1;

    // A kept switch: its key, its variable, how to read and set it, and its default.
    sealed record Kept(string Key, string Env, Func<bool> Get, Action<bool> Set, bool Default);

    static readonly Kept[] Switches =
    [
        new("kf1.perpixel", "KF1_PERPIXEL", () => GteLightMap.Enabled, v => GteLightMap.Enabled = v, true),
        new("kf1.ao", "KF1_AO", () => GteDepth.AmbientOcclusion, v => GteDepth.AmbientOcclusion = v, true),
        new("kf1.ao.normals", "KF1_AO_NORMALS", () => GteDepth.AoNormals, v => GteDepth.AoNormals = v, true),
        new("kf1.mipmaps", "KF1_MIPMAPS", () => GteDepth.Mipmaps, v => GteDepth.Mipmaps = v, true),
    ];

    static readonly string[] FogItems = ["At the corners (the game's)", "By pixel depth", "By distance from the eye"];
    static readonly string[] QualityItems = ["Low", "Medium", "High"];
    static readonly string[] RendererItems = ["Reference packets", "Retained beside the packets", "Retained"];

    /// <summary>The fog: 0 at the corners, 1 by pixel depth, 2 by distance from the eye.</summary>
    public static int Fog => GteDepth.RadialFog ? 2 : RetainedScene.MainFogFromZ ? 1 : 0;

    public static void SetFog(int mode)
    {
        mode = Math.Clamp(mode, 0, 2);
        RetainedScene.MainFogFromZ = mode >= 1;
        GteDepth.RadialFog = mode == 2;
    }

    static void SetQuality(int quality)
    {
        _quality = Math.Clamp(quality, 0, 2);
        GteDepth.AoResolution = _quality == 2 ? 0 : _quality + 1;
        GteDepth.AoSamples = _quality == 0 ? 8 : 16;
    }

    static void SetDistance(float cells) =>
        GteDepth.PlainDepth = float.IsFinite(cells) && cells > 0 ? Math.Clamp(cells, 2, 16) * Cell : 0;

    /// <summary>Applies the variables at boot. The kept values follow in <see cref="Ready"/>.</summary>
    public static void Configure()
    {
        foreach (var k in Switches) k.Set(Flag(k.Env, k.Default));
        SetFog(Env("KF1_FOG")?.Trim().ToLowerInvariant() switch { "0" or "corners" => 0, "1" or "depth" => 1, _ => DefaultFog });
        SetQuality(Env("KF1_AO_QUALITY")?.Trim().ToLowerInvariant() switch { "low" => 0, "high" => 2, _ => 1 });
        GteDepth.Anisotropy = Math.Clamp(int.TryParse(Env("KF1_ANISO"), NumberStyles.Integer, CultureInfo.InvariantCulture, out int a) ? a : DefaultAniso, 1, 16);
        SetDistance(FloatEnv("KF1_ENHANCEDIST"));
        RenderDistance.SetCells(FloatEnv("KF1_RENDERDIST", DefaultRender));
        RenderDistance.SetFade(FloatEnv("KF1_RENDERDIST_FADE", DefaultFade));
    }

    /// <summary>Puts the kept values back, for each variable that is unset. Called from
    /// TestingSection.Ready, once the config is loaded.</summary>
    public static void Ready()
    {
        if (Unset("KF1_GPU_WORLD") && Rt.View.GetInt(GpuWorldKey, -1) is >= 0 and var gw) GpuWorld.Setting = gw;
        foreach (var k in Switches)
            if (Unset(k.Env) && Rt.View.GetInt(k.Key, -1) is >= 0 and var v) k.Set(v != 0);
        if (Unset("KF1_FOG") && Rt.View.GetInt(FogKey, -1) is >= 0 and var fog) SetFog(fog);
        if (Unset("KF1_AO_QUALITY") && Rt.View.GetInt(AoQualityKey, -1) is >= 0 and var q) SetQuality(q);
        if (Unset("KF1_ANISO") && Rt.View.GetInt(AnisoKey, -1) is >= 0 and var an) GteDepth.Anisotropy = Math.Clamp(an, 1, 16);
        if (Unset("KF1_ENHANCEDIST")) ReadFloat(EnhanceKey, SetDistance);
        if (Unset("KF1_RENDERDIST")) ReadFloat(RenderKey, RenderDistance.SetCells);
        if (Unset("KF1_RENDERDIST_FADE")) ReadFloat(FadeKey, RenderDistance.SetFade);
    }

    static bool Unset(string env) => string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable(env));

    static string? Env(string name) => Environment.GetEnvironmentVariable(name);

    // 0 or "off" is false; any other value is true; unset or empty takes the fallback.
    static bool Flag(string name, bool fallback)
    {
        string? v = Env(name)?.Trim().ToLowerInvariant();
        return string.IsNullOrEmpty(v) ? fallback : v is not ("0" or "off");
    }

    static float FloatEnv(string name, float fallback = 0f) =>
        float.TryParse(Env(name), NumberStyles.Float, CultureInfo.InvariantCulture, out float v) && float.IsFinite(v) ? v : fallback;

    static void ReadFloat(string key, Action<float> set)
    {
        float v = Rt.View.GetFloat(key, float.NaN);
        if (float.IsFinite(v)) set(v);
    }

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

    static Kept K(string key) => Switches.First(s => s.Key == key);

    static void Toggle(string label, Kept k, string tip)
    {
        bool v = k.Get();
        if (ImGui.Checkbox(label, ref v))
        {
            k.Set(v);
            Keep(k.Key, v ? 1 : 0);
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip(tip);
    }

    static void Note(string text)
    {
        ImGui.PushStyleColor(ImGuiCol.Text, ImGui.GetStyle().Colors[(int)ImGuiCol.TextDisabled]);
        ImGui.TextWrapped(text);
        ImGui.PopStyleColor();
    }

    public static void Draw()
    {
        ImGui.SeparatorText("World on the GPU");
        Note("On by default, to be looked at. Depth fog, distance fog and the enhancement distance act through per-pixel lighting.");

        int mode = GpuWorld.Setting;
        ImGui.SetNextItemWidth(240);
        if (ImGui.Combo("Renderer", ref mode, RendererItems, RendererItems.Length))
        {
            GpuWorld.Setting = mode;
            Keep(GpuWorldKey, mode);
        }

        Toggle("Per-pixel lighting", K("kf1.perpixel"), "Lights each pixel rather than each corner, so the shading follows the surface.");

        int fog = Fog;
        ImGui.SetNextItemWidth(240);
        if (ImGui.Combo("Fog", ref fog, FogItems, FogItems.Length))
        {
            SetFog(fog);
            Keep(FogKey, fog);
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip("Where the fog is worked out. Depth and distance need per-pixel lighting on.");

        Toggle("Ambient occlusion", K("kf1.ao"), "Darkens the creases where walls, floor and ceiling meet.");
        Toggle("Geometry normals", K("kf1.ao.normals"), "Gives the occlusion the geometry's own normals. Only matters with ambient occlusion on.");

        int quality = _quality;
        ImGui.SetNextItemWidth(160);
        if (ImGui.Combo("Occlusion quality", ref quality, QualityItems, QualityItems.Length))
        {
            SetQuality(quality);
            Keep(AoQualityKey, _quality);
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip("Samples and resolution of the occlusion pass. Only matters with ambient occlusion on.");

        int aniso = GteDepth.Anisotropy;
        ImGui.SetNextItemWidth(240);
        if (ImGui.SliderInt("Anisotropic filtering", ref aniso, 1, 16, "%dx"))
        {
            GteDepth.Anisotropy = aniso;
            Keep(AnisoKey, aniso);
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip("Sharpens textures seen at a steep angle. 1x is off.");

        Toggle("Mipmaps", K("kf1.mipmaps"), "Draws distant textures from smaller copies, which stops them shimmering.");

        // The enhancements' reach: a checkbox for every distance, then the cells.
        bool everywhere = GteDepth.PlainDepth <= 0;
        float cells = everywhere ? 8 : GteDepth.PlainDepth / Cell;
        if (ImGui.Checkbox("Enhance at every distance", ref everywhere))
        {
            SetDistance(everywhere ? 0 : cells);
            KeepFloat(EnhanceKey, everywhere ? 0 : cells);
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip("Applies the enhancements however far the surface is. Off, only out to the distance below.");
        ImGui.BeginDisabled(everywhere);
        ImGui.SetNextItemWidth(240);
        if (ImGui.SliderFloat("Enhancement distance (cells)", ref cells, 2, 16, "%.1f")) SetDistance(cells);
        if (ImGui.IsItemDeactivatedAfterEdit()) KeepFloat(EnhanceKey, cells);
        ImGui.EndDisabled();

        // The retained renderer's reach and the fade at its edge.
        bool far = RenderDistance.Cells > 0;
        float reach = far ? RenderDistance.Cells : 10;
        if (ImGui.Checkbox("Draw past the game's distance", ref far))
        {
            RenderDistance.SetCells(far ? reach : 0);
            KeepFloat(RenderKey, far ? reach : 0);
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip("Draws the world past the game's distance. Needs the retained renderer (Renderer: Retained).");
        ImGui.BeginDisabled(!far);
        ImGui.SetNextItemWidth(240);
        if (ImGui.SliderFloat("Render distance (cells)", ref reach, 8, RenderDistance.MaxCells, "%.0f")) RenderDistance.SetCells(reach);
        if (ImGui.IsItemDeactivatedAfterEdit()) KeepFloat(RenderKey, reach);
        ImGui.EndDisabled();

        bool fade = RenderDistance.FadeCells > 0;
        float band = fade ? RenderDistance.FadeCells : 3;
        if (ImGui.Checkbox("Fade in at the edge of the view", ref fade))
        {
            RenderDistance.SetFade(fade ? band : 0);
            KeepFloat(FadeKey, fade ? band : 0);
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip("Fades the world in across the band at the render distance's edge, rather than letting it pop in.");
        ImGui.BeginDisabled(!fade);
        ImGui.SetNextItemWidth(240);
        if (ImGui.SliderFloat("Fade band (cells)", ref band, 0.5f, RenderDistance.MaxFade, "%.1f")) RenderDistance.SetFade(band);
        if (ImGui.IsItemDeactivatedAfterEdit()) KeepFloat(FadeKey, band);
        ImGui.EndDisabled();
    }
}
