// KF1's first-run setup (verdite/setup.h): its pages and tiers over the option
// table, and the Dear ImGui screen that draws it on a desktop. Android has no
// screen for it yet, so there it is neither shown nor marked as seen.
#include "internal.hpp"
#include <imgui.h>
#include <verdite/setup.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kf::ext {
namespace {
using verdite::OptionValue;
using verdite::SetupChoice;
using verdite::SetupGroup;
using verdite::SetupOptionItem;
using verdite::SetupPage;
using verdite::SetupPageTiers;
using verdite::SetupPreset;
using verdite::SetupSpec;
using verdite::SetupValue;

// Raise to ask every player again.
constexpr int setup_version = 1;

SetupChoice choice(std::string label, std::string help, std::vector<std::string> tiers, std::vector<SetupValue> values) {
    return {std::move(label), std::move(help), std::move(tiers), std::move(values), false};
}

SetupChoice costly(SetupChoice made) {
    made.higher_gpu_cost = true;
    return made;
}

SetupOptionItem option(std::string id) { return {std::move(id), std::nullopt}; }

// The aspect of the monitor the window is on, as the nearest choice.
std::string suggested_aspect() {
    const SDL_DisplayID display = window() ? SDL_GetDisplayForWindow(window()) : 0;
    const SDL_DisplayMode *mode = display ? SDL_GetDesktopDisplayMode(display) : nullptr;
    if (!mode || mode->w <= 0 || mode->h <= 0)
        return "16:9";
    const double ratio = static_cast<double>(mode->w) / mode->h;
    constexpr std::array<std::pair<const char *, double>, 4> aspects = {
        {{"4:3", 4.0 / 3.0}, {"16:10", 1.6}, {"16:9", 16.0 / 9.0}, {"21:9", 64.0 / 27.0}}};
    const auto nearest = std::ranges::min_element(aspects, {}, [ratio](const auto &each) { return std::abs(each.second - ratio); });
    return nearest->first;
}

std::vector<SetupValue> preset_values(std::string aspect, double resolution, double frame_rate) {
    return {{"gameplay.classic_mode", false},
            {"display.aspect", std::move(aspect)},
            {"display.resolution", resolution},
            {"display.frame_rate", frame_rate}};
}

std::vector<SetupPreset> presets() {
    const std::string aspect = suggested_aspect();
    std::vector<SetupValue> original = preset_values("4:3", 1, 20);
    original.push_back({"gameplay.enemy_turn", 1.0});
    return {
        {"original", "Original",
         "King's Field as the PlayStation showed it: 4:3 at 320x240, one frame per update, wobbling polygons and "
         "15-bit colour, with the original controls.",
         "original", std::move(original)},
        {"balanced", "Balanced",
         "The original look and pace, with what the hardware got wrong put right: steady polygons, smooth colour, "
         "even texels, the full frame rate, and the sticks.",
         "balanced", preset_values(aspect, 0, 0)},
        {"modern", "Modern",
         "Everything Verdite adds: ambient occlusion, anti-aliasing, the minimap and the rest, at the frame rate of "
         "your display.",
         "modern", preset_values(aspect, 0, 0)},
        {"custom", "Custom", "Start from your settings as they are, and choose each page yourself.", "", {{"gameplay.classic_mode", false}}},
    };
}

SetupPage display_page() {
    return {"display", "Display", "Suggested for your monitor; change anything here.",
            {option("display.fullscreen"), option("display.aspect"), option("display.hud_at_edges"),
             option("display.resolution"), option("display.frame_rate")},
            SetupPageTiers::Hidden};
}

SetupPage picture_page() {
    const SetupGroup polygons{"polygons", "Polygons", "", {
        choice("Wobbling and warping", "Corners snap to the 320x240 grid and textures bend, as on the PlayStation.",
               {"original"}, {{"display.subpixel", false}, {"display.perspective", false}}),
        choice("Steady", "Corners between pixels and perspective-correct textures.", {"balanced", "modern"},
               {{"display.subpixel", true}, {"display.perspective", true}}),
    }};
    const SetupGroup colour{"colour", "Colour", "", {
        choice("15-bit with dither", "The PlayStation's colour depth and its dither pattern.", {"original"},
               {{"display.true_color", false}}),
        choice("Smooth", "Full colour: smooth gradients in shadows and fog.", {"balanced", "modern"},
               {{"display.true_color", true}}),
    }};
    const SetupGroup textures{"textures", "Textures", "", {
        choice("Blocky, as on the PlayStation", "Nearest texels, shimmering in the distance, coarse distant walls.",
               {"original"},
               {{"display.texture_filter", 0.0}, {"display.mipmaps", false}, {"display.anisotropy", 1.0},
                {"display.detailed_map", false}}),
        choice("Sharp, steady texels", "The original texels drawn evenly, without shimmer, sharp at a slant.",
               {"balanced", "modern"},
               {{"display.texture_filter", 3.0}, {"display.mipmaps", true}, {"display.anisotropy", 16.0},
                {"display.detailed_map", true}}),
        choice("Smooth (bilinear)", "Texels blended into each other.", {},
               {{"display.texture_filter", 1.0}, {"display.mipmaps", true}, {"display.anisotropy", 16.0},
                {"display.detailed_map", true}}),
    }};
    const SetupGroup lighting{"lighting", "Lighting", "", {
        choice("The game's, at the corners", "Light and fog blended across each polygon, as on the PlayStation.",
               {"original"},
               {{"display.pixel_lighting", false}, {"display.ambient_occlusion", false}, {"display.fog", 0.0}}),
        choice("The game's, per pixel", "The same lights and fog worked out at every pixel.", {"balanced"},
               {{"display.pixel_lighting", true}, {"display.ambient_occlusion", false}, {"display.fog", 2.0}}),
        costly(choice("Per pixel, with ambient occlusion", "Plus soft shadowing in corners and under ledges.",
                      {"modern"},
                      {{"display.pixel_lighting", true}, {"display.ambient_occlusion", true}, {"display.fog", 2.0}})),
    }};
    const SetupGroup edges{"edges", "Edges", "", {
        choice("Jagged", "No anti-aliasing.", {"original", "balanced"}, {{"display.antialiasing", 0.0}}),
        choice("Smoothed (FXAA)", "Nearly free; the HUD stays sharp.", {"modern"}, {{"display.antialiasing", 1.0}}),
        costly(choice("Supersampled (4x)", "Four times the pixels, averaged: cleanest.", {},
                      {{"display.antialiasing", 2.0}})),
    }};
    return {"picture", "Picture", "", {polygons, colour, textures, lighting, edges}, SetupPageTiers::Offered};
}

SetupPage controls_page() {
    const SetupGroup running{"running", "Running", "", {
        choice("No run key", "Walk at the original's one speed.", {"original"}, {{"controls.run_speed", 100.0}}),
        choice("Run key", "Shift, or the left stick's click, runs at 1.6 times walking speed.", {"balanced", "modern"},
               {{"controls.run_speed", 160.0}}),
    }};
    const SetupGroup looking{"looking", "Looking up and down", "", {
        choice("The original's 17 degrees", "", {"original"}, {{"controls.look_limit", 17.0}}),
        choice("Up to 70 degrees", "Look at the ceiling and the floor at your feet.", {"balanced", "modern"},
               {{"controls.look_limit", 70.0}}),
    }};
    return {"controls", "Controls", "", {running, looking}, SetupPageTiers::Offered};
}

SetupPage turning_page() {
    const SetupGroup sticks{"sticks", "Turning", "", {
        choice("D-pad and keys only", "The sticks work as the D-pad, as the game came out before the DualShock.",
               {"original"}, {{"analog.enabled", false}}),
        choice("Twin stick", "The left stick walks and strafes, the right turns and looks, at the D-pad's speed.",
               {"balanced", "modern"},
               {{"analog.enabled", true}, {"analog.sticks", 0.0}, {"analog.look_acceleration", false}}),
        choice("Twin stick with acceleration",
               "Holding the right stick out turns faster over time, up to 2.2 times the D-pad's speed.", {},
               {{"analog.enabled", true}, {"analog.sticks", 0.0}, {"analog.look_acceleration", true}}),
        choice("One stick (tank)", "The left stick walks and turns; L1 and R1 strafe.", {},
               {{"analog.enabled", true}, {"analog.sticks", 1.0}}),
    }};
    const SetupGroup enemies{"enemy_turn", "Enemy turn speed", "", {
        choice("Fixed", "Enemies turn at the speed below, whatever you play with. x1 is the original.", {"original"},
               {{"gameplay.enemy_turn_auto", false}}),
        choice("Automatic",
               "Enemies keep up with how fast you can turn: as the original with the D-pad, keys or a stick at "
               "its speed, faster with acceleration, three times as fast with the mouse.",
               {"balanced", "modern"}, {{"gameplay.enemy_turn_auto", true}}),
    }};
    return {"turning", "Turning",
            "The fights were built on the D-pad's fixed turn. Turning faster than that, with the mouse or a stick's "
            "acceleration, lets you circle an enemy it cannot follow; faster enemies put that right.",
            {sticks, enemies, SetupOptionItem{"gameplay.enemy_turn", SetupValue{"gameplay.enemy_turn_auto", false}}},
            SetupPageTiers::Hidden};
}

SetupPage gameplay_page() {
    const SetupGroup fixes{"fixes", "Bug fixes", "", {
        choice("As released", "The original's bugs left in.", {"original"},
               {{"gameplay.fix_ceiling_fall", false}, {"gameplay.fix_spatial_panning", false}}),
        choice("Fixed", "Enemies fall after hitting a ceiling; flagged sounds pan as meant.", {"balanced", "modern"},
               {{"gameplay.fix_ceiling_fall", true}, {"gameplay.fix_spatial_panning", true}}),
    }};
    const SetupGroup help{"help", "Help", "", {
        choice("None", "Keys chosen from the item menu, no item descriptions, no minimap.", {"original"},
               {{"gameplay.auto_use_keys", false}, {"overlay.item_descriptions", false}, {"map.minimap", false}}),
        choice("Light", "A carried key opens its lock; items say what they do. No minimap.", {"balanced"},
               {{"gameplay.auto_use_keys", true}, {"overlay.item_descriptions", true}, {"map.minimap", false}}),
        choice("Full", "As Light, with a minimap of what you have seen.", {"modern"},
               {{"gameplay.auto_use_keys", true}, {"overlay.item_descriptions", true}, {"map.minimap", true}}),
    }};
    return {"gameplay", "Gameplay", "", {fixes, help}, SetupPageTiers::Offered};
}

SetupSpec kf1_spec() {
    SetupSpec spec;
    spec.version = setup_version;
    spec.tiers = {{"original", "Original", "As the PlayStation ran it."},
                  {"balanced", "Balanced", "The original, with what the hardware got wrong put right."},
                  {"modern", "Modern", "Everything Verdite adds."}};
    spec.presets = presets();
    spec.pages = {display_page(), picture_page(), controls_page(), turning_page(), gameplay_page()};
    return spec;
}

// ---- The screens ----

using Control = std::function<void(verdite::Setup &)>;

// Dear ImGui draws every frame, so the screen keeps the last view and draws it
// in setup_draw; a control waits there until the frame is drawn, since running
// it replaces the view being drawn.
class OverlayScreen final : public verdite::SetupScreen {
public:
    void show(const verdite::SetupPageView &view) override {
        view_ = view;
        focus_first_ = true;
    }
    void close() override { view_.reset(); }
    bool open() const { return view_.has_value(); }
    void draw(std::vector<Control> &controls);

private:
    void draw_welcome(const verdite::SetupPageView &view, std::vector<Control> &controls);
    void draw_page(const verdite::SetupPageView &view, std::vector<Control> &controls);
    static void draw_summary(const verdite::SetupPageView &view);
    void draw_footer(const verdite::SetupPageView &view, std::vector<Control> &controls);

    std::optional<verdite::SetupPageView> view_;
    bool focus_first_ = false;
};

// Headless: nothing is drawn; the agent channel drives it.
class UndrawnScreen final : public verdite::SetupScreen {
public:
    void show(const verdite::SetupPageView &) override { open_ = true; }
    void close() override { open_ = false; }
    bool open() const { return open_; }

private:
    bool open_ = false;
};

struct SetupState {
    SetupSpec spec;
    std::optional<verdite::Setup> setup;
    OverlayScreen overlay_screen;
    UndrawnScreen undrawn_screen;
    bool drawn = false;   // Dear ImGui draws this run
    std::vector<Control> controls;
};
SetupState state;

void dimmed(const std::string &text) {
    if (text.empty())
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void focus_here(bool &first) {
    if (std::exchange(first, false))
        ImGui::SetKeyboardFocusHere();
}

void OverlayScreen::draw_welcome(const verdite::SetupPageView &view, std::vector<Control> &controls) {
    ImGui::TextUnformatted("Welcome to Verdite1");
    ImGui::Separator();
    ImGui::TextWrapped("Choose where to start. Each is only a starting point: the pages after it show every choice, "
                       "and everything can be changed later in the settings (F1).");
    ImGui::Spacing();
    const float width = ImGui::GetContentRegionAvail().x;
    for (const SetupPreset *preset : view.presets) {
        ImGui::PushID(preset->id.c_str());
        focus_here(focus_first_);
        if (ImGui::Button(preset->label.c_str(), ImVec2(width, 0)))
            controls.emplace_back([id = preset->id](verdite::Setup &setup) { setup.choose_preset(id); });
        dimmed(preset->help);
        ImGui::Spacing();
        ImGui::PopID();
    }
}

std::string choice_label(const SetupChoice &each) {
    return each.higher_gpu_cost ? each.label + "  (higher GPU cost)" : each.label;
}

void draw_group(const verdite::SetupGroupView &view, std::vector<Control> &controls, bool &focus_first) {
    const SetupGroup &group = *view.group;
    ImGui::PushID(group.id.c_str());
    const std::string preview = view.chosen ? choice_label(group.choices[*view.chosen]) : std::string("Keep as is");
    focus_here(focus_first);
    if (ImGui::BeginCombo(group.label.c_str(), preview.c_str())) {
        for (std::size_t i = 0; i < group.choices.size(); ++i)
            if (ImGui::Selectable(choice_label(group.choices[i]).c_str(), view.chosen == i))
                controls.emplace_back([id = group.id, i](verdite::Setup &setup) { setup.choose(id, i); });
        ImGui::EndCombo();
    }
    if (view.chosen)
        dimmed(group.choices[*view.chosen].help);
    ImGui::PopID();
}

void draw_option(const verdite::OptionRowView &view, std::vector<Control> &controls, bool &focus_first) {
    const verdite::OptionRow &row = *view.row;
    const auto set = [&controls, id = row.id](OptionValue value) {
        controls.emplace_back([id, value = std::move(value)](verdite::Setup &setup) { setup.set_option(id, value); });
    };
    ImGui::PushID(row.id.c_str());
    focus_here(focus_first);
    switch (row.kind) {
    case verdite::OptionKind::Toggle: {
        bool on = std::get<bool>(view.value);
        if (ImGui::Checkbox(row.label.c_str(), &on))
            set(on);
        break;
    }
    case verdite::OptionKind::Choice:
        if (ImGui::BeginCombo(row.label.c_str(), verdite::option_value_label(row, view.value).c_str())) {
            for (const auto &each : row.choices)
                if (ImGui::Selectable(each.label.c_str(), each.value == view.value))
                    set(each.value);
            ImGui::EndCombo();
        }
        break;
    case verdite::OptionKind::Number: {
        float value = static_cast<float>(std::get<double>(view.value));
        if (slider_input(row.label.c_str(), &value, static_cast<float>(row.min), static_cast<float>(row.max),
                         row.format.empty() ? "%.2f" : row.format.c_str(), static_cast<float>(row.step)))
            set(static_cast<double>(value));
        break;
    }
    case verdite::OptionKind::Action:
    case verdite::OptionKind::Status:
        break;
    }
    if (!row.help.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", row.help.c_str());
    ImGui::PopID();
}

void OverlayScreen::draw_page(const verdite::SetupPageView &view, std::vector<Control> &controls) {
    const SetupPage &page = *view.spec_page;
    ImGui::TextUnformatted(page.title.c_str());
    ImGui::Separator();
    if (!page.text.empty())
        ImGui::TextWrapped("%s", page.text.c_str());
    if (!view.tiers.empty()) {
        ImGui::TextUnformatted("Set everything to:");
        for (std::size_t t = 0; t < view.tiers.size(); ++t) {
            const verdite::SetupTier &tier = *view.tiers[t];
            ImGui::SameLine();
            const bool lit = view.page_tier == t;
            if (lit)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(tier.label.c_str()))
                controls.emplace_back([id = tier.id](verdite::Setup &setup) { setup.choose_page_tier(id); });
            if (lit)
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("%s", tier.help.c_str());
        }
    }
    ImGui::Spacing();
    for (const verdite::SetupItemView &item : view.items) {
        if (const auto *group = std::get_if<verdite::SetupGroupView>(&item))
            draw_group(*group, controls, focus_first_);
        else
            draw_option(std::get<verdite::OptionRowView>(item), controls, focus_first_);
        ImGui::Spacing();
    }
}

void OverlayScreen::draw_summary(const verdite::SetupPageView &view) {
    ImGui::TextUnformatted("Summary");
    ImGui::Separator();
    if (view.changes.empty()) {
        ImGui::TextWrapped("Nothing changes: your settings already match.");
        return;
    }
    ImGui::TextWrapped("Finish changes these settings:");
    if (!ImGui::BeginTable("changes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        return;
    for (const verdite::SetupChange &change : view.changes) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(change.option_label.data(), change.option_label.data() + change.option_label.size());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", change.from.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(change.to.c_str());
    }
    ImGui::EndTable();
}

void OverlayScreen::draw_footer(const verdite::SetupPageView &view, std::vector<Control> &controls) {
    ImGui::Separator();
    const verdite::SetupStep step = view.step;
    if (step != verdite::SetupStep::Welcome) {
        if (ImGui::Button("Back"))
            controls.emplace_back([](verdite::Setup &setup) { setup.back(); });
        ImGui::SameLine();
    }
    if (step == verdite::SetupStep::Page) {
        if (ImGui::Button("Next"))
            controls.emplace_back([](verdite::Setup &setup) { setup.next(); });
        ImGui::SameLine();
        ImGui::TextDisabled("Page %zu of %zu", view.page + 1, view.page_count);
        ImGui::SameLine();
    }
    if (step == verdite::SetupStep::Summary) {
        focus_here(focus_first_);
        if (ImGui::Button("Finish"))
            controls.emplace_back([](verdite::Setup &setup) { setup.finish(); });
        ImGui::SameLine();
    }
    if (ImGui::Button("Skip setup"))
        controls.emplace_back([](verdite::Setup &setup) { setup.skip(); });
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Keep your settings as they are. Setup is in F1 > Settings > Interface.");
}

void OverlayScreen::draw(std::vector<Control> &controls) {
    if (!view_)
        return;
    const verdite::SetupPageView &view = *view_;
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float scale = settings().ui_scale;
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(620.0f * scale, viewport->Size.x * 0.95f),
                                    std::min(560.0f * scale, viewport->Size.y * 0.95f)),
                             ImGuiCond_Always);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Setup", nullptr, flags)) {
        ImGui::End();
        return;
    }
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
    ImGui::BeginChild("body", ImVec2(0, -footer));
    ImGui::PushItemWidth(-ImGui::GetContentRegionAvail().x * 0.45f);
    switch (view.step) {
    case verdite::SetupStep::Welcome:
        draw_welcome(view, controls);
        break;
    case verdite::SetupStep::Page:
        draw_page(view, controls);
        break;
    case verdite::SetupStep::Summary:
        draw_summary(view);
        break;
    }
    ImGui::PopItemWidth();
    ImGui::EndChild();
    draw_footer(view, controls);
    ImGui::End();
}

void mark_seen(int version) {
    settings().setup_seen = version;
    settings_mark_dirty();
}

// Every value the spec would write must be one its row takes, or Finish drops it.
void check_spec(const SetupSpec &spec) {
    int values = 0, problems = 0;
    const auto check = [&](const SetupValue &value) {
        ++values;
        if (options().check(value.option, value.value))
            return;
        ++problems;
        std::printf("[setup] %s: not a value it takes\n", value.option.c_str());
    };
    for (const SetupPreset &preset : spec.presets)
        std::ranges::for_each(preset.values, check);
    for (const SetupPage &page : spec.pages)
        for (const verdite::SetupItem &item : page.items) {
            if (const auto *group = std::get_if<SetupGroup>(&item))
                for (const SetupChoice &each : group->choices)
                    std::ranges::for_each(each.values, check);
            else if (const auto &plain = std::get<SetupOptionItem>(item); plain.shown_while)
                check(*plain.shown_while);
        }
    std::printf("[setup] check: %d value(s), %d problem(s)\n", values, problems);
}

verdite::SetupScreen &screen() {
    if (state.drawn)
        return state.overlay_screen;
    return state.undrawn_screen;
}

} // namespace

bool setup_available() {
#ifdef __ANDROID__
    return false;
#else
    return true;
#endif
}

void setup_begin(Overlay overlay) {
    state.drawn = overlay == Overlay::Drawn;
    check_spec(kf1_spec());
    if (state.drawn && setup_available() && verdite::setup_due(kf1_spec(), settings().setup_seen))
        setup_run();
}

void setup_run() {
    if (!setup_available())
        return;
    // Rebuilt each time, for the monitor the window is on now.
    state.setup.reset();
    state.spec = kf1_spec();
    state.setup.emplace(state.spec, options(), platform_features(), mark_seen);
    state.setup->start(screen());
}

bool setup_open() { return state.setup && state.setup->running(); }

void setup_draw() {
    if (!state.drawn || !setup_open())
        return;
    state.overlay_screen.draw(state.controls);
    std::vector<Control> controls = std::exchange(state.controls, {});
    for (const Control &control : controls)
        if (state.setup)
            control(*state.setup);
}

std::string setup_command(std::string_view line) {
    if (line == "start") {
        setup_run();
        return setup_open() ? "" : "no setup on this platform";
    }
    if (line.empty())
        return "";
    if (!state.setup || !state.setup->running())
        return "setup is not open";
    if (!state.setup->command(line))
        return "not a control that fits";
    return "";
}

std::string setup_json() {
    if (!state.setup || !state.setup->running())
        return "null";
    return verdite::setup_json(state.setup->view());
}
} // namespace kf::ext
