using System.Numerics;
using ImGuiNET;
using RecompOne.Runtime.Config;
using RecompOne.Runtime.Host;
using RecompOne.Runtime.Host.Window;

namespace Kf2.Settings;

/// <summary>
/// **The Input pane, drawn by the port rather than extended by it.**
///
/// Every other patch page joins a section through
/// <c>SettingsRegistry.Extend</c>, which appends after that section's own body.
/// For Video and Gameplay that is exactly right. For Input it was not: the
/// runtime's own pane is two tab bars, a sixteen-row binding table and a reset
/// button — about 450px of a 500px popup — so the port's four pages landed
/// *below the fold*, in an order nobody chose (none of them set
/// <c>Order</c>, so they sorted on the title ordinal and the map's pad binding
/// came out between the keyboard layout and mouse look). Worse than the
/// scrolling, the two keyboard-layout buttons write the sixteen rows of a table
/// a screen above them, and the stick settings were nowhere near the pad one.
///
/// <c>docs/INPUT.md</c> weighed this exact wrapper and **rejected** it, in favour
/// of a <c>controls</c> section of the port's own beside Input. That is reversed
/// here on a fact nobody had checked: <c>settings.input</c> already renders
/// **"Controles" in pt-BR *and* es-419**, so a second sidebar entry called
/// Controls collides head-on with the Input pane's own name in two of the three
/// languages. The general finding is worth more than this instance — *a new
/// sidebar entry has to be checked against every language of the ones already
/// there, not just English.* Splitting the pane could not have delivered the
/// point of the exercise anyway, since the layout buttons have to sit with the
/// table they write.
///
/// The old objection stands and is now a cost being paid rather than an argument
/// that lost: the port owns a section the runtime registers, in a checkout that
/// is gitignored and moves under us. What pays it is that the wrapper is cheap
/// to abandon — deleting one <c>Register</c> line in <see cref="PatchSettings"/>
/// restores the runtime's pane exactly — and that the copied half
/// (<see cref="BindingTable"/>) names its source so a pin bump has something to
/// diff.
///
/// **`Register` replaces by id, and that is the whole mechanism.**
/// <c>SettingsRegistry.Register</c> does <c>RemoveAll(s =&gt; s.Id == section.Id)</c>
/// and then adds, so registering this on <c>RuntimeReadyEvent</c> — after
/// <c>HostWindow.Load</c> has registered the runtime's five — takes the pane
/// over. Do **not** <c>Unregister("input")</c> first: it states removal where the
/// intent is substitution, and it hides the one failure that matters — if
/// upstream ever renames the id, an unregister no-ops silently and the register
/// adds a *second* Input tab. <see cref="PatchSettings"/> warns on that directly
/// instead.
///
/// **The four pages had to leave the registry, not just be reordered in it.**
/// <c>SettingsPopup</c> draws <c>current.Draw()</c> and then every
/// <c>GetExtensions(current.Id)</c>, and <c>SettingsRegistry</c> has
/// <c>Extend</c> with no un-extend — the list is append-only with no removal API
/// — so a page still registered against <c>"input"</c> would draw a second time
/// under the tab bar, outside every tab, permanently. So the registrations are
/// dropped from <c>PatchSettings.Install</c> and this class holds the instances.
///
/// <c>IPatchPage.Order</c> is inert for a page drawn here (the sequence is
/// written out below, so there is no list to sort) and <c>Title</c> keeps only
/// its degenerate half: an empty title declines the heading, which is what a
/// page alone in a tab that already names it wants.
///
/// See "The Input pane is the port's" in docs/INPUT.md.
/// </summary>
public sealed class InputSection : ISettingsSection
{
    /// <summary>The runtime's own id: this replaces its section rather than
    /// joining the sidebar beside it.</summary>
    public string Id => "input";

    /// <summary>The runtime's own key too — "Input", "Controles", "Controles" —
    /// so nothing is re-translated and the sidebar entry does not move.</summary>
    public string TitleKey => "settings.input";

    /// <summary>The runtime's own order, so the sidebar keeps its shape:
    /// interface -10, input 0, display 5, gameplay 7, audio 10, paths 20.</summary>
    public int Order => 0;

    // One capture per device rather than one index shared by both. The runtime
    // had a single _remapRow serving two devices and two pad slots and had to
    // clear it in four places; two fields make switching tab mid-capture a
    // non-event by construction instead of by remembering.
    int _keyRow = -1;
    int _padRow = -1;
    bool _padAdd;

    // The pages, in the order they draw, grouped by the device they are about.
    // MapButtonPage is here rather than under Gameplay because a player looking
    // for what a button does looks under Input; AnalogPage and MousePage are the
    // stick and pointer halves of the same keyboard-and-mouse question.
    readonly IPatchPage[] _keyboard = [new KeyLayoutPage()];
    // King's Field's own: the King's Field II port's pages were AnalogPage,
    // MapButtonPage and MousePage, which drive KF2's patches.
    readonly IPatchPage[] _gamepad = [new TwinStickPage()];
    readonly IPatchPage[] _mouse = [new MouseLookPage()];

    public void Draw()
    {
        if (!ImGui.BeginTabBar("##kf2-input")) return;

        Tab(Localization.T("settings.input.keyboard"), DrawKeyboard);
        Tab(Localization.T("settings.input.gamepad"), DrawGamepad);
        if (_mouse.Length > 0) Tab("Mouse", DrawMouse);

        ImGui.EndTabBar();
    }

    /// <summary>
    /// One tab, with its body in a scrolling child of its own.
    ///
    /// **The tab bar has to stay out of the scroll.** The table alone is about
    /// 490px — sixteen rows at a frame height plus cell padding, plus a header —
    /// against roughly 365px of tab body in a 500px popup, so every tab scrolls
    /// whatever is above it. Drawn in the flow of the settings content child, the
    /// bar would scroll off the top with everything else and the pane would be
    /// worse than the one it replaces. The child takes the remaining height and
    /// owns the scrollbar; the padding push is around <c>BeginChild</c> only —
    /// patch 0031's shape — so the body is not inset a second time inside a
    /// content child that has already padded it.
    /// </summary>
    static void Tab(string title, Action body)
    {
        if (!ImGui.BeginTabItem(title)) return;

        ImGui.PushStyleVar(ImGuiStyleVar.WindowPadding, Vector2.Zero);
        bool open = ImGui.BeginChild("##body", Vector2.Zero, ImGuiChildFlags.None);
        ImGui.PopStyleVar();

        if (open) body();

        ImGui.EndChild();
        ImGui.EndTabItem();
    }

    void DrawKeyboard()
    {
        Pages(_keyboard);
        ImGui.Spacing();

        BindingTable.Draw(gamepad: false, ref _keyRow, ref _padAdd);
        ActionNote();

        ImGui.Spacing();

        // KeyLayout.ApplyStock, not `Keys = new KeyBindings()` as the runtime
        // did: the same object, plus the marker that stops KeyLayout.Install's
        // next-launch migration putting the port's layout back over it. The
        // "RecompOne layout" button a few lines above does exactly this, and two
        // buttons that agree on screen have to agree in code — which they could
        // afford not to while they were 450px apart and are now adjacent.
        if (ImGui.Button(Localization.T("settings.input.reset_defaults")))
        {
            KeyLayout.ApplyStock();
            _keyRow = -1;
        }
    }

    void DrawGamepad()
    {
        // Above the pages, not below the table where the runtime had it: this is
        // the answer to "why is none of this doing anything", so it has to be met
        // before the twin-stick block rather than after the sixteen rows.
        if (!HostWindow.IsPadConnected(0))
        {
            ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.75f, 0.3f, 1f));
            ImGui.TextWrapped(Localization.T("settings.input.no_gamepad"));
            ImGui.PopStyleColor();
            ImGui.Spacing();
        }

        Pages(_gamepad);
        ImGui.Spacing();

        BindingTable.Draw(gamepad: true, ref _padRow, ref _padAdd);
        ActionNote();

        ImGui.Spacing();

        // Nothing migrates pad bindings, so this is the plain reset the runtime
        // had. Pad2 is deliberately left alone — see BindingTable.
        if (ImGui.Button(Localization.T("settings.input.reset_defaults")))
        {
            ConfigManager.Game.Pad = new GamepadBindings();
            ConfigManager.SaveGame();
            _padRow = -1;
        }
    }

    void DrawMouse() => Pages(_mouse);

    /// <summary>The heading rule, unchanged from <c>PatchSettings.Draw</c>: an
    /// empty title declines it. Kept in one place so a page moved back out of a
    /// tab behaves identically, and <c>PushID</c> stays even though a tab item
    /// already scopes its contents — a page's widgets being namespaced should not
    /// depend on which container happens to be drawing it.</summary>
    static void Pages(IPatchPage[] pages)
    {
        foreach (var page in pages)
        {
            if (!string.IsNullOrEmpty(page.Title)) ImGui.SeparatorText(page.Title);

            ImGui.PushID(page.Id);
            page.Draw();
            ImGui.PopID();
        }
    }

    /// <summary>The qualification the action column needs, said once under the
    /// table rather than sixteen times in it. See <see cref="BindingTable"/>.</summary>
    static void ActionNote() =>
        PatchSettings.Note("That middle column is what the buttons do by default. King's Field has a " +
                           "control configuration screen of its own, and remapping there moves them.");
}
