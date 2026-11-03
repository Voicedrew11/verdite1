#include "../ext/setting_rows.h"

#include <doctest/doctest.h>

using namespace kf;
using namespace kf::ext;

TEST_CASE("a control style is one choice over the analog settings") {
    Settings settings;
    for (const ControlStyle style : {ControlStyle::DPad, ControlStyle::Tank, ControlStyle::Twin, ControlStyle::TwinAccelerated}) {
        set_control_style(settings, style);
        CHECK(control_style(settings) == style);
    }
    set_control_style(settings, ControlStyle::TwinAccelerated);
    set_control_style(settings, ControlStyle::Tank);
    CHECK_FALSE(settings.analog.look_acceleration);   // nothing to accelerate: the right stick is not used
    set_control_style(settings, ControlStyle::DPad);
    CHECK(settings.analog.mode == StickMode::Tank);   // off keeps the rest for later
}

TEST_CASE("the look's ease and stop make three states") {
    Settings settings;
    CHECK(look_response(settings) == LookResponse::Eased);
    for (const LookResponse response : {LookResponse::Direct, LookResponse::StopsDead, LookResponse::Eased}) {
        set_look_response(settings, response);
        CHECK(look_response(settings) == response);
    }
    settings.analog.ease = false;
    settings.analog.instant_stop = true;
    CHECK(look_response(settings) == LookResponse::Direct);   // direct stops dead anyway
}

TEST_CASE("dynamic light off keeps the shadows choice") {
    Settings settings;
    CHECK(dynamic_light(settings) == DynamicLight::Shadows);
    set_dynamic_light(settings, DynamicLight::Off);
    CHECK(dynamic_light(settings) == DynamicLight::Off);
    CHECK(settings.shadows);
    set_dynamic_light(settings, DynamicLight::On);
    CHECK(dynamic_light(settings) == DynamicLight::On);
    CHECK_FALSE(settings.shadows);
}

TEST_CASE("ambient occlusion off keeps its quality") {
    Settings settings;
    CHECK(ao_choice(settings) == AoChoice::Medium);
    set_ao_choice(settings, AoChoice::High);
    CHECK(settings.ao_quality == 2);
    set_ao_choice(settings, AoChoice::Off);
    CHECK(ao_choice(settings) == AoChoice::Off);
    CHECK(settings.ao_quality == 2);
}

TEST_CASE("any anisotropy walks the mip chain") {
    Settings settings;
    CHECK(aniso_choice(settings) == 16);
    set_aniso_choice(settings, 0);
    CHECK_FALSE(settings.mipmaps);
    CHECK(settings.anisotropy == 1);
    CHECK(aniso_choice(settings) == 0);
    set_aniso_choice(settings, 1);
    CHECK(settings.mipmaps);
    CHECK(aniso_choice(settings) == 1);
    set_aniso_choice(settings, 4);
    CHECK(settings.anisotropy == 4);
    CHECK(aniso_choice(settings) == 4);
}

TEST_CASE("the enemy turn is automatic or a multiple") {
    Settings settings;
    CHECK(enemy_turn_choice(settings) == enemy_turn_automatic);
    set_enemy_turn_choice(settings, 2.0);
    CHECK_FALSE(settings.enemy_turn_auto);
    CHECK(enemy_turn_choice(settings) == 2.0);
    set_enemy_turn_choice(settings, enemy_turn_automatic);
    CHECK(settings.enemy_turn_auto);
    CHECK(settings.enemy_turn == 2.0f);
}

TEST_CASE("stick sens sets both speeds and is custom when they are apart") {
    Settings settings;
    CHECK(stick_sens(settings) == 1.0);
    set_stick_sens(settings, 1.25);
    CHECK(settings.analog.turn_sensitivity == 1.25f);
    CHECK(settings.analog.pitch_sensitivity == 1.25f);
    CHECK(stick_sens(settings) == 1.25);
    settings.analog.pitch_sensitivity = 0.75f;
    CHECK(stick_sens(settings) == stick_sens_custom);
    settings.analog.turn_sensitivity = settings.analog.pitch_sensitivity = 0.65f;
    CHECK(stick_sens(settings) == stick_sens_custom);   // together, but on no stop
}

TEST_CASE("an old file's mixtures settle on a state a row can show") {
    Settings settings;
    settings.subpixel = true;
    settings.perspective = false;
    settings.fix_ceiling_fall = true;
    settings.fix_spatial_panning = false;
    settings.mipmaps = false;
    settings.anisotropy = 3;
    settings.enemy_turn = 3.1f;
    settings_tidy(settings);
    CHECK(wiggle(settings));
    CHECK_FALSE(bug_fixes(settings));
    CHECK_FALSE(settings.fix_ceiling_fall);
    CHECK(settings.mipmaps);
    CHECK(settings.anisotropy == 2);
    CHECK(settings.enemy_turn == 3.0f);
}

TEST_CASE("settings_tidy leaves reachable settings alone") {
    Settings settings;
    settings_tidy(settings);
    CHECK_FALSE(wiggle(settings));
    CHECK(bug_fixes(settings));
    CHECK(aniso_choice(settings) == 16);
    settings.mipmaps = false;
    settings.anisotropy = 1;
    set_wiggle(settings, true);
    settings_tidy(settings);
    CHECK(wiggle(settings));
    CHECK(aniso_choice(settings) == 0);
}
