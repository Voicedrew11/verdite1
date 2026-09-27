using ImGuiNET;
using RecompOne.Runtime;
using RecompOne.Runtime.Host.Window;

namespace Kf2.Settings;

/// <summary>
/// The water switches, under Video ▸ Experimental after the Z-buffer: the murk, the
/// waves and the three reflection sources, each on its own. Their tuning is on the console
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
        "kf2.waves.label": {
          "en": "Water waves",
          "pt-BR": "Ondas na água",
          "es-419": "Olas en el agua"
        },
        "kf2.waves.tooltip": {
          "en": "Water moves: a slow swell lifts and lowers the surface, and ripples push and shade its texture, so the pattern no longer repeats tile by tile. The edges of the water stay where they are. Experimental.",
          "pt-BR": "A água se move: uma ondulação lenta sobe e desce a superfície, e marolas deslocam e sombreiam sua textura, para que o padrão não se repita ladrilho a ladrilho. As bordas da água ficam onde estão. Experimental.",
          "es-419": "El agua se mueve: un oleaje lento sube y baja la superficie, y ondas desplazan y sombrean su textura, para que el patrón no se repita baldosa a baldosa. Los bordes del agua se quedan donde están. Experimental."
        },
        "kf2.waves.swell": {
          "en": "Swell height",
          "pt-BR": "Altura da ondulação",
          "es-419": "Altura del oleaje"
        },
        "kf2.waves.swell.tooltip": {
          "en": "How far the surface rises and falls, in world units (a floor tile is 2048). Only open water moves; 0 turns the swell off.",
          "pt-BR": "Quanto a superfície sobe e desce, em unidades do mundo (um ladrilho do piso tem 2048). Só a água aberta se move; 0 desliga a ondulação.",
          "es-419": "Cuánto sube y baja la superficie, en unidades del mundo (una baldosa del piso mide 2048). Solo se mueve el agua abierta; 0 apaga el oleaje."
        },
        "kf2.waves.swellsize": {
          "en": "Swell length",
          "pt-BR": "Comprimento da ondulação",
          "es-419": "Longitud del oleaje"
        },
        "kf2.waves.swellsize.tooltip": {
          "en": "The distance between crests of the longest swell, in world units. The water bends only at tile corners, so short swells look faceted.",
          "pt-BR": "A distância entre as cristas da ondulação mais longa, em unidades do mundo. A água só se dobra nos cantos dos ladrilhos, então ondulações curtas parecem facetadas.",
          "es-419": "La distancia entre crestas del oleaje más largo, en unidades del mundo. El agua solo se dobla en las esquinas de las baldosas, así que un oleaje corto se ve facetado."
        },
        "kf2.waves.ripple": {
          "en": "Ripple strength",
          "pt-BR": "Intensidade das marolas",
          "es-419": "Intensidad de las ondas"
        },
        "kf2.waves.ripple.tooltip": {
          "en": "How far the ripples push the water's texture, in world units (one of its pixels is 32). 0 leaves the texture still.",
          "pt-BR": "Quanto as marolas deslocam a textura da água, em unidades do mundo (um pixel dela tem 32). 0 deixa a textura parada.",
          "es-419": "Cuánto desplazan las ondas la textura del agua, en unidades del mundo (uno de sus píxeles mide 32). 0 deja la textura quieta."
        },
        "kf2.waves.ripplesize": {
          "en": "Ripple size",
          "pt-BR": "Tamanho das marolas",
          "es-419": "Tamaño de las ondas"
        },
        "kf2.waves.ripplesize.tooltip": {
          "en": "The length of the longest ripple, in world units; three shorter ones ride on it. They fade out in the distance before they can shimmer.",
          "pt-BR": "O comprimento da marola mais longa, em unidades do mundo; três mais curtas vão sobre ela. Elas somem à distância antes de poderem cintilar.",
          "es-419": "La longitud de la onda más larga, en unidades del mundo; tres más cortas van sobre ella. Se desvanecen a lo lejos antes de poder titilar."
        },
        "kf2.waves.shade": {
          "en": "Ripple shading",
          "pt-BR": "Sombreamento das marolas",
          "es-419": "Sombreado de las ondas"
        },
        "kf2.waves.shade.tooltip": {
          "en": "How much the ripples lighten the water on one slope and darken it on the other.",
          "pt-BR": "Quanto as marolas clareiam a água de um lado e a escurecem do outro.",
          "es-419": "Cuánto aclaran las ondas el agua de un lado y la oscurecen del otro."
        },
        "kf2.waves.speed": {
          "en": "Wave speed",
          "pt-BR": "Velocidade das ondas",
          "es-419": "Velocidad de las olas"
        },
        "kf2.waves.speed.tooltip": {
          "en": "How fast the swell and the ripples move. They stop when the world stops.",
          "pt-BR": "Quão rápido a ondulação e as marolas se movem. Param quando o mundo para.",
          "es-419": "Qué tan rápido se mueven el oleaje y las ondas. Se detienen cuando el mundo se detiene."
        },
        "kf2.waves.reset": {
          "en": "Reset waves",
          "pt-BR": "Restaurar ondas",
          "es-419": "Restablecer olas"
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

        bool waves = Waves.Enabled;
        if (ImGui.Checkbox(Localization.T("kf2.waves.label"), ref waves))
        {
            Waves.SetEnabled(waves);
            PatchSettings.Set(Waves.OnKey, waves);
        }
        if (ImGui.IsItemHovered()) ImGui.SetTooltip(Localization.T("kf2.waves.tooltip"));
        DrawWaves(waves);

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

    static void DrawWaves(bool on)
    {
        ImGui.BeginDisabled(!on);
        ImGui.Indent();

        Slider("kf2.waves.swell", Waves.SwellKey, ref Waves.Swell, 0f, 512f, "%.0f", false);
        Slider("kf2.waves.swellsize", Waves.SwellSizeKey, ref Waves.SwellSize, 2048f, 65536f, "%.0f", true);
        Slider("kf2.waves.ripple", Waves.RippleKey, ref WaterWaves.Distort, 0f, 200f, "%.0f", false);
        Slider("kf2.waves.ripplesize", Waves.RippleSizeKey, ref WaterWaves.Scale, 100f, 4096f, "%.0f", true);
        Slider("kf2.waves.shade", Waves.ShadeKey, ref WaterWaves.Shade, 0f, 1f, "%.2f", false);
        Slider("kf2.waves.speed", Waves.SpeedKey, ref Waves.Speed, 0f, 4f, "%.2f", false);

        if (ImGui.Button(Localization.T("kf2.waves.reset")))
        {
            Waves.Swell = Waves.DefaultSwell;
            Waves.SwellSize = Waves.DefaultSwellSize;
            WaterWaves.Distort = Waves.DefaultRipple;
            WaterWaves.Scale = Waves.DefaultRippleSize;
            WaterWaves.Shade = Waves.DefaultShade;
            Waves.Speed = Waves.DefaultSpeed;
            PatchSettings.Set(Waves.SwellKey, Waves.DefaultSwell);
            PatchSettings.Set(Waves.SwellSizeKey, Waves.DefaultSwellSize);
            PatchSettings.Set(Waves.RippleKey, Waves.DefaultRipple);
            PatchSettings.Set(Waves.RippleSizeKey, Waves.DefaultRippleSize);
            PatchSettings.Set(Waves.ShadeKey, Waves.DefaultShade);
            PatchSettings.Set(Waves.SpeedKey, Waves.DefaultSpeed);
            Waves.Changed();
        }

        ImGui.Unindent();
        ImGui.EndDisabled();
    }

    static void Slider(string label, string key, ref float value, float min, float max, string format, bool log)
    {
        var flags = ImGuiSliderFlags.AlwaysClamp | (log ? ImGuiSliderFlags.Logarithmic : ImGuiSliderFlags.None);
        if (ImGui.SliderFloat(Localization.T(label), ref value, min, max, format, flags))
        {
            PatchSettings.Set(key, value);
            Waves.Changed();
        }
        if (ImGui.IsItemHovered(ImGuiHoveredFlags.AllowWhenDisabled))
            ImGui.SetTooltip(Localization.T(label + ".tooltip"));
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
