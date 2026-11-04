// KF1's settings as libverdite option rows: what the overlay's Settings window
// and Android's settings panel draw. A row's id is its settings.json path where
// it has one. Each set stores, applies and marks the settings for saving.
#include "internal.hpp"
#include "mod_menu.h"
#include <kf/platform/coop.h>
#include <kf/platform/language.h>
#include <kf/platform/language_runtime.h>
#include <kf/renderer/frontend.h>
#include <kf/renderer/renderer.h>
#include <type_traits>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <utility>

namespace kf::ext {
namespace {
using verdite::OptionChoice;
using verdite::OptionKind;
using verdite::OptionPage;
using verdite::OptionRow;
using verdite::OptionSection;
using verdite::OptionState;
using verdite::OptionValue;
using verdite::PlatformFeature;

using Apply = void (*)();
void store_only() {}

constexpr auto feature(PlatformFeature f) { return static_cast<verdite::PlatformFeatures>(f); }

OptionRow row(std::string id, OptionKind kind, std::string label, std::string help) {
    OptionRow made;
    made.id = std::move(id);
    made.kind = kind;
    made.label = std::move(label);
    made.help = std::move(help);
    return made;
}

void settings_changed(Apply apply) {
    apply();
    settings_mark_dirty();
}

OptionRow toggle(std::string id, std::string label, std::string help, bool &field, Apply apply) {
    OptionRow made = row(std::move(id), OptionKind::Toggle, std::move(label), std::move(help));
    made.get = [&field] { return OptionValue(field); };
    made.set = [&field, apply](const OptionValue &v) {
        field = std::get<bool>(v);
        settings_changed(apply);
    };
    return made;
}

struct Range {
    double min, max, step;
    const char *format;
};

// A float read back as the decimal it was set to (0.1, not 0.1000000015).
double as_double(float field) { return std::round(static_cast<double>(field) * 1e6) / 1e6; }
double as_double(int field) { return field; }

template <typename Number>
OptionRow number(std::string id, std::string label, std::string help, Number &field, Range range, Apply apply) {
    OptionRow made = row(std::move(id), OptionKind::Number, std::move(label), std::move(help));
    made.min = range.min;
    made.max = range.max;
    made.step = range.step;
    made.format = range.format;
    made.get = [&field] { return OptionValue(as_double(field)); };
    made.set = [&field, apply](const OptionValue &v) {
        const double d = std::get<double>(v);
        field = std::is_integral_v<Number> ? static_cast<Number>(std::lround(d)) : static_cast<Number>(d);
        settings_changed(apply);
    };
    return made;
}

// Ints and enums are held as numbers; bools and strings as themselves.
template <typename Field> constexpr bool held_as_number = std::is_enum_v<Field> || std::is_same_v<Field, int>;

template <typename Field> OptionValue as_value(const Field &field) {
    if constexpr (held_as_number<Field>)
        return static_cast<double>(field);
    else
        return field;
}

template <typename Field>
OptionRow choice(std::string id, std::string label, std::string help, Field &field,
                 std::vector<std::pair<Field, std::string>> choices, Apply apply) {
    OptionRow made = row(std::move(id), OptionKind::Choice, std::move(label), std::move(help));
    for (auto &[value, name] : choices)
        made.choices.push_back(OptionChoice{as_value(value), std::move(name)});
    made.get = [&field] { return as_value(field); };
    made.set = [&field, apply](const OptionValue &v) {
        if constexpr (held_as_number<Field>)
            field = static_cast<Field>(std::get<double>(v));
        else
            field = std::get<Field>(v);
        settings_changed(apply);
    };
    return made;
}

// Several saved settings as one choice (setting_rows.h). Choices are values a choice row holds as
// itself or as a number, as choice() above.
template <typename Choice>
OptionRow combined(std::string id, std::string label, std::string help, std::vector<std::pair<Choice, std::string>> choices,
                   Choice (*read)(const Settings &), void (*write)(Settings &, Choice), Apply apply) {
    OptionRow made = row(std::move(id), OptionKind::Choice, std::move(label), std::move(help));
    for (auto &[value, name] : choices)
        made.choices.push_back(OptionChoice{as_value(value), std::move(name)});
    made.get = [read] { return as_value(read(settings())); };
    made.set = [write, apply](const OptionValue &v) {
        if constexpr (std::is_enum_v<Choice> || std::is_same_v<Choice, int>)
            write(settings(), static_cast<Choice>(std::get<double>(v)));
        else
            write(settings(), std::get<Choice>(v));
        settings_changed(apply);
    };
    return made;
}

OptionRow combined_toggle(std::string id, std::string label, std::string help, bool (*read)(const Settings &),
                          void (*write)(Settings &, bool), Apply apply) {
    OptionRow made = row(std::move(id), OptionKind::Toggle, std::move(label), std::move(help));
    made.get = [read] { return OptionValue(read(settings())); };
    made.set = [write, apply](const OptionValue &v) {
        write(settings(), std::get<bool>(v));
        settings_changed(apply);
    };
    return made;
}

OptionRow status(std::string id, std::string label, std::function<std::string()> text) {
    OptionRow made = row(std::move(id), OptionKind::Status, std::move(label), {});
    made.get = [text = std::move(text)] { return OptionValue(text()); };
    return made;
}

OptionRow note(std::string id, std::string text) {
    return status(std::move(id), {}, [text = std::move(text)] { return text; });
}

OptionRow action(std::string id, std::string label, std::string help, std::function<void()> run) {
    OptionRow made = row(std::move(id), OptionKind::Action, std::move(label), std::move(help));
    made.run = std::move(run);
    return made;
}

OptionRow needs(OptionRow made, PlatformFeature f) {
    made.needs |= feature(f);
    return made;
}

// Adds a condition to a row's state, keeping what it had.
OptionRow shown_if(OptionRow made, std::function<bool()> shown) {
    made.state = [before = std::move(made.state), shown = std::move(shown)] {
        OptionState state = before ? before() : OptionState{};
        state.shown = state.shown && shown();
        return state;
    };
    return made;
}

OptionRow enabled_if(OptionRow made, std::function<bool()> enabled) {
    made.state = [before = std::move(made.state), enabled = std::move(enabled)] {
        OptionState state = before ? before() : OptionState{};
        state.enabled = state.enabled && enabled();
        return state;
    };
    return made;
}

bool not_classic() { return !settings().classic; }

// VSync follows the frame rate: Unlimited turns it off.
void apply_frame_rate() {
    apply_render_settings();
    apply_display_settings();
}

// Classic mode overrides the section's rows; they keep the player's values for later.
OptionSection classic_locked(OptionSection section) {
    for (OptionRow &each : section.rows)
        each = enabled_if(std::move(each), not_classic);
    return section;
}

OptionRow classic_note(const std::string &page) {
    return shown_if(note(page + ".classic_note", "Classic mode is on: these follow the original game."),
                    [] { return settings().classic; });
}

void apply_classic() {
    apply_render_settings();
    apply_control_settings();
    coop_room_settings_changed();
}

// A tick rate set by a host goes to the room.
void apply_tick_rate() {
    apply_control_settings();
    coop_room_settings_changed();
}

// ---- Display ----

OptionSection display_window() {
    Settings &s = settings();
    return {"", {
        needs(toggle("display.fullscreen", "Fullscreen", "Also Alt+Enter or F11.", s.fullscreen, apply_display_settings),
              PlatformFeature::Window),
        needs(enabled_if(toggle("display.vsync", "VSync", "Unlimited frame rate turns it off.", s.vsync,
                                apply_display_settings),
                         [] { return settings().frame_rate != frame_rate_unlimited; }),
              PlatformFeature::Window),
        toggle("display.show_fps", "Show FPS", "", s.show_fps, store_only),
    }};
}

OptionSection display_picture() {
    Settings &s = settings();
    OptionRow rendering = status("display.rendering", "Rendering", [] {
        const RenderTarget target = applied_render_target();
        return std::to_string(target.columns * target.scale) + "x" + std::to_string(render_height * target.scale);
    });
    return classic_locked({"", {
        choice("display.frame_rate", "Frame rate",
               "The game still updates at its tick rate (Gameplay, 15 a second by default); extra frames are drawn "
               "between updates, so the view trails by one update (67 ms at 15).",
               s.frame_rate, {{20, "One per update (original)"}, {0, "Match display refresh"}, {60, "60 fps"},
                              {120, "120 fps"}, {144, "144 fps"}, {165, "165 fps"}, {240, "240 fps"}, {frame_rate_unlimited, "Unlimited"}},
               apply_frame_rate),
        choice<std::string>("display.aspect", "Aspect ratio",
                            "Wider aspects show more of the world to each side. Menus stay in the centre.", s.aspect,
                            {{"4:3", "4:3 (original)"}, {"16:9", "16:9"}, {"16:10", "16:10"}, {"21:9", "21:9"},
                             {"window", "Match window"}},
                            apply_render_settings),
        enabled_if(toggle("display.hud_at_edges", "HUD at the screen edges",
                          "In a wider aspect, the gauges move out to the left edge and the compass to the right, as far "
                          "from the edges as in 4:3. Off: the HUD stays in its 4:3 box.",
                          s.hud_at_edges, apply_render_settings),
                   [] { return settings().aspect != "4:3"; }),
        choice("display.resolution", "Resolution",
               "How sharply the 3D picture is drawn, before it is fitted to the window. 1x is the PlayStation's.",
               s.resolution,
               {{0, "Native (match window)"}, {1, "1x (320x240)"}, {2, "2x"}, {3, "3x"}, {4, "4x"}, {5, "5x"},
                {6, "6x"}, {7, "7x"}, {8, "8x"}},
               apply_render_settings),
        rendering,
    }});
}

OptionSection display_look() {
    Settings &s = settings();
    return classic_locked({"Picture", {
        combined_toggle("display.wiggle", "Polygon wiggle",
                        "On: corners snap to the 320x240 grid and textures warp (affine mapping), as on the PlayStation. "
                        "Off: sub-pixel vertices and perspective-correct textures.",
                        wiggle, set_wiggle, apply_render_settings),
        toggle("display.true_color", "True colour",
               "Full colour precision: smooth gradients in shadows and fog. Off: the PlayStation's 15-bit colour "
               "and its dither pattern.",
               s.true_color, apply_render_settings),
        choice("display.texture_filter", "Texture filter",
               "Textures on walls, floors and models. Sharp keeps the original blocky texels, all drawn the same "
               "size. The HUD and menus stay pixel-sharp.",
               s.texture_filter,
               {{0, "Nearest (original)"}, {1, "Bilinear"}, {2, "3-point (N64 style)"}, {3, "Sharp (even texels)"}},
               apply_render_settings),
        combined<int>("display.aniso", "Anisotropic filtering",
                      "Off: distant textures shimmer, as on the PlayStation. Trilinear: mipmaps. 2x to 16x: anisotropic "
                      "samples over the mipmaps, sharp on floors and walls at a slant. Not with 3-point.",
                      {{0, "Off"}, {1, "Trilinear"}, {2, "2x"}, {4, "4x"}, {8, "8x"}, {16, "16x"}}, aniso_choice,
                      set_aniso_choice, apply_render_settings),
        toggle("display.detailed_map", "Detailed map",
               "Draws every map cell with its close-up model. The PlayStation used coarse models past the nearest "
               "cells, so wall textures changed as you walked up.",
               s.detailed_map, apply_render_settings),
        choice("display.fog", "Fog",
               "Where the darkness that swallows distant walls is measured. Corners: blended across each polygon, as "
               "on the PlayStation. Per pixel: smooth on large floors and walls; by distance it also stays put as "
               "the view turns.",
               s.fog, {{0, "Corners (original)"}, {1, "Per pixel, by depth"}, {2, "Per pixel, by distance"}},
               apply_render_settings),
        number("display.enhanced_distance", "Enhanced distance",
               "Past this many map cells, walls and floors are drawn the PlayStation's way: sharp texels, no ambient "
               "occlusion, fog at the corners. 0 enhances everything.",
               s.enhanced_distance, {0, 16, 1, "%.0f cells"}, apply_render_settings),
        choice("display.antialiasing", "Anti-aliasing",
               "Smooths jagged polygon edges. FXAA is nearly free and leaves the HUD sharp. SSAA renders 4 or 9 "
               "times the pixels and averages them: cleanest, but costly.",
               s.antialiasing, {{0, "Off"}, {1, "FXAA"}, {2, "4x SSAA"}, {3, "9x SSAA"}}, apply_render_settings),
    }});
}

OptionSection display_lighting() {
    Settings &s = settings();
    return classic_locked({"Lighting", {
        toggle("display.pixel_lighting", "Per-pixel lighting",
               "The game's own lights worked out at every pixel of a model, not blended from its corners: smooth "
               "shading over curved creatures, and light that turns with them between game updates.",
               s.pixel_lighting, apply_render_settings),
        combined<DynamicLight>("display.dynamic_light", "Dynamic lights",
                               "Torches cast flickering light on nearby walls and floors, stopped by solid walls, and "
                               "fire and lightning spells light up what they pass. Shadows: the four nearest lights cast "
                               "them; they only remove the added light, so nothing gets darker than in the original.",
                               {{DynamicLight::Off, "Off"}, {DynamicLight::On, "On"}, {DynamicLight::Shadows, "On, with shadows"}},
                               dynamic_light, set_dynamic_light, apply_render_settings),
        enabled_if(toggle("display.flame_glow", "Flame glow",
                          "A soft halo around torch flames and fire or lightning spells. Hidden when something is in "
                          "front of the flame.",
                          s.flame_glow, apply_render_settings),
                   [] { return settings().dynamic_lights; }),
        combined<AoChoice>("display.ao", "Ambient occlusion",
                           "Soft shadowing in corners, where walls meet floors and under ledges. Low measures at the "
                           "game's own pixels, Medium at twice that, High at the full resolution.",
                           {{AoChoice::Off, "Off"}, {AoChoice::Low, "Low"}, {AoChoice::Medium, "Medium"}, {AoChoice::High, "High"}},
                           ao_choice, set_ao_choice, apply_render_settings),
    }});
}

OptionSection display_accuracy() {
    return classic_locked({"Accuracy", {
        toggle("display.depth_buffer", "Depth buffer (experimental)",
               "Stops parts of objects showing through walls at some angles. The PlayStation sorted whole polygons "
               "instead; some effects may look different with this on.",
               settings().depth_buffer, apply_render_settings),
    }});
}

OptionPage display_page() {
    return {"Display", {OptionSection{"", {classic_note("display")}}, display_window(), display_picture(),
                        display_look(), display_lighting(), display_accuracy()}};
}

// ---- Audio ----

OptionPage audio_page() {
    return {"Audio", {OptionSection{"", {number("audio.volume", "Volume", "", settings().volume, {0, 100, 1, "%.0f%%"},
                                                apply_audio_settings)}}}};
}

// ---- Controls ----

OptionSection controls_movement() {
    Settings &s = settings();
    OptionSection movement = classic_locked({"", {
        number("controls.look_limit", "Look up/down limit",
               "How far the camera can tilt up or down. The original game allows about 17 degrees.", s.look_limit,
               {17, 85, 1, "%.0f deg"}, apply_control_settings),
    }});
    movement.rows.insert(movement.rows.begin(), classic_note("controls"));
    movement.rows.push_back(needs(
        choice<std::string>("controls.pad_layout", "Gamepad buttons",
                            "West: Square attacks, Cross examines and confirms, Circle opens the menu and backs out, "
                            "Triangle uses the item or spell. Japanese: Triangle attacks, Circle examines and confirms, "
                            "Cross opens the menu and backs out, Square uses the item or spell.",
                            s.pad_layout, {{"west", "West (US King's Field)"}, {"jp", "Japanese (this disc's)"}},
                            apply_control_settings),
        PlatformFeature::Gamepad));
    return movement;
}

bool analog_on() { return settings().analog.enabled; }
bool twin_stick() { return analog_on() && settings().analog.mode == StickMode::Twin; }

OptionSection gamepad_section(OptionSection section) {
    section = classic_locked(std::move(section));
    for (OptionRow &each : section.rows)
        each = needs(std::move(each), PlatformFeature::Gamepad);
    return section;
}

OptionSection controls_analog() {
    AnalogOptions &a = settings().analog;
    std::vector<std::pair<double, std::string>> sens;
    for (const double stop : stick_sens_stops) {
        char text[16];
        std::snprintf(text, sizeof text, "%.2fx", stop);
        sens.emplace_back(stop, text);
    }
    return gamepad_section({"Sticks", {
        combined<ControlStyle>("analog.style", "Controls",
                               "D-pad: as the PlayStation had; the left stick works as the D-pad. 3D: tank controls, the "
                               "left stick walks and turns; L1 and R1 strafe. 2D: twin stick, the left stick walks and "
                               "strafes, the right turns and looks. 2D accel: holding the right stick near the edge turns "
                               "faster over time, and enemies turn to match.",
                               {{ControlStyle::DPad, "D-pad"}, {ControlStyle::Tank, "3D (tank)"}, {ControlStyle::Twin, "2D (twin stick)"},
                                {ControlStyle::TwinAccelerated, "2D with acceleration"}},
                               control_style, set_control_style, apply_control_settings),
        enabled_if(combined<double>("analog.stick_sens", "Stick sensitivity", "Turn and look speed together; Fine tuning sets them apart. 1x at full deflection is the D-pad's rate.",
                                    std::move(sens), stick_sens, set_stick_sens, apply_control_settings),
                   analog_on),
        enabled_if(toggle("analog.invert_pitch", "Invert camera", "Push up to look down, as a flight stick.", a.invert_pitch,
                          apply_control_settings),
                   twin_stick),
    }});
}

OptionSection controls_fine_tuning() {
    AnalogOptions &a = settings().analog;
    auto in_analog = [](OptionRow made) { return enabled_if(std::move(made), analog_on); };
    auto acceleration_on = [] { return twin_stick() && settings().analog.look_acceleration; };
    return gamepad_section({"Fine tuning", {
        in_analog(number("analog.turn_sensitivity", "Turn speed", "", a.turn_sensitivity, {0.1, 2.0, 0.05, "%.2fx"},
                         apply_control_settings)),
        in_analog(number("analog.pitch_sensitivity", "Look up/down speed", "", a.pitch_sensitivity,
                         {0.1, 2.0, 0.05, "%.2fx"}, apply_control_settings)),
        in_analog(number("analog.move_sensitivity", "Walk speed", "1x at full deflection is the D-pad's speed.", a.move_sensitivity,
                         {0.1, 1.0, 0.05, "%.2fx"}, apply_control_settings)),
        enabled_if(combined<LookResponse>("analog.look_response", "Look response",
                                          "Eased: the view speeds up and slows down by the game's own step, as with the D-pad. "
                                          "Stops dead: eases in, stops on release. Direct: follows the stick.",
                                          {{LookResponse::Eased, "Eased (original)"}, {LookResponse::StopsDead, "Stops dead"},
                                           {LookResponse::Direct, "Direct"}},
                                          look_response, set_look_response, apply_control_settings),
                   twin_stick),
        in_analog(number("analog.look_deadzone", "Look deadzone", "", a.look_deadzone, {0.0, 0.5, 0.01, "%.2f"},
                         apply_control_settings)),
        in_analog(number("analog.move_deadzone", "Move deadzone", "", a.move_deadzone, {0.0, 0.5, 0.01, "%.2f"},
                         apply_control_settings)),
        in_analog(number("analog.look_curve", "Look curve",
                         "Above 1: finer control near the centre, the full speed still at the edge.", a.look_curve,
                         {0.5, 3.0, 0.05, "%.2f"}, apply_control_settings)),
        in_analog(number("analog.move_curve", "Move curve", "", a.move_curve, {0.5, 3.0, 0.05, "%.2f"},
                         apply_control_settings)),
        enabled_if(number("analog.acceleration_max", "Acceleration up to", "", a.acceleration_max,
                          {1.0, 4.0, 0.1, "%.1fx"}, apply_control_settings),
                   acceleration_on),
        enabled_if(number("analog.acceleration_time_s", "Acceleration time", "", a.acceleration_time_s,
                          {0.1, 2.0, 0.05, "%.2f s"}, apply_control_settings),
                   acceleration_on),
        in_analog(toggle("analog.invert_turn", "Invert turn", "", a.invert_turn, apply_control_settings)),
        in_analog(toggle("analog.invert_strafe", "Invert strafe", "", a.invert_strafe, apply_control_settings)),
        in_analog(toggle("analog.invert_forward", "Invert walk", "", a.invert_forward, apply_control_settings)),
    }});
}

OptionSection controls_bindings() {
    static constexpr std::pair<const char *, const char *> bindings[] = {
        {"Move / strafe", "W A S D"}, {"Turn", "Left / Right arrows"},
        {"Look", "Mouse, Page Up / Page Down"}, {"Attack", "Space, left mouse"}, {"Magic", "Q, right mouse"},
        {"Interact", "F, Enter"}, {"Menu", "Tab (opens and backs out)"}, {"Back", "Backspace, Tab, right mouse in menus"},
        {"Quick save", "F2"}, {"Quick load", "F4 (also on the title screen)"},
        {"Pause", "P, Escape"}, {"Overlay", "F1"}, {"Fullscreen", "F11, Alt+Enter"}};
    OptionSection section{"Keyboard (rebinding is planned)", {}};
    for (const auto &[what, keys] : bindings)
        section.rows.push_back(needs(status(std::string("controls.keys.") + what, what, [keys] { return std::string(keys); }),
                                     PlatformFeature::Keyboard));
    return section;
}

OptionSection controls_touch() {
    return {"Touch screen", {
        needs(toggle("controls.touch_haptics", "Vibrate on button press",
                     "A short click under your finger as an on-screen button goes down. Follows the phone's own "
                     "touch feedback setting.",
                     settings().touch_haptics, apply_control_settings),
              PlatformFeature::Touch),
    }};
}

OptionPage controls_page() {
    return {"Controls", {controls_movement(), controls_touch(), controls_analog(), controls_fine_tuning(), controls_bindings()}};
}

// ---- Gameplay ----

OptionSection gameplay_classic() {
    OptionRow classic = toggle("gameplay.classic_mode", "Classic mode",
                               "Play King's Field as it originally shipped: 4:3 at 320x240, 20 fps, polygon wiggle, "
                               "texture warping, 15-bit colour with dithering, no added lighting or anti-aliasing, the "
                               "original look range, no equipment stats, quick save or bug fixes. "
                               "Fullscreen, volume, controls, language and mods are unaffected.",
                               settings().classic, apply_classic);
    classic.state = [] {
        OptionState state;
        if (settings().classic)
            state.note = "On: as originally shipped. Your own settings are kept for later.";
        return state;
    };
    return {"", {classic}};
}

OptionSection gameplay_world() {
    Settings &s = settings();
    OptionSection section = classic_locked({"", {
        choice("gameplay.tick_rate", "Tick rate",
               "How often the world updates. The PlayStation's frame gate allows at most 20 a second, and the console "
               "fell below that wherever a scene took longer to draw; 15 stands for a typical rate on the console. "
               "The game counts updates, not time, so walking, turning, enemies and timers all slow down or speed up "
               "with it; the music does not.",
               s.tick_rate, {{30, "30 a second (fast)"}, {20, "20 a second (the game's limit)"},
                             {15, "15 a second (default)"}, {12, "12 a second (slower)"}},
               apply_tick_rate),
        toggle("gameplay.auto_use_keys", "Use carried keys on locked doors and chests",
               "Interacting with a lock unlocks it (with the unlock sound) when you carry its key, instead of having "
               "to choose the key from the item menu.",
               s.auto_keys, apply_control_settings),
    }});
    // In a room everyone runs at the host's speed.
    section.rows.front() = enabled_if(std::move(section.rows.front()), [] { return !coop_follows_host(); });
    return section;
}

OptionSection gameplay_enemies() {
    std::vector<std::pair<double, std::string>> turns;
    turns.emplace_back(enemy_turn_automatic, "Automatic");
    turns.emplace_back(1.0, "x1 (original)");
    for (int quarters = 5; quarters <= 16; ++quarters) {
        const double scale = quarters / 4.0;
        char text[16];
        std::snprintf(text, sizeof text, "x%.2f", scale);
        turns.emplace_back(scale, text);
    }
    return classic_locked({"Enemies", {
        combined<double>("gameplay.enemy_turn_speed", "Enemy turn speed",
                         "How fast an enemy turns while it chases you or backs away from you; its wandering keeps the "
                         "game's pace. The fights were built on the D-pad's turn. Automatic: enemies keep up with how "
                         "fast you can turn with what you last turned with: the D-pad and keys as the original, a stick "
                         "by its turn speed and acceleration, the mouse or a touch drag at x3. Above x1 a chasing enemy "
                         "also aims at where you are every update; the original re-aimed on about one update in eight.",
                         std::move(turns), enemy_turn_choice, set_enemy_turn_choice, apply_control_settings),
    }});
}

OptionPage gameplay_page() {
    return {"Gameplay", {gameplay_classic(), gameplay_world(), gameplay_enemies()}};
}

// ---- Language ----

// A file chosen in a dialog. The dialog answers later, possibly on another
// thread, so options_poll picks the result up on the game thread.
enum class Pick : std::uint8_t { None, Patch, Disc };
std::mutex pick_lock;
Pick pick_pending = Pick::None, pick_done = Pick::None;
std::string pick_path;

void pick_file(Pick purpose) {
    static const SDL_DialogFileFilter patch_filter[] = {{"English patch (.rar or .ppf)", "rar;ppf;zip;7z"}};
    static const SDL_DialogFileFilter disc_filter[] = {{"Disc images", "cue;bin;iso;chd"}};
    {
        std::lock_guard guard(pick_lock);
        pick_pending = purpose;
    }
    SDL_ShowOpenFileDialog([](void *, const char *const *files, int) {
        std::lock_guard guard(pick_lock);
        pick_done = files && files[0] ? pick_pending : Pick::None;
        pick_path = files && files[0] ? files[0] : "";
        pick_pending = Pick::None;
    }, nullptr, window(), purpose == Pick::Patch ? patch_filter : disc_filter, 1, nullptr, false);
}

void choose_language(Language language) {
    settings().language = language_code(language);
    language_preference = language;
    language_request(language);
    settings_mark_dirty();
}

LanguageTask task_now() { return language_task_state(nullptr); }
bool both_languages() { return language_available(Language::English) && language_available(Language::Japanese); }
Language other_language() { return game_language() == Language::English ? Language::Japanese : Language::English; }

OptionRow language_choice() {
    OptionRow made = row("gameplay.language", OptionKind::Choice, "Language",
                         "King's Field was released only in Japanese. The English version is John Osborne's fan "
                         "translation (v1.0). Saves work in both languages.");
    made.choices = {{std::string("en"), "English"}, {std::string("ja"), "Japanese"}};
    made.get = [] { return OptionValue(std::string(language_code(language_requested()))); };
    made.set = [](const OptionValue &v) {
        Language language;
        if (language_parse(std::get<std::string>(v).c_str(), &language))
            choose_language(language);
    };
    made.state = [] {
        OptionState state;
        state.enabled = both_languages();
        if (language_requested() != game_language())
            state.note = std::string("Switches to ") + language_name(language_requested()) + " when you return to the game.";
        else
            state.note = std::string("In use: ") + language_name(game_language()) + ". The game's Configuration menu can switch too.";
        return state;
    };
    return made;
}

OptionRow language_files(Language language) {
    return status(std::string("language.files.") + language_code(language), language_name(language),
                  [language] { return std::string(language_available(language) ? "ready" : "not set up"); });
}

OptionSection language_setup() {
    auto idle = [] { return task_now() != LanguageTask::Working; };
    auto needs_english = [] { return !language_available(Language::English) && language_available(Language::Japanese); };
    auto needs_japanese = [] { return !language_available(Language::Japanese); };
    OptionRow message = status("language.task", "", [] {
        std::string text;
        language_task_state(&text);
        return text;
    });
    return {"Game files", {
        language_files(Language::English),
        language_files(Language::Japanese),
        shown_if(note("language.english_note", "Add English with the translator's patch, applied to your Japanese game files."),
                 needs_english),
        enabled_if(shown_if(action("language.download_english", "Download English patch",
                                   "Downloads John Osborne's release (about 6 MB) from the Internet Archive copy of the "
                                   "translator's site, checks it, and builds English next to Japanese.",
                                   language_task_download_english),
                            needs_english),
                   idle),
        needs(enabled_if(shown_if(action("language.patch_file", "Choose patch file...",
                                         "Kings_Field_Jap_to_Eng_v1.0.rar, or the .ppf inside it.",
                                         [] { pick_file(Pick::Patch); }),
                                  needs_english),
                         idle),
              PlatformFeature::FileDialog),
        shown_if(note("language.japanese_note",
                      "Japanese needs your original Japanese disc image (.cue/.bin, .iso or .chd); a disc with the "
                      "English patch applied can't be turned back."),
                 needs_japanese),
        needs(enabled_if(shown_if(action("language.add_disc", "Add Japanese disc...", "", [] { pick_file(Pick::Disc); }),
                                  needs_japanese),
                         idle),
              PlatformFeature::FileDialog),
        shown_if(message, [] { return task_now() != LanguageTask::Idle; }),
        shown_if(action("language.switch", "Switch language", "", [] {
                     choose_language(other_language());
                     language_task_clear();
                 }),
                 [] { return task_now() == LanguageTask::Done && language_available(other_language()); }),
        shown_if(action("language.task_ok", "OK", "", language_task_clear),
                 [] { return task_now() == LanguageTask::Failed; }),
    }};
}

OptionPage language_page() { return {"Language", {OptionSection{"", {language_choice()}}, language_setup()}}; }

// ---- Interface ----

OptionSection interface_overlay() {
    Settings &s = settings();
    OptionRow scale = number("overlay.ui_scale", "Interface scale",
                             "The overlay's size. Applies when you release the slider. Mods are not scaled.", s.ui_scale,
                             {0.75, 2.5, 0.05, "%.2f"}, overlay_restyle);
    scale.settle = true;
    return {"", {
        action("overlay.run_setup", "Run setup again",
               "The first-run setup: presets, then a page per part of the game, then a summary before anything "
               "changes.",
               setup_run),
        toggle("overlay.pause_when_open", "Pause the game while the overlay is open", "", s.pause_when_open, store_only),
        toggle("overlay.item_descriptions", "Item descriptions in the game's menus",
               "Under the item preview: what the item does, such as healing or an equipment effect.",
               s.item_descriptions, store_only),
        scale,
    }};
}

OptionSection interface_updates() {
    OptionRow check = toggle("overlay.update_check", "Check for updates at launch",
                             "Asks GitHub for the latest release at most once a day. Nothing is downloaded.",
                             settings().update_check, store_only);
    check.state = [] {
        OptionState state;
        state.note = update_notice_setting_note();
        return state;
    };
    return {"Updates", {check}};
}

OptionSection interface_discord() {
    Settings &s = settings();
    OptionRow presence = toggle("discord.enabled", "Discord Rich Presence",
                                "Shows what you are doing in the game on your Discord profile, while Discord is running.",
                                s.discord, store_only);
    presence.state = [] {
        OptionState state;
        state.enabled = !discord_forced();
        if (discord_forced())
            state.note = "Set by KF_DISCORD for this run.";
        return state;
    };
    OptionSection section{"Discord", {
        presence,
        enabled_if(toggle("discord.level", "Level", "", s.discord_level, store_only), discord_enabled),
        enabled_if(toggle("discord.location", "Location", "Floor and place.", s.discord_location, store_only),
                   discord_enabled),
        enabled_if(toggle("discord.vitals", "HP and MP", "", s.discord_vitals, store_only), discord_enabled),
        enabled_if(toggle("discord.exp", "EXP", "EXP to the next level.", s.discord_exp, store_only), discord_enabled),
    }};
    for (OptionRow &each : section.rows)
        each = needs(std::move(each), PlatformFeature::Discord);
    return section;
}

OptionPage interface_page() { return {"Interface", {interface_overlay(), interface_updates(), interface_discord()}}; }

// ---- Multiplayer ----

#if KF_ONLINE
OptionSection multiplayer_own() {
    Settings &s = settings();
    return {"", {
        choice("online.name_bars", "Name bars",
               "The other players' names above their knights. Through walls is how Minecraft shows them; In sight "
               "lets walls in front of a knight hide its name.",
               s.name_bars, {{2, "Through walls"}, {1, "In sight"}, {0, "Off"}}, store_only),
        toggle("online.pickup_feed", "Party's finds",
               "The lines at the side of the picture saying who in the party found, bought, sold or used what.",
               s.pickup_feed, store_only),
        toggle("online.messages", "Messages",
               "Messages other players left on the ground, and leaving your own with the Message Stone. "
               "Connects to the Verdite online service while you play. Always on in a coop room.",
               s.online_messages, store_only),
    }};
}

OptionRow send_away_row(std::size_t slot) {
    OptionRow made = action("online.send_away." + std::to_string(slot), "Send away guest " + std::to_string(slot + 1),
                            "Sends this player out of your room for this session. Their character stays in the "
                            "world, and what they wore goes back in the pack.",
                            [slot] { coop_send_away(slot); });
    made.state = [slot] {
        OptionState state;
        state.note = coop_guest_name(slot);
        state.shown = !state.note.empty();
        return state;
    };
    return made;
}

OptionSection multiplayer_host() {
    Settings &s = settings();
    OptionSection section{"Your room", {
        toggle("online.room_open", "New players",
               "Whether players new to this world may join. Someone who already has a character in it may always "
               "come back.",
               s.room_open, store_only),
        choice("online.revive_wait_s", "Revive wait",
               "The most a player who fell waits to come back beside another, even in the middle of a fight. They "
               "come back sooner once that player is out of the fight.",
               s.revive_wait_s, {{10, "10 seconds"}, {30, "30 seconds"}, {60, "60 seconds"}}, coop_room_settings_changed),
    }};
    for (std::size_t slot = 0; slot < coop_guest_slots; ++slot)
        section.rows.push_back(send_away_row(slot));
    for (OptionRow &each : section.rows)
        each = shown_if(std::move(each), coop_hosting);
    return section;
}

OptionPage multiplayer_page() { return {"Multiplayer", {multiplayer_own(), multiplayer_host()}}; }
#endif

} // namespace

static std::vector<verdite::OptionPage> all_pages() {
    return {display_page(), audio_page(), controls_page(), gameplay_page(), language_page(), interface_page(),
#if KF_ONLINE
            multiplayer_page(),
#endif
            mod_menu_options_page()};
}

verdite::OptionTable &options() {
    static verdite::OptionTable table(all_pages());
    return table;
}

void options_rebuild() {
    options() = verdite::OptionTable(all_pages());
}

verdite::PlatformFeatures platform_features() {
    verdite::PlatformFeatures has = feature(PlatformFeature::Gamepad) | feature(PlatformFeature::FileDialog);
#ifdef __ANDROID__
    has |= feature(PlatformFeature::Touch);
#else
    has |= feature(PlatformFeature::Window) | feature(PlatformFeature::Keyboard) | feature(PlatformFeature::Mouse) |
           feature(PlatformFeature::Discord);
#endif
    return has;
}

void options_poll() {
    std::lock_guard guard(pick_lock);
    if (pick_done == Pick::Patch)
        language_task_patch_file(pick_path);
    else if (pick_done == Pick::Disc)
        language_task_add_disc(pick_path);
    pick_done = Pick::None;
}

std::string options_json() { return verdite::options_json(options().view(platform_features())); }

std::string option_change(std::string_view id, std::string_view text) {
    const OptionRow *found = options().find(id);
    if (!found)
        return "no such option";
    if (found->kind == OptionKind::Action)
        return text.empty() && options().run(id) ? "" : "cannot press it now";
    const auto value = verdite::option_value_from_text(*found, text);
    if (!value)
        return "not a value it takes";
    return options().set(id, *value) ? "" : "cannot change it now";
}
}
