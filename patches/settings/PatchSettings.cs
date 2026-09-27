using ImGuiNET;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Host.Window;
using Rt = RecompOne.Runtime.Runtime;

namespace Kf2.Settings;

/// <summary>A patch's settings, drawn with ImGui.</summary>
public interface IPatchPage
{
    /// <summary>Stable id; also the ImGui id scope the page is drawn under.</summary>
    string Id { get; }

    /// <summary>The heading the page sits under, inside the section it joins.
    /// **Pages that give the same title share one heading** — a single checkbox
    /// does not deserve a rule and a name of its own, so several of them can sit
    /// together under "Enhancements" while something with real structure, like the
    /// frame rate, keeps its own.
    ///
    /// The title is the heading and nothing else: it used to be the sort key too,
    /// which made the order of the port's groups an accident of how they were
    /// spelled — "Enhancements" drew above "Frame pacing" because E sorts before
    /// F. <see cref="Order"/> is the sort key now.
    ///
    /// **An empty title declines the heading**, which is the degenerate case of
    /// that sharing rule: <c>SettingsPopup</c> draws a rule naming the section
    /// before any extension runs, so a section whose pages are the whole pane —
    /// <see cref="GameplaySection"/> is the one — gets a second rule immediately
    /// under the first with nothing between them. The pages still get their
    /// spacing and their order; they just do not name themselves twice.</summary>
    string Title { get; }

    /// <summary>Where the page sits among the section's other pages, low first.
    ///
    /// Defaulted, so a page that does not care states nothing and falls back to
    /// the title comparison it had before. **Pages sharing a <see cref="Title"/>
    /// have to be given adjacent orders**: <see cref="PatchSettings.Draw"/> opens
    /// a heading whenever the title changes, so a page separating two of them
    /// draws the heading twice.
    ///
    /// **A page drawn by a section wrapper is a body, not a page**, and this is
    /// inert for one: <see cref="InputSection"/> writes its sequence out in three
    /// small arrays, so there is no list to sort and no tie to break. Only the
    /// degenerate half of the <see cref="Title"/> rule survives there — an empty
    /// title declines the heading — because the tab is already the heading.</summary>
    int Order => 0;

    /// <summary>
    /// Draw the page's controls.
    ///
    /// **A tooltip says what the control <em>does</em>, in one short line.** These
    /// were each a small essay — the crosshatch named as 4x4, affine mapping,
    /// whole-pixel vertices, the 15-bit frame buffer, the tick the picture trails
    /// by. All of that is true and none of it is a player's question, which is only
    /// ever "what changes if I tick this". The mechanism is written up in the
    /// patch's own doc comment and in <c>docs/</c>, and the numbers that justify it
    /// belong on the console behind the probe env vars; a hover in a settings
    /// window is not the place for either. Where a control genuinely needs a
    /// paragraph — the frame rate's consequences, the smoothing being inert at the
    /// tick rate — that goes in a wrapped note under the control, where it can be
    /// read rather than raced against a mouse.
    /// </summary>
    void Draw();
}

/// <summary>
/// Where a patch's settings live: inside the runtime's own settings sections.
///
/// Mods get a settings UI for free -- <c>IMod.DrawSettings</c> is drawn under the
/// gear button in the Mods popup -- and patches got nothing, so a patch's only
/// knob was an environment variable read once at startup. That is the real cost
/// of moving something out of <c>mods/</c> and into <c>patches/</c>: the code
/// keeps working and the whole panel disappears.
///
/// The replacement is not a panel of our own. <c>SettingsRegistry.Extend</c> takes
/// a section id and a draw callback and runs it after that section's own content,
/// so a patch's settings can go where a user would already look for them -- the
/// frame rate belongs under Video, beside vsync and render scale, and not in a
/// King's Field box off to one side. A patch registers a page against a section:
///
/// <code>
/// PatchSettings.Register("display", new FramePacingPage());
/// </code>
///
/// The section ids are the runtime's own: <c>interface</c>, <c>display</c>,
/// <c>paths</c>, <c>audio</c> — plus <c>gameplay</c>, the one section the port
/// adds itself (see <see cref="GameplaySection"/>), for patches that change how
/// the game plays rather than how the machine behaves. An id that matches no
/// section is reported at startup rather than silently drawing nothing.
///
/// <c>input</c> is not on that list any more, and it is the other way the port can
/// own a pane: <see cref="InputSection"/> **replaces** the runtime's section
/// rather than extending it, because <c>Extend</c> can only append and the
/// runtime's own Input body fills the popup. Registering a page against
/// <c>"input"</c> is refused with a message, since <c>Extend</c> has no un-extend
/// and such a page would draw outside every tab forever.
///
/// Settings persist through <see cref="Set(string,bool)"/> and friends, which is
/// <c>Runtime.View</c> plus an immediate <c>SaveView</c> -- the same
/// <c>interface.ini</c> store the mods use -- keyed <c>kf2.&lt;patch&gt;.&lt;name&gt;</c>.
/// Note that <c>ConfigManager.Load()</c> runs inside <c>HostWindow.Initialize</c>,
/// i.e. **after** Program.cs: a patch that wants a persisted default has to read
/// it on <c>RuntimeReadyEvent</c> and not in its own <c>Configure</c>, or it will
/// read a fresh empty config and then overwrite the file with it.
/// </summary>
public static class PatchSettings
{
    static readonly Dictionary<string, List<IPatchPage>> _pages = new(StringComparer.OrdinalIgnoreCase);
    static readonly Dictionary<string, List<IPatchPage>> _slots = new(StringComparer.OrdinalIgnoreCase);

    static bool _installed;
    static bool _registered;

    /// <summary>
    /// Add a page to one of the runtime's settings sections. Registering the same
    /// page id again replaces it, so a patch reloaded during development does not
    /// stack up duplicates.
    ///
    /// The list is kept in <see cref="IPatchPage.Order"/> order, ties broken by
    /// title, so the order the pages are registered in below carries no meaning —
    /// keep it matching the drawn order anyway, so the file reads as the pane does.
    /// </summary>
    public static void Register(string sectionId, IPatchPage page)
    {
        if (string.IsNullOrWhiteSpace(sectionId) || page == null) return;

        // "input" is the port's own pane now (InputSection), and Extend has no
        // un-extend -- a page registered here would draw under the tab bar,
        // outside every tab, permanently. Add it to one of InputSection's tab
        // lists instead.
        if (string.Equals(sectionId, "input", StringComparison.OrdinalIgnoreCase))
        {
            Console.Error.WriteLine($"[KF2] settings: \"{page.Id}\" registered against " +
                                    "\"input\", which the port draws itself; add it to " +
                                    "InputSection's tabs instead");
            return;
        }

        if (!_pages.TryGetValue(sectionId, out var list))
            _pages[sectionId] = list = [];

        list.RemoveAll(p => p.Id == page.Id);
        list.Add(page);
        list.Sort((a, b) => a.Order != b.Order
            ? a.Order.CompareTo(b.Order)
            : string.Compare(a.Title, b.Title, StringComparison.Ordinal));
    }

    /// <summary>
    /// Add a page **inside** a section's own body, at a slot the section draws.
    ///
    /// <see cref="Register"/> appends after the section has drawn everything it
    /// has, which is where a group of the port's own belongs. An option that is
    /// one of the section's ordinary options does not belong there: an aspect
    /// ratio is the same kind of choice as the render scale and wants to be next
    /// to it, above the backend combo, not in a block underneath the lot.
    /// <c>SettingsRegistry.DrawSlot</c> is that — <c>patches/recompone/0013</c>,
    /// which is the only reason this needs the checkout patched at all.
    ///
    /// A slot page draws **bare**: no <c>SeparatorText</c>, no heading, so it sits
    /// in line with the section's own widgets and <see cref="IPatchPage.Title"/>
    /// goes unused. Note the one asymmetry with <see cref="Register"/>: a section
    /// id that does not exist is reported at startup, and a **slot id nothing
    /// draws is silent** — there is no list of slots to check it against.
    /// </summary>
    public static void RegisterSlot(string slotId, IPatchPage page)
    {
        if (string.IsNullOrWhiteSpace(slotId) || page == null) return;

        if (!_slots.TryGetValue(slotId, out var list))
            _slots[slotId] = list = [];

        list.RemoveAll(p => p.Id == page.Id);
        list.Add(page);
    }

    /// <summary>
    /// Hand the pages to the settings UI. Waits for <c>RuntimeReadyEvent</c>, which
    /// is dispatched at the end of <c>Runtime.Initialize</c> -- after the host
    /// window's Load has run, so the runtime's own sections are all registered and
    /// a bad section id can be caught. Program.cs runs before any of that.
    /// </summary>
    public static void Install()
    {
        if (_installed) return;
        _installed = true;

        // King's Field has the pages whose features do not depend on the game's own
        // routines. The King's Field II port also registered FramePacingPage,
        // FrameSmoothingPage, PerPixelLightingPage, EvenFogPage, ReflectionsPage,
        // RemasterPage, RemasterPacksPage and FastGeometryPage under display, and
        // MapPage, AutoReloadPage and MouseLeadPage under gameplay; each comes back
        // with the patch it controls (docs/KF1.md, "What came across from King's
        // Field II").
        Register("display", new PerspectivePage());
        Register("display", new SubpixelPage());
        Register("display", new ShadingPage());
        Register("display", new AmbientOcclusionPage());
        Register("display", new AnisotropicPage());
        Register("display", new ZBufferPage());
        // Nothing registers a PGXP or a geometry-precision page either. PGXP buys
        // no coverage in this game and costs a fifth of the frame rate, and the
        // depth buffer's picture has never been judged -- both are comparisons, so
        // they are KF2_PGXP* and KF2_ZBUFFER* on the console and nothing in Video.
        RegisterSlot("display.render_scale", new WidescreenPage());
        Register("audio", new AudioPage());
        // Nothing registers against "input": the port draws that whole pane
        // itself (InputSection), and SettingsRegistry.Extend has no un-extend --
        // a page left here would draw a second time under the tab bar, outside
        // every tab, with no way to take it back.
        Event.AddListener<RuntimeReadyEvent>(_ => RegisterUi());
    }

    /// <summary>
    /// The section the port adds to is called Video, not Display.
    ///
    /// "Display" reads as where the window lives; everything in that section is how
    /// the picture is made, and the port puts a frame rate and a dither switch in
    /// beside vsync and render scale. Renaming it needs no patch to the RecompOne
    /// checkout: the tab is drawn from <c>Localization.T("settings.display")</c>,
    /// and <c>Merge</c> is public and overwrites by key.
    ///
    /// Only English is overridden for that one, because the runtime's other two
    /// languages already say exactly this — pt-BR "Vídeo", es-419 "Video". The
    /// **id stays <c>display</c>**, so <see cref="Register"/>,
    /// <c>SettingsRegistry</c> and the runtime's own section are untouched; only
    /// the label moves.
    ///
    /// <c>settings.gameplay</c> is the opposite case: a key the runtime has never
    /// heard of, for the section the port adds. A new key has to supply every
    /// language, since <c>Localization.T</c> falls back to English with a warning
    /// and then to printing the key itself.
    /// </summary>
    const string SectionNames = """
    {
      "strings": {
        "settings.display": { "en": "Video" },
        "settings.gameplay": {
          "en": "Gameplay",
          "pt-BR": "Jogabilidade",
          "es-419": "Jugabilidad"
        }
      }
    }
    """;

    static void RegisterUi()
    {
        if (_registered) return;
        _registered = true;

        Localization.Merge(SectionNames);

        // Before the section-id check below, and before the sidebar is first
        // drawn: the runtime's five sections are registered inside HostWindow's
        // Load, and RuntimeReadyEvent is dispatched after it, so this is the
        // earliest moment a sixth can join them and still be in place for the
        // first frame of the settings popup.
        // Only with something to put in it: nothing registers against "gameplay"
        // until a gameplay feature is ported to King's Field.
        if (_pages.ContainsKey("gameplay")) SettingsRegistry.Register(new GameplaySection());

        // Input is a *replacement*, not an addition: Register removes by id, so
        // this takes the runtime's own pane over. If upstream ever renames that
        // id we would silently add a second Input tab rather than replacing the
        // first, which is the one failure worth naming out loud.
        if (!SectionExists("input"))
            Console.Error.WriteLine("[KF2] settings: no \"input\" section to replace; " +
                                    "the port's Input pane will be a second tab");
        SettingsRegistry.Register(new InputSection());

        foreach (var (sectionId, pages) in _pages)
        {
            if (!SectionExists(sectionId))
            {
                Console.Error.WriteLine($"[KF2] settings: no section \"{sectionId}\"; " +
                                        $"{string.Join(", ", pages.Select(p => p.Title))} will not be shown");
                continue;
            }

            var group = pages;
            SettingsRegistry.Extend(sectionId, () => Draw(group));
        }

        // Slots go through the same Extend table, keyed by the slot id the section
        // passes to DrawSlot rather than by a section id, and draw without headings.
        foreach (var (slotId, pages) in _slots)
        {
            var group = pages;
            SettingsRegistry.Extend(slotId, () => DrawBare(group));
        }
    }

    static bool SectionExists(string sectionId)
    {
        foreach (var section in SettingsRegistry.Sections)
            if (string.Equals(section.Id, sectionId, StringComparison.OrdinalIgnoreCase)) return true;
        return false;
    }

    /// <summary>
    /// Drawn after the section's own content, so the rule above it is what marks
    /// where the runtime's settings end and the port's begin. Pages are sorted by
    /// title, so pages sharing one get drawn together under a single heading.
    /// </summary>
    static void Draw(List<IPatchPage> pages)
    {
        string? heading = null;
        foreach (var page in pages)
        {
            if (page.Title != heading)
            {
                heading = page.Title;
                ImGui.Spacing();
                // An empty title declines the heading: the section's own rule is
                // already above it and a second one under it, with nothing in
                // between, is a rule for its own sake. See IPatchPage.Title.
                if (!string.IsNullOrEmpty(heading)) ImGui.SeparatorText(heading);
            }

            ImGui.PushID(page.Id);
            page.Draw();
            ImGui.PopID();
        }
    }

    /// <summary>A slot's pages, drawn in line with the section's own controls —
    /// no heading, since the point of a slot is that the option is one of the
    /// section's ordinary options.</summary>
    static void DrawBare(List<IPatchPage> pages)
    {
        foreach (var page in pages)
        {
            ImGui.PushID(page.Id);
            page.Draw();
            ImGui.PopID();
        }
    }

    // interface.ini, saved on the spot: a setting is changed once and then the
    // player goes back to the game, and there is no later moment to write it.

    public static bool Get(string key, bool fallback) => Rt.View.GetBool(key, fallback);

    public static int Get(string key, int fallback) => Rt.View.GetInt(key, fallback);

    public static float Get(string key, float fallback) => Rt.View.GetFloat(key, fallback);

    public static void Set(string key, bool value)
    {
        Rt.View.SetBool(key, value);
        Rt.SaveView();
    }

    public static void Set(string key, int value)
    {
        Rt.View.SetInt(key, value);
        Rt.SaveView();
    }

    public static void Set(string key, float value)
    {
        Rt.View.SetFloat(key, value);
        Rt.SaveView();
    }

    /// <summary>
    /// A dimmed, wrapped line under a control, for the paragraph a tooltip is the
    /// wrong place for — why a control is inert at this frame rate, what the rate
    /// does not change. <c>TextDisabled</c> does not wrap and unwrapped prose runs
    /// straight out of the settings window, so it is the colour plus
    /// <c>TextWrapped</c> rather than the one call it looks like.
    /// </summary>
    public static void Note(string text)
    {
        ImGui.PushStyleColor(ImGuiCol.Text, ImGui.GetStyle().Colors[(int)ImGuiCol.TextDisabled]);
        ImGui.TextWrapped(text);
        ImGui.PopStyleColor();
    }
}
