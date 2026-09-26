using ImGuiNET;
using RecompOne.Runtime;
using RecompOne.Runtime.Host.Window;

namespace Kf2.Settings;

/// <summary>
/// The water switches, under Video ▸ Experimental after the Z-buffer: the murk and
/// the three reflection sources, each on its own. Their tuning is on the console
/// (<c>KF2_MURK_*</c>, <c>KF2_SSR_*</c>, <c>KF2_PLANAR_*</c>, <c>KF2_RETAINED_*</c>),
/// as the occlusion pass's is.
/// </summary>
public sealed class ReflectionsPage : IPatchPage
{
    public string Id => "ssr";
    public string Title => "Experimental";
    public int Order => 28;

    const string Strings = """
    {
      "strings": {
        "kf2.murk.label": {
          "en": "Murky water",
          "pt-BR": "Água turva",
          "es-419": "Agua turbia"
        },
        "kf2.murk.tooltip": {
          "en": "Water darkens with depth: the further the view runs through it to the bottom, the murkier it gets. Needs no reflections. Experimental.",
          "pt-BR": "A água escurece com a profundidade: quanto mais a vista a atravessa até o fundo, mais turva fica. Não precisa de reflexos. Experimental.",
          "es-419": "El agua se oscurece con la profundidad: cuanto más la atraviesa la vista hasta el fondo, más turbia se ve. No necesita reflejos. Experimental."
        },
        "kf2.murk.distance": {
          "en": "Murk depth",
          "pt-BR": "Profundidade da turvação",
          "es-419": "Profundidad de la turbidez"
        },
        "kf2.murk.distance.tooltip": {
          "en": "How much water the view crosses before it is mostly murk, in world units (a floor tile is 2048). Lower is murkier.",
          "pt-BR": "Quanta água a vista atravessa antes de ficar quase toda turva, em unidades do mundo (um ladrilho do piso tem 2048). Menor é mais turvo.",
          "es-419": "Cuánta agua atraviesa la vista antes de ser casi toda turbia, en unidades del mundo (una baldosa del piso mide 2048). Menor es más turbio."
        },
        "kf2.murk.colour": {
          "en": "Murk colour",
          "pt-BR": "Cor da turvação",
          "es-419": "Color de la turbidez"
        },
        "kf2.murk.colour.tooltip": {
          "en": "The colour deep water fades to. It is fogged with the water, as the game's own colours are.",
          "pt-BR": "A cor para a qual a água funda se desvanece. É afetada pela névoa junto com a água, como as cores do próprio jogo.",
          "es-419": "El color al que se desvanece el agua profunda. La niebla lo afecta junto con el agua, como a los colores del propio juego."
        },
        "kf2.murk.reset": {
          "en": "Reset murk",
          "pt-BR": "Restaurar turvação",
          "es-419": "Restablecer turbidez"
        },
        "kf2.ssr.label": {
          "en": "Screen-space reflections",
          "pt-BR": "Reflexos em espaço de tela",
          "es-419": "Reflejos en espacio de pantalla"
        },
        "kf2.ssr.tooltip": {
          "en": "Water reflects what is on screen above it, where no planar or world reflection answers. Experimental.",
          "pt-BR": "A água reflete o que está na tela acima dela, onde nenhum reflexo planar ou do mundo responde. Experimental.",
          "es-419": "El agua refleja lo que está en pantalla por encima de ella, donde ningún reflejo planar o del mundo responde. Experimental."
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
        bool murk = Murk.Enabled;
        if (ImGui.Checkbox(Localization.T("kf2.murk.label"), ref murk))
        {
            Murk.SetEnabled(murk);
            PatchSettings.Set(Murk.OnKey, murk);
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip(Localization.T("kf2.murk.tooltip"));
        DrawMurk(murk);

        bool retained = RetainedMap.Enabled;
        if (ImGui.Checkbox(Localization.T("kf2.ssr.retained.label"), ref retained))
        {
            RetainedMap.SetEnabled(retained);
            PatchSettings.Set(RetainedMap.OnKey, retained);
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip(Localization.T("kf2.ssr.retained.tooltip"));

        // The world reflections draw the planes themselves.
        ImGui.BeginDisabled(retained);
        bool planar = PlanarWalk.Enabled;
        if (ImGui.Checkbox(Localization.T("kf2.ssr.planar.label"), ref planar))
        {
            PlanarWalk.SetEnabled(planar);
            PatchSettings.Set(PlanarWalk.OnKey, planar);
        }
        ImGui.EndDisabled();
        if (ImGui.IsItemHovered(ImGuiHoveredFlags.AllowWhenDisabled))
            ImGui.SetTooltip(Localization.T("kf2.ssr.planar.tooltip"));

        bool on = Reflections.Enabled;
        if (ImGui.Checkbox(Localization.T("kf2.ssr.label"), ref on))
        {
            Reflections.SetEnabled(on);
            PatchSettings.Set(Reflections.OnKey, on);
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip(Localization.T("kf2.ssr.tooltip"));
    }

    static void DrawMurk(bool on)
    {
        ImGui.BeginDisabled(!on);
        ImGui.Indent();

        float dist = WaterMurk.Distance;
        if (ImGui.SliderFloat(Localization.T("kf2.murk.distance"), ref dist, 100f, 8000f, "%.0f",
                              ImGuiSliderFlags.AlwaysClamp | ImGuiSliderFlags.Logarithmic))
        {
            WaterMurk.Distance = dist;
            PatchSettings.Set(Murk.DistanceKey, dist);
        }
        if (ImGui.IsItemHovered(ImGuiHoveredFlags.AllowWhenDisabled))
            ImGui.SetTooltip(Localization.T("kf2.murk.distance.tooltip"));

        var col = new System.Numerics.Vector3(WaterMurk.R, WaterMurk.G, WaterMurk.B);
        if (ImGui.ColorEdit3(Localization.T("kf2.murk.colour"), ref col, ImGuiColorEditFlags.Float))
        {
            WaterMurk.R = Math.Clamp(col.X, 0f, 1f);
            WaterMurk.G = Math.Clamp(col.Y, 0f, 1f);
            WaterMurk.B = Math.Clamp(col.Z, 0f, 1f);
            PatchSettings.Set(Murk.RKey, WaterMurk.R);
            PatchSettings.Set(Murk.GKey, WaterMurk.G);
            PatchSettings.Set(Murk.BKey, WaterMurk.B);
        }
        if (ImGui.IsItemHovered(ImGuiHoveredFlags.AllowWhenDisabled))
            ImGui.SetTooltip(Localization.T("kf2.murk.colour.tooltip"));

        if (ImGui.Button(Localization.T("kf2.murk.reset")))
        {
            WaterMurk.Distance = Murk.DefaultDistance;
            WaterMurk.R = Murk.DefaultR; WaterMurk.G = Murk.DefaultG; WaterMurk.B = Murk.DefaultB;
            PatchSettings.Set(Murk.DistanceKey, Murk.DefaultDistance);
            PatchSettings.Set(Murk.RKey, Murk.DefaultR);
            PatchSettings.Set(Murk.GKey, Murk.DefaultG);
            PatchSettings.Set(Murk.BKey, Murk.DefaultB);
        }

        ImGui.Unindent();
        ImGui.EndDisabled();
    }
}
