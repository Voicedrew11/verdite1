// KF1's first-run setup (verdite/setup.h): its pages and tiers over the option table, and which
// screen shows them: setup_overlay.cpp's window on a desktop, setup_panel.cpp's views on Android.
#include "internal.hpp"
#include "setup_overlay.h"
#include <verdite/setup.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kf::ext {
namespace {
using verdite::SetupChoice;
using verdite::SetupGroup;
using verdite::SetupOptionItem;
using verdite::SetupPage;
using verdite::SetupPageTiers;
using verdite::SetupPreset;
using verdite::SetupSpec;
using verdite::SetupValue;

// Raise to ask every player again.
constexpr int setup_version = 2;

SetupChoice choice(std::string label, std::string help, std::vector<std::string> tiers, std::vector<SetupValue> values) {
    return {std::move(label), std::move(help), std::move(tiers), std::move(values), false};
}

SetupChoice costly(SetupChoice made) {
    made.higher_gpu_cost = true;
    return made;
}

SetupOptionItem option(std::string id) { return {std::move(id), std::nullopt, verdite::SetupOptionLook::Stepper}; }

const SDL_DisplayMode *monitor_mode() {
    const SDL_DisplayID display = window() ? SDL_GetDisplayForWindow(window()) : 0;
    const SDL_DisplayMode *mode = display ? SDL_GetDesktopDisplayMode(display) : nullptr;
    return mode && mode->w > 0 && mode->h > 0 ? mode : nullptr;
}

// The aspect of the monitor the window is on, as the nearest choice. A phone has no window to fit:
// the picture takes the screen's shape.
std::string suggested_aspect() {
    const SDL_DisplayMode *mode = monitor_mode();
    if (std::string_view(default_aspect) == "window")
        return default_aspect;
    if (!mode)
        return "16:9";
    const double ratio = static_cast<double>(mode->w) / mode->h;
    constexpr std::array<std::pair<const char *, double>, 4> aspects = {
        {{"4:3", 4.0 / 3.0}, {"16:10", 1.6}, {"16:9", 16.0 / 9.0}, {"21:9", 64.0 / 27.0}}};
    const auto nearest = std::ranges::min_element(aspects, {}, [ratio](const auto &each) { return std::abs(each.second - ratio); });
    return nearest->first;
}

std::string monitor_line() {
    const SDL_DisplayMode *mode = monitor_mode();
    if (!mode)
        return "";
    std::string line = "Your display: " + std::to_string(mode->w) + " x " + std::to_string(mode->h);
    if (mode->refresh_rate > 0)
        line += " at " + std::to_string(static_cast<int>(std::lround(mode->refresh_rate))) + " Hz";
    return line + ".";
}

std::vector<SetupValue> preset_values(std::string aspect, double resolution, double frame_rate) {
    return {{"gameplay.classic_mode", false},
            {"display.aspect", std::move(aspect)},
            {"display.resolution", resolution},
            {"display.frame_rate", frame_rate}};
}

std::string aspect_name(const std::string &aspect) { return aspect == "window" ? "Full screen" : aspect; }

std::vector<SetupPreset> presets() {
    const std::string aspect = suggested_aspect();
    const std::string wide_summary = aspect_name(aspect) + " / Native resolution / Display refresh";
    return {
        {"original", "Original", "The game as the PlayStation ran it: its picture, and one stick that walks and turns as its D-pad did.", "original",
         preset_values("4:3", 1, 20), "4:3 / 320x240 / One frame per update"},
        {"balanced", "Balanced",
         "The PlayStation's look and feel with its rendering artifacts (texture warping, vertex jitter) corrected, "
         "and analog controls.",
         "balanced", preset_values(aspect, 0, 0), wide_summary},
        {"modern", "Modern",
         "Every enhancement the port adds: smoother motion, sharper textures, ambient occlusion, anti-aliasing and "
         "twin-stick controls.",
         "modern", preset_values(aspect, 0, 0), wide_summary},
        {"custom", "Custom", "Start from your current settings.", "", {{"gameplay.classic_mode", false}}, ""},
    };
}

SetupPage display_page() {
    const SetupGroup window{"window", "Window", "", {
        choice("Windowed", "", {}, {{"display.fullscreen", false}}),
        choice("Fullscreen", "Also Alt+Enter or F11.", {}, {{"display.fullscreen", true}}),
    }};
    std::string text = "How the picture is shown, apart from the styles that follow.";
    if (const std::string line = monitor_line(); !line.empty())
        text += " " + line;
    return {"display", "Display", std::move(text),
            {window, option("display.resolution"), option("display.aspect"),
             SetupOptionItem{"display.frame_rate", std::nullopt, verdite::SetupOptionLook::Slider}},
            SetupPageTiers::Hidden};
}

constexpr const char *tiers_intro =
    "Choose an option for each row, or set them all at once. Original is the game as the PlayStation ran it. Balanced "
    "keeps that look and feel with steady polygons and smooth colour. Modern adds the port's enhancements.";

SetupPage controls_page() {
    const SetupGroup looking{"looking", "Looking up and down", "", {
        choice("17 degrees", "The original's limit on tilting the view.", {"original"}, {{"controls.look_limit", 17.0}}),
        choice("Up to 70 degrees", "Look at the ceiling and at the floor at your feet.", {"balanced", "modern"},
               {{"controls.look_limit", 70.0}}),
    }};
    const SetupGroup vibration{"vibration", "Vibration", "", {
        choice("None", "On-screen buttons give no feedback.", {"original"}, {{"controls.touch_haptics", false}}),
        choice("On button press", "A short click under your finger as an on-screen button goes down.", {"balanced", "modern"},
               {{"controls.touch_haptics", true}}),
    }};
    return {"controls", "Controls", tiers_intro, {looking, vibration}, SetupPageTiers::Offered};
}

SetupPage presentation_page() {
    const SetupGroup image{"image", "Image", "", {
        choice("Wobbling, warping, dithered",
               "As the PlayStation drew it: textures warp, polygons wobble on the 320x240 grid, 15-bit colour with "
               "dither, unfiltered textures, coarse distant walls.",
               {"original"},
               {{"display.wiggle", true}, {"display.true_color", false}, {"display.texture_filter", 0.0},
                {"display.aniso", 0.0}, {"display.detailed_map", false}}),
        choice("Steady, sharp pixels",
               "Perspective-correct textures, polygons placed between pixels and full colour with no dither. The "
               "original texels stay sharp, drawn evenly, with mipmaps and 16x anisotropic filtering.",
               {"balanced", "modern"},
               {{"display.wiggle", false}, {"display.true_color", true}, {"display.texture_filter", 3.0},
                {"display.aniso", 16.0}, {"display.detailed_map", true}}),
        choice("Smooth and filtered",
               "As steady, sharp pixels, with the texels blended into each other (bilinear) instead of kept sharp.", {},
               {{"display.wiggle", false}, {"display.true_color", true}, {"display.texture_filter", 1.0},
                {"display.aniso", 16.0}, {"display.detailed_map", true}}),
    }};
    const SetupGroup lighting{"lighting", "Lighting and fog", "", {
        choice("Corner lighting",
               "Light and fog worked out at each polygon's corners, as the console did. No ambient occlusion.",
               {"original"},
               {{"display.pixel_lighting", false}, {"display.ao", 0.0}, {"display.fog", 0.0}}),
        choice("Smooth lighting and fog",
               "Per-pixel lighting, and fog by distance from the eye. No ambient occlusion.",
               {"balanced"},
               {{"display.pixel_lighting", true}, {"display.ao", 0.0}, {"display.fog", 2.0}}),
        costly(choice("Smooth, with soft shadows",
                      "Per-pixel lighting and fog, plus ambient occlusion: soft shadowing in corners, where walls meet "
                      "floors and under ledges.",
                      {"modern"}, {{"display.pixel_lighting", true}, {"display.ao", 2.0}, {"display.fog", 2.0}})),
    }};
    const SetupGroup edges{"edges", "Edges", "", {
        choice("Jagged", "No anti-aliasing.", {"original", "balanced"}, {{"display.antialiasing", 0.0}}),
        choice("Smoothed (FXAA)", "Edges softened after drawing. Nearly free, and the HUD stays sharp.", {"modern"},
               {{"display.antialiasing", 1.0}}),
        costly(choice("Supersampled (4x)", "Four times the pixels, averaged: the cleanest edges.", {},
                      {{"display.antialiasing", 2.0}})),
    }};
    const SetupGroup view{"view", "View", "", {
        choice("HUD in the middle", "The gauges and the compass stay in the 4:3 box in the middle of a wide picture.",
               {"original", "balanced"}, {{"display.hud_at_edges", false}}),
        choice("HUD at the edges", "In a wide picture, the gauges move out to the left edge and the compass to the right.",
               {"modern"}, {{"display.hud_at_edges", true}}),
    }};
    return {"presentation", "Presentation", tiers_intro, {image, lighting, edges, view}, SetupPageTiers::Offered};
}

SetupPage turning_page() {
    const SetupGroup sticks{"sticks", "Controls", "", {
        choice("Tank controls (3D)", "One stick walks and turns, as the D-pad does; L1 and R1 strafe. You turn at the original rate.",
               {"original"}, {{"analog.style", 1.0}}),
        choice("Twin stick (2D)",
               "The left stick walks and strafes, the right stick turns and looks, at the D-pad's rate. You turn at the "
               "original rate.",
               {"balanced", "modern"}, {{"analog.style", 2.0}}),
        choice("Twin stick (2D) with acceleration",
               "As twin stick, but holding the right stick out speeds the turn up to 2.2 times.", {},
               {{"analog.style", 3.0}}),
    }};
    const SetupGroup enemies{"enemy_turn", "Enemy turn speed", "", {
        choice("Automatic",
               "Enemies keep up with how fast you can turn: as the original with the D-pad, keys or a stick at its "
               "speed, faster with acceleration, three times as fast with the mouse or a touch drag.",
               {"balanced", "modern"}, {{"gameplay.enemy_turn_speed", 0.0}}),
        choice("x1 (original)", "Enemies turn as on the PlayStation, whatever you play with.", {"original"},
               {{"gameplay.enemy_turn_speed", 1.0}}),
        choice("x1.5", "Half as fast again, whatever you play with.", {}, {{"gameplay.enemy_turn_speed", 1.5}}),
        choice("x2", "Twice as fast, whatever you play with.", {}, {{"gameplay.enemy_turn_speed", 2.0}}),
        choice("x3", "Three times as fast, whatever you play with.", {}, {{"gameplay.enemy_turn_speed", 3.0}}),
    }};
    return {"turning", "Turning and difficulty",
            "King's Field's fights are balanced around how fast you turn. On the PlayStation you turned at one fixed "
            "rate, and every creature was tuned to keep up with it.\n\n"
            "The port can turn you faster: look acceleration speeds up the right stick, and the mouse or a touch drag "
            "turns as fast as you move it. A creature at its original pace can then be circled forever without landing "
            "a hit.\n\n"
            "The answer is to let creatures turn faster when they turn toward you; their wandering keeps the game's pace.",
            {sticks, enemies},
            SetupPageTiers::Hidden};
}

verdite::SetupWelcome welcome_words() {
    constexpr const char *prompt =
        "Choose a starting point. Every choice it makes is shown on the pages that follow, where you can change any of them.";
    if (settings().setup_seen >= setup_version)
        return {"Run Setup Again",
                "Choose again how the game should look and control. Your current settings are where Custom starts.", prompt,
                "Cancel"};
    return {"Welcome",
            "Before you play, choose how the game should look and control. Everything here can be changed later in the settings.",
            prompt, "Skip setup"};
}

SetupSpec kf1_spec() {
    SetupSpec spec;
    spec.version = setup_version;
    spec.welcome = welcome_words();
    spec.tiers = {{"original", "Original", "As the PlayStation ran it."},
                  {"balanced", "Balanced", "That look and feel, with steady polygons and smooth colour."},
                  {"modern", "Modern", "Every enhancement the port adds."}};
    spec.presets = presets();
    spec.pages = {display_page(), controls_page(), presentation_page(), turning_page()};
    return spec;
}

// ---- The screens ----

// Headless: nothing is drawn; the agent channel drives it.
class UndrawnScreen final : public verdite::SetupScreen {
public:
    void show(const verdite::SetupPageView &) override {}
    void close() override {}
};

struct SetupState {
    SetupSpec spec;
    std::optional<verdite::Setup> setup;
    SetupOverlayScreen overlay_screen;
    UndrawnScreen undrawn_screen;
    bool drawn = false;   // this run has a screen
    std::vector<SetupControl> controls;
};
SetupState state;

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
    if (!state.drawn)
        return state.undrawn_screen;
    if (verdite::SetupScreen *panel = setup_panel_screen())
        return *panel;
    return state.overlay_screen;
}

} // namespace

void setup_begin(Overlay overlay) {
    state.drawn = overlay == Overlay::Drawn;
    check_spec(kf1_spec());
    settings_page_check();
    if (state.drawn && verdite::setup_due(kf1_spec(), settings().setup_seen))
        setup_run();
}

void setup_run() {
    // Rebuilt each time, for the monitor the window is on now.
    state.setup.reset();
    state.spec = kf1_spec();
    state.setup.emplace(state.spec, options(), platform_features(), mark_seen);
    state.setup->start(screen());
}

bool setup_open() { return state.setup && state.setup->running(); }

bool setup_shows_game() { return setup_open() && !setup_panel_screen(); }

void setup_poll() {
    for (const std::string &line : setup_panel_take_controls())
        if (state.setup && state.setup->running() && !state.setup->command(line))
            std::printf("[setup] not a control that fits: %s\n", line.c_str());
}

void setup_draw() {
    if (!state.drawn || setup_panel_screen() || !setup_open())
        return;
    state.overlay_screen.draw(state.controls);
    std::vector<SetupControl> controls = std::exchange(state.controls, {});
    for (const SetupControl &control : controls)
        if (state.setup && state.setup->running())
            control(*state.setup);
}

std::string setup_command(std::string_view line) {
    if (line == "start") {
        setup_run();
        return "";
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
