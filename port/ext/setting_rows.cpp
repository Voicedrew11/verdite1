#include "setting_rows.h"

#include <algorithm>
#include <cmath>

namespace kf::ext {
namespace {
bool same_speed(double a, double b) { return std::abs(a - b) < 1e-4; }
} // namespace

ControlStyle control_style(const Settings &settings) {
    const AnalogOptions &analog = settings.analog;
    if (!analog.enabled)
        return ControlStyle::DPad;
    if (analog.mode == StickMode::Tank)
        return ControlStyle::Tank;
    return analog.look_acceleration ? ControlStyle::TwinAccelerated : ControlStyle::Twin;
}

void set_control_style(Settings &settings, ControlStyle style) {
    AnalogOptions &analog = settings.analog;
    analog.enabled = style != ControlStyle::DPad;
    if (style == ControlStyle::DPad)
        return;
    analog.mode = style == ControlStyle::Tank ? StickMode::Tank : StickMode::Twin;
    analog.look_acceleration = style == ControlStyle::TwinAccelerated;
}

LookResponse look_response(const Settings &settings) {
    if (!settings.analog.ease)
        return LookResponse::Direct;
    return settings.analog.instant_stop ? LookResponse::StopsDead : LookResponse::Eased;
}

void set_look_response(Settings &settings, LookResponse response) {
    settings.analog.ease = response != LookResponse::Direct;
    settings.analog.instant_stop = response == LookResponse::StopsDead;
}

DynamicLight dynamic_light(const Settings &settings) {
    if (!settings.dynamic_lights)
        return DynamicLight::Off;
    return settings.shadows ? DynamicLight::Shadows : DynamicLight::On;
}

void set_dynamic_light(Settings &settings, DynamicLight light) {
    settings.dynamic_lights = light != DynamicLight::Off;
    if (light != DynamicLight::Off)
        settings.shadows = light == DynamicLight::Shadows;
}

AoChoice ao_choice(const Settings &settings) {
    if (!settings.ambient_occlusion)
        return AoChoice::Off;
    return static_cast<AoChoice>(settings.ao_quality + 1);
}

void set_ao_choice(Settings &settings, AoChoice choice) {
    settings.ambient_occlusion = choice != AoChoice::Off;
    if (choice != AoChoice::Off)
        settings.ao_quality = static_cast<int>(choice) - 1;
}

int aniso_choice(const Settings &settings) { return settings.mipmaps ? std::max(settings.anisotropy, 1) : 0; }

void set_aniso_choice(Settings &settings, int choice) {
    settings.mipmaps = choice > 0;
    settings.anisotropy = std::max(choice, 1);
}

double enemy_turn_choice(const Settings &settings) {
    return settings.enemy_turn_auto ? enemy_turn_automatic : static_cast<double>(settings.enemy_turn);
}

void set_enemy_turn_choice(Settings &settings, double choice) {
    settings.enemy_turn_auto = choice == enemy_turn_automatic;
    if (!settings.enemy_turn_auto)
        settings.enemy_turn = static_cast<float>(choice);
}

double stick_sens(const Settings &settings) {
    const double turn = settings.analog.turn_sensitivity;
    if (!same_speed(turn, settings.analog.pitch_sensitivity))
        return stick_sens_custom;
    const auto stop = std::ranges::find_if(stick_sens_stops, [turn](double each) { return same_speed(each, turn); });
    return stop == stick_sens_stops.end() ? stick_sens_custom : *stop;
}

void set_stick_sens(Settings &settings, double sens) {
    settings.analog.turn_sensitivity = settings.analog.pitch_sensitivity = static_cast<float>(sens);
}

bool wiggle(const Settings &settings) { return !settings.subpixel && !settings.perspective; }

void set_wiggle(Settings &settings, bool on) { settings.subpixel = settings.perspective = !on; }

bool bug_fixes(const Settings &settings) { return settings.fix_ceiling_fall && settings.fix_spatial_panning; }

void set_bug_fixes(Settings &settings, bool on) { settings.fix_ceiling_fall = settings.fix_spatial_panning = on; }

void settings_tidy(Settings &settings) {
    set_wiggle(settings, !(settings.subpixel && settings.perspective));
    set_bug_fixes(settings, bug_fixes(settings));
    // The row's stops are powers of two; a hand-edited 3 goes down to 2.
    int samples = 1;
    while (samples * 2 <= std::clamp(settings.anisotropy, 1, 16))
        samples *= 2;
    settings.anisotropy = samples;
    if (samples > 1)
        settings.mipmaps = true;
    settings.enemy_turn = std::round(std::clamp(settings.enemy_turn, 1.0f, 4.0f) * 4.0f) / 4.0f;
}

} // namespace kf::ext
