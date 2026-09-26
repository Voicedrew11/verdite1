using ImGuiNET;
using RecompOne.Runtime.Host.Window;

namespace Kf2.Settings;

/// <summary>
/// The reflections switch, under Video ▸ Experimental after the Z-buffer, with the
/// two sources of the world it can reflect under it. Their tuning is on the console
/// (<c>KF2_SSR_*</c>, <c>KF2_PLANAR_*</c>, <c>KF2_RETAINED_*</c>), as the occlusion
/// pass's is.
/// </summary>
public sealed class ReflectionsPage : IPatchPage
{
    public string Id => "ssr";
    public string Title => "Experimental";
    public int Order => 28;

    const string Strings = """
    {
      "strings": {
        "kf2.ssr.label": {
          "en": "Water reflections",
          "pt-BR": "Reflexos na água",
          "es-419": "Reflejos en el agua"
        },
        "kf2.ssr.tooltip": {
          "en": "Water reflects what is on screen above it. Experimental.",
          "pt-BR": "A água reflete o que está na tela acima dela. Experimental.",
          "es-419": "El agua refleja lo que está en pantalla por encima de ella. Experimental."
        },
        "kf2.ssr.planar.label": {
          "en": "Planar reflections",
          "pt-BR": "Reflexos planares",
          "es-419": "Reflejos planares"
        },
        "kf2.ssr.planar.tooltip": {
          "en": "Draws the world a second time from below the water, so the reflection includes what is off screen or hidden. Costs a second walk of the scene whenever water is in view. Experimental.",
          "pt-BR": "Desenha o mundo uma segunda vez por baixo da água, para que o reflexo inclua o que está fora da tela ou escondido. Custa um segundo percurso da cena sempre que há água à vista. Experimental.",
          "es-419": "Dibuja el mundo una segunda vez desde debajo del agua, para que el reflejo incluya lo que está fuera de pantalla u oculto. Cuesta un segundo recorrido de la escena siempre que haya agua a la vista. Experimental."
        },
        "kf2.ssr.retained.label": {
          "en": "World reflections",
          "pt-BR": "Reflexos do mundo",
          "es-419": "Reflejos del mundo"
        },
        "kf2.ssr.retained.tooltip": {
          "en": "Keeps the area's geometry on the graphics card and draws reflections from it: water and reflective floors are mirrored exactly, and every other reflective surface looks up a cubemap around the camera instead of the picture. Nothing on screen is borrowed. Replaces planar reflections while on. Experimental.",
          "pt-BR": "Mantém a geometria da área na placa de vídeo e desenha os reflexos a partir dela: a água e os pisos reflexivos são espelhados com exatidão, e toda outra superfície reflexiva consulta um cubemap ao redor da câmera em vez da imagem. Nada da tela é reaproveitado. Substitui os reflexos planares enquanto ligado. Experimental.",
          "es-419": "Mantiene la geometría del área en la tarjeta gráfica y dibuja los reflejos a partir de ella: el agua y los pisos reflectantes se reflejan con exactitud, y cualquier otra superficie reflectante consulta un cubemap alrededor de la cámara en lugar de la imagen. No se toma nada de la pantalla. Reemplaza los reflejos planares mientras está activo. Experimental."
        }
      }
    }
    """;

    public ReflectionsPage() => Localization.Merge(Strings);

    public void Draw()
    {
        bool on = Reflections.Enabled;
        if (ImGui.Checkbox(Localization.T("kf2.ssr.label"), ref on))
        {
            Reflections.SetEnabled(on);
            PatchSettings.Set(Reflections.OnKey, on);
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip(Localization.T("kf2.ssr.tooltip"));

        // Read by the reflection pass, so it means nothing with that off.
        ImGui.Indent();
        ImGui.BeginDisabled(!on);
        bool retained = RetainedMap.Enabled;
        if (ImGui.Checkbox(Localization.T("kf2.ssr.retained.label"), ref retained))
        {
            RetainedMap.SetEnabled(retained);
            PatchSettings.Set(RetainedMap.OnKey, retained);
        }
        ImGui.EndDisabled();
        if (ImGui.IsItemHovered(ImGuiHoveredFlags.AllowWhenDisabled))
            ImGui.SetTooltip(Localization.T("kf2.ssr.retained.tooltip"));

        // The world reflections draw the planes themselves.
        ImGui.BeginDisabled(!on || retained);
        bool planar = PlanarWalk.Enabled;
        if (ImGui.Checkbox(Localization.T("kf2.ssr.planar.label"), ref planar))
        {
            PlanarWalk.SetEnabled(planar);
            PatchSettings.Set(PlanarWalk.OnKey, planar);
        }
        ImGui.EndDisabled();
        if (ImGui.IsItemHovered(ImGuiHoveredFlags.AllowWhenDisabled))
            ImGui.SetTooltip(Localization.T("kf2.ssr.planar.tooltip"));
        ImGui.Unindent();
    }
}
