// settings.json: display/audio/overlay options plus a "mods" object holding each
// mod's enabled flag and config table. Unknown keys are preserved.
#include "internal.hpp"
#include <cJSON.h>
#include <kf/audio/sound.h>
#include <kf/lib/map_types.h>
#include <kf/platform/host.h>
#include <kf/platform/analog.h>
#include <kf/platform/enemy_turn.h>
#include <kf/platform/input.h>
#include <kf/platform/language_runtime.h>
#include <kf/platform/menu_pointer.h>
#include <kf/platform/touch.h>
#include <kf/platform/user_folder.h>
#include <kf/renderer/frontend.h>
#include <kf/renderer/renderer.h>
#include <verdite/crash.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace kf::ext {
namespace {
Settings current;
cJSON *document;
bool dirty;
Uint64 dirty_since;

std::filesystem::path settings_path() { return user_dir() / "settings.json"; }

bool read_bool(const cJSON *object, const char *key, bool fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}
double read_number(const cJSON *object, const char *key, double fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}
std::string read_string(const cJSON *object, const char *key, const std::string &fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : fallback;
}
float read_float(const cJSON *object, const char *key, float fallback) {
    return static_cast<float>(read_number(object, key, fallback));
}

AnalogOptions read_analog(const cJSON *analog, AnalogOptions o) {
    o.enabled = read_bool(analog, "enabled", o.enabled);
    o.mode = read_string(analog, "sticks", o.mode == StickMode::Tank ? "tank" : "twin") == "tank" ? StickMode::Tank
                                                                                                   : StickMode::Twin;
    o.turn_sensitivity = read_float(analog, "turn_sensitivity", o.turn_sensitivity);
    o.pitch_sensitivity = read_float(analog, "pitch_sensitivity", o.pitch_sensitivity);
    o.move_sensitivity = read_float(analog, "move_sensitivity", o.move_sensitivity);
    o.look_deadzone = read_float(analog, "look_deadzone", o.look_deadzone);
    o.move_deadzone = read_float(analog, "move_deadzone", o.move_deadzone);
    o.look_curve = read_float(analog, "look_curve", o.look_curve);
    o.move_curve = read_float(analog, "move_curve", o.move_curve);
    o.invert_turn = read_bool(analog, "invert_turn", o.invert_turn);
    o.invert_pitch = read_bool(analog, "invert_pitch", o.invert_pitch);
    o.invert_strafe = read_bool(analog, "invert_strafe", o.invert_strafe);
    o.invert_forward = read_bool(analog, "invert_forward", o.invert_forward);
    o.ease = read_bool(analog, "ease", o.ease);
    o.instant_stop = read_bool(analog, "instant_stop", o.instant_stop);
    o.look_acceleration = read_bool(analog, "look_acceleration", o.look_acceleration);
    o.acceleration_max = read_float(analog, "acceleration_max", o.acceleration_max);
    o.acceleration_time_s = read_float(analog, "acceleration_time_s", o.acceleration_time_s);
    return o;
}

cJSON *object_entry(cJSON *parent, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsObject(item)) {
        cJSON_DeleteItemFromObjectCaseSensitive(parent, key);
        item = cJSON_AddObjectToObject(parent, key);
    }
    return item;
}
void write_value(cJSON *parent, const char *key, cJSON *value) {
    if (cJSON_GetObjectItemCaseSensitive(parent, key))
        cJSON_ReplaceItemInObjectCaseSensitive(parent, key, value);
    else
        cJSON_AddItemToObject(parent, key, value);
}

void write_analog(cJSON *analog, const AnalogOptions &o) {
    write_value(analog, "enabled", cJSON_CreateBool(o.enabled));
    write_value(analog, "sticks", cJSON_CreateString(o.mode == StickMode::Tank ? "tank" : "twin"));
    write_value(analog, "turn_sensitivity", cJSON_CreateNumber(o.turn_sensitivity));
    write_value(analog, "pitch_sensitivity", cJSON_CreateNumber(o.pitch_sensitivity));
    write_value(analog, "move_sensitivity", cJSON_CreateNumber(o.move_sensitivity));
    write_value(analog, "look_deadzone", cJSON_CreateNumber(o.look_deadzone));
    write_value(analog, "move_deadzone", cJSON_CreateNumber(o.move_deadzone));
    write_value(analog, "look_curve", cJSON_CreateNumber(o.look_curve));
    write_value(analog, "move_curve", cJSON_CreateNumber(o.move_curve));
    write_value(analog, "invert_turn", cJSON_CreateBool(o.invert_turn));
    write_value(analog, "invert_pitch", cJSON_CreateBool(o.invert_pitch));
    write_value(analog, "invert_strafe", cJSON_CreateBool(o.invert_strafe));
    write_value(analog, "invert_forward", cJSON_CreateBool(o.invert_forward));
    write_value(analog, "ease", cJSON_CreateBool(o.ease));
    write_value(analog, "instant_stop", cJSON_CreateBool(o.instant_stop));
    write_value(analog, "look_acceleration", cJSON_CreateBool(o.look_acceleration));
    write_value(analog, "acceleration_max", cJSON_CreateNumber(o.acceleration_max));
    write_value(analog, "acceleration_time_s", cJSON_CreateNumber(o.acceleration_time_s));
}
}

std::filesystem::path user_dir() {
    const auto &folder = user_folder();
    return folder.empty() ? std::filesystem::path("user") : folder;
}

std::filesystem::path mods_dir() { return user_dir() / "mods"; }

Settings &settings() { return current; }

void settings_load() {
    static bool loaded = false;
    if (loaded)
        return;
    loaded = true;
    std::ifstream file(settings_path(), std::ios::binary);
    std::stringstream text;
    if (file)
        text << file.rdbuf();
    document = cJSON_Parse(text.str().c_str());
    if (!cJSON_IsObject(document)) {
        if (file)
            std::printf("[settings] settings.json is not valid JSON; starting from defaults\n");
        cJSON_Delete(document);
        document = cJSON_CreateObject();
    }
    const cJSON *display = cJSON_GetObjectItemCaseSensitive(document, "display");
    current.fullscreen = read_bool(display, "fullscreen", current.fullscreen);
    current.vsync = read_bool(display, "vsync", current.vsync);
    current.show_fps = read_bool(display, "show_fps", current.show_fps);
    current.aspect = read_string(display, "aspect", current.aspect);
    current.hud_at_edges = read_bool(display, "hud_at_edges", current.hud_at_edges);
    current.resolution = static_cast<int>(read_number(display, "resolution", current.resolution));
    current.subpixel = read_bool(display, "subpixel", current.subpixel);
    current.perspective = read_bool(display, "perspective", current.perspective);
    current.depth_buffer = read_bool(display, "depth_buffer", current.depth_buffer);
    current.true_color = read_bool(display, "true_color", current.true_color);
    current.pixel_lighting = read_bool(display, "pixel_lighting", current.pixel_lighting);
    current.dynamic_lights = read_bool(display, "dynamic_lights", current.dynamic_lights);
    current.ambient_occlusion = read_bool(display, "ambient_occlusion", current.ambient_occlusion);
    current.ao_quality = std::clamp(static_cast<int>(read_number(display, "ao_quality", current.ao_quality)), 0, 2);
    current.texture_filter = static_cast<int>(read_number(display, "texture_filter", current.texture_filter));
    current.mipmaps = read_bool(display, "mipmaps", current.mipmaps);
    current.detailed_map = read_bool(display, "detailed_map", current.detailed_map);
    current.anisotropy = std::clamp(static_cast<int>(read_number(display, "anisotropy", current.anisotropy)), 1, 16);
    current.enhanced_distance =
        std::clamp(static_cast<int>(read_number(display, "enhanced_distance", current.enhanced_distance)), 0, 16);
    current.fog = std::clamp(static_cast<int>(read_number(display, "fog", current.fog)), 0, 2);
    current.antialiasing = static_cast<int>(read_number(display, "antialiasing", current.antialiasing));
    current.flame_glow = read_bool(display, "flame_glow", current.flame_glow);
    current.shadows = read_bool(display, "shadows", current.shadows);
    current.frame_rate = static_cast<int>(read_number(display, "frame_rate", current.frame_rate));
    const cJSON *audio = cJSON_GetObjectItemCaseSensitive(document, "audio");
    current.volume = static_cast<int>(read_number(audio, "volume", current.volume));
    const cJSON *controls = cJSON_GetObjectItemCaseSensitive(document, "controls");
    current.look_limit = static_cast<int>(read_number(controls, "look_limit", current.look_limit));
    current.pad_layout = read_string(controls, "pad_layout", current.pad_layout);
    current.touch_haptics = read_bool(controls, "touch_haptics", current.touch_haptics);
    current.analog = read_analog(cJSON_GetObjectItemCaseSensitive(document, "analog"), current.analog);
    const cJSON *gameplay = cJSON_GetObjectItemCaseSensitive(document, "gameplay");
    current.language = read_string(gameplay, "language", current.language);
    current.classic = read_bool(gameplay, "classic_mode", current.classic);
    current.auto_keys = read_bool(gameplay, "auto_use_keys", current.auto_keys);
    current.reload_on_death = read_bool(gameplay, "reload_on_death", current.reload_on_death);
    current.tick_rate = static_cast<int>(read_number(gameplay, "tick_rate", current.tick_rate));
    current.enemy_turn_auto = read_bool(gameplay, "enemy_turn_auto", current.enemy_turn_auto);
    current.enemy_turn = std::clamp(static_cast<float>(read_number(gameplay, "enemy_turn", current.enemy_turn)), 1.0f, 4.0f);
    const cJSON *map = cJSON_GetObjectItemCaseSensitive(document, "map");
    const cJSON *overlay = cJSON_GetObjectItemCaseSensitive(document, "overlay");
    current.pause_when_open = read_bool(overlay, "pause_when_open", current.pause_when_open);
    current.ui_scale = static_cast<float>(read_number(overlay, "ui_scale", current.ui_scale));
    current.setup_seen = static_cast<int>(read_number(overlay, "setup_seen", current.setup_seen));
    current.ui_font = read_string(overlay, "font", current.ui_font);
    current.item_descriptions = read_bool(overlay, "item_descriptions", current.item_descriptions);
    current.update_check = read_bool(overlay, "update_check", current.update_check);
    const cJSON *discord = cJSON_GetObjectItemCaseSensitive(document, "discord");
    current.discord = read_bool(discord, "enabled", current.discord);
    current.discord_level = read_bool(discord, "level", current.discord_level);
    current.discord_location = read_bool(discord, "location", current.discord_location);
    current.discord_vitals = read_bool(discord, "vitals", current.discord_vitals);
    current.discord_exp = read_bool(discord, "exp", current.discord_exp);
    const cJSON *online = cJSON_GetObjectItemCaseSensitive(document, "online");
    current.online_messages = read_bool(online, "messages", current.online_messages);
    current.name_bars = std::clamp(static_cast<int>(read_number(online, "name_bars", current.name_bars)), 0, 2);
    current.pickup_feed = read_bool(online, "pickup_feed", current.pickup_feed);
    current.room_open = read_bool(online, "room_open", current.room_open);
    current.revive_wait_s = static_cast<int>(read_number(online, "revive_wait_s", current.revive_wait_s));
    if (current.revive_wait_s != 10 && current.revive_wait_s != 60)
        current.revive_wait_s = 30;
    settings_tidy(current);
}

void settings_mark_dirty() {
    if (!dirty)
        dirty_since = SDL_GetTicks();
    dirty = true;
}

void settings_save_if_dirty() {
    // Debounced: sliders dragged for a second write once.
    if (!dirty || SDL_GetTicks() - dirty_since < 500 || settings_page_holding())
        return;
    settings_save();
}

void settings_save() {
    if (!document)
        return;
    dirty = false;
    cJSON *display = object_entry(document, "display");
    write_value(display, "fullscreen", cJSON_CreateBool(current.fullscreen));
    write_value(display, "vsync", cJSON_CreateBool(current.vsync));
    write_value(display, "show_fps", cJSON_CreateBool(current.show_fps));
    write_value(display, "aspect", cJSON_CreateString(current.aspect.c_str()));
    write_value(display, "hud_at_edges", cJSON_CreateBool(current.hud_at_edges));
    write_value(display, "resolution", cJSON_CreateNumber(current.resolution));
    write_value(display, "subpixel", cJSON_CreateBool(current.subpixel));
    write_value(display, "perspective", cJSON_CreateBool(current.perspective));
    write_value(display, "depth_buffer", cJSON_CreateBool(current.depth_buffer));
    write_value(display, "true_color", cJSON_CreateBool(current.true_color));
    write_value(display, "pixel_lighting", cJSON_CreateBool(current.pixel_lighting));
    write_value(display, "dynamic_lights", cJSON_CreateBool(current.dynamic_lights));
    write_value(display, "ambient_occlusion", cJSON_CreateBool(current.ambient_occlusion));
    write_value(display, "ao_quality", cJSON_CreateNumber(current.ao_quality));
    write_value(display, "texture_filter", cJSON_CreateNumber(current.texture_filter));
    write_value(display, "mipmaps", cJSON_CreateBool(current.mipmaps));
    write_value(display, "detailed_map", cJSON_CreateBool(current.detailed_map));
    write_value(display, "anisotropy", cJSON_CreateNumber(current.anisotropy));
    write_value(display, "fog", cJSON_CreateNumber(current.fog));
    write_value(display, "enhanced_distance", cJSON_CreateNumber(current.enhanced_distance));
    write_value(display, "antialiasing", cJSON_CreateNumber(current.antialiasing));
    write_value(display, "flame_glow", cJSON_CreateBool(current.flame_glow));
    write_value(display, "shadows", cJSON_CreateBool(current.shadows));
    write_value(display, "frame_rate", cJSON_CreateNumber(current.frame_rate));
    cJSON *audio = object_entry(document, "audio");
    write_value(audio, "volume", cJSON_CreateNumber(current.volume));
    cJSON *controls = object_entry(document, "controls");
    write_value(controls, "look_limit", cJSON_CreateNumber(current.look_limit));
    write_value(controls, "pad_layout", cJSON_CreateString(current.pad_layout.c_str()));
    write_value(controls, "touch_haptics", cJSON_CreateBool(current.touch_haptics));
    write_analog(object_entry(document, "analog"), current.analog);
    cJSON *gameplay = object_entry(document, "gameplay");
    write_value(gameplay, "language", cJSON_CreateString(current.language.c_str()));
    write_value(gameplay, "classic_mode", cJSON_CreateBool(current.classic));
    write_value(gameplay, "auto_use_keys", cJSON_CreateBool(current.auto_keys));
    write_value(gameplay, "reload_on_death", cJSON_CreateBool(current.reload_on_death));
    write_value(gameplay, "tick_rate", cJSON_CreateNumber(current.tick_rate));
    write_value(gameplay, "enemy_turn_auto", cJSON_CreateBool(current.enemy_turn_auto));
    write_value(gameplay, "enemy_turn", cJSON_CreateNumber(current.enemy_turn));
    cJSON *map = object_entry(document, "map");
    cJSON *overlay = object_entry(document, "overlay");
    write_value(overlay, "pause_when_open", cJSON_CreateBool(current.pause_when_open));
    write_value(overlay, "ui_scale", cJSON_CreateNumber(current.ui_scale));
    write_value(overlay, "setup_seen", cJSON_CreateNumber(current.setup_seen));
    write_value(overlay, "font", cJSON_CreateString(current.ui_font.c_str()));
    write_value(overlay, "item_descriptions", cJSON_CreateBool(current.item_descriptions));
    write_value(overlay, "update_check", cJSON_CreateBool(current.update_check));
    cJSON *discord = object_entry(document, "discord");
    write_value(discord, "enabled", cJSON_CreateBool(current.discord));
    write_value(discord, "level", cJSON_CreateBool(current.discord_level));
    write_value(discord, "location", cJSON_CreateBool(current.discord_location));
    write_value(discord, "vitals", cJSON_CreateBool(current.discord_vitals));
    write_value(discord, "exp", cJSON_CreateBool(current.discord_exp));
    cJSON *online = object_entry(document, "online");
    write_value(online, "messages", cJSON_CreateBool(current.online_messages));
    write_value(online, "name_bars", cJSON_CreateNumber(current.name_bars));
    write_value(online, "pickup_feed", cJSON_CreateBool(current.pickup_feed));
    write_value(online, "room_open", cJSON_CreateBool(current.room_open));
    write_value(online, "revive_wait_s", cJSON_CreateNumber(current.revive_wait_s));
    char *text = cJSON_Print(document);
    if (!text)
        return;
    const auto temporary = settings_path().string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << text;
    }
    cJSON_free(text);
    std::error_code error;
    std::filesystem::rename(temporary, settings_path(), error);
    if (error)
        std::printf("[settings] cannot save settings.json: %s\n", error.message().c_str());
}

cJSON *settings_mod_entry(const std::string &id) {
    if (!document)
        document = cJSON_CreateObject();
    return object_entry(object_entry(document, "mods"), id.c_str());
}

const Settings &effective_settings() {
    if (!current.classic)
        return current;
    static Settings classic;
    classic = current;
    classic.aspect = "4:3";
    classic.hud_at_edges = false;
    classic.resolution = 1;           // 320x240, scaled up to the window
    classic.subpixel = false;         // polygon wiggle
    classic.perspective = false;      // affine texture warping
    classic.depth_buffer = false;
    classic.true_color = false;       // 15-bit colour with dithering
    classic.pixel_lighting = false;
    classic.dynamic_lights = false;
    classic.ambient_occlusion = false;
    classic.texture_filter = 0;
    classic.mipmaps = false;
    classic.detailed_map = false;
    classic.anisotropy = 1;
    classic.fog = 0;
    classic.antialiasing = 0;
    classic.flame_glow = false;
    classic.shadows = false;
    classic.frame_rate = 20;
    classic.tick_rate = 20;
    classic.look_limit = 17;
    classic.auto_keys = false;
    classic.reload_on_death = false;
    classic.enemy_turn_auto = false;
    classic.enemy_turn = 1.0f;
    classic.analog.enabled = false;   // the game came out before the DualShock
    classic.fix_ceiling_fall = false;
    classic.fix_spatial_panning = false;
    classic.item_descriptions = false;
    return classic;
}

namespace {
RenderTarget applied_target{render_width, 1};
// The share of a light a shadow removes; the game's own lighting is untouched. The judged look.
constexpr float shadow_strength = 0.7f;

// The Display settings in the frontend's terms (frontend.h).
PictureOptions picture_options(const Settings &e) {
    PictureOptions options;
    options.true_color = e.true_color;
    options.depth_buffer = e.depth_buffer;
    options.texture_filter = static_cast<TextureFilter>(std::clamp(e.texture_filter, 0, 3));
    options.smoothing = e.frame_rate != 20;
    options.supersample = render_supersample;
    options.pixel_lighting = e.pixel_lighting;
    options.dynamic_lights = e.dynamic_lights;
    options.light_strength = 1.0f;
    options.light_occlusion = true;
    options.shadows = e.shadows ? shadow_strength : 0.0f;
    options.ambient_occlusion = e.ambient_occlusion ? 1.0f : 0.0f;
    options.occlusion_quality = static_cast<OcclusionQuality>(std::clamp(e.ao_quality, 0, 2));
    options.glow = e.flame_glow ? 1.0f : 0.0f;
    options.fxaa = e.antialiasing == 1;
    options.mipmaps = e.mipmaps;
    options.anisotropy = e.anisotropy;
    options.fog = static_cast<FogMode>(std::clamp(e.fog, 0, 2));
    options.enhanced_depth = static_cast<float>(e.enhanced_distance * KF_MAP_TILE_SIZE);
    options.vsync = current.vsync && e.frame_rate != frame_rate_unlimited;
    return options;
}
} // namespace

RenderTarget applied_render_target() { return applied_target; }

void apply_display_settings() {
    if (SDL_Window *handle = window())
        SDL_SetWindowFullscreen(handle, current.fullscreen);
    host_set_picture_options(picture_options(effective_settings()));
}

void apply_render_settings() {
    const Settings &e = effective_settings();
    render_subpixel = e.subpixel;
    render_perspective = e.perspective;
    render_detailed_map = e.detailed_map;
    render_hud_at_edges = e.hud_at_edges;
    render_dynamic_lights = e.dynamic_lights;
    render_light_occlusion = true;
    render_supersample = e.antialiasing == 2 ? 2 : e.antialiasing == 3 ? 3 : 1;
    render_smoothing = e.frame_rate != 20;
    render_frame_limit = e.frame_rate == 20 ? 0 : e.frame_rate;
    host_set_picture_options(picture_options(e));
    SDL_Window *handle = window();
    if (!handle)
        return;
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(handle, &width, &height);
    if (width <= 0 || height <= 0)
        return;
    // Logical columns for 240 rows: 4:3 is the original 320.
    double aspect = 4.0 / 3.0;
    if (e.aspect == "16:9") aspect = 16.0 / 9.0;
    else if (e.aspect == "16:10") aspect = 16.0 / 10.0;
    else if (e.aspect == "21:9") aspect = 64.0 / 27.0;
    else if (e.aspect == "window") aspect = static_cast<double>(width) / height;
    int columns = static_cast<int>(std::lround(render_height * aspect));
    columns = std::clamp(columns + (columns & 1), render_width, 640);
    int scale = e.resolution;
    if (scale <= 0) {
        // Native: enough pixels to cover the picture's height in the window.
        const double shown = std::min(static_cast<double>(height), width * render_height / static_cast<double>(columns));
        scale = static_cast<int>(std::ceil(shown / render_height - 0.01));
    }
    // Supersampling renders N times the pixels per axis; presentation averages them.
    scale = std::clamp(scale * render_supersample, 1, 16);
    if (columns * scale > 16384)
        scale = 16384 / columns;
    if (applied_target.columns == columns && applied_target.scale == scale)
        return;
    if (host_set_render_target(columns, scale)) {
        applied_target = {columns, scale};
        std::printf("[display] render target %dx%d (%d columns, %dx)\n", columns * scale, render_height * scale, columns, scale);
    } else {
        std::printf("[display] cannot create a %dx%d render target\n", columns * scale, render_height * scale);
    }
}

void apply_control_settings() {
    const Settings &e = effective_settings();
    classic_mode = current.classic;
    // Degrees to 1/4096 turns; never tighter than the original limit, and short of straight up/down.
    const int degrees = std::clamp(e.look_limit, 17, 85);
    player_pitch_limit = std::max(191, degrees * 4096 / 360);
    fix_actor_ceiling_fall = e.fix_ceiling_fall;
    auto_use_keys = e.auto_keys;
    host_set_enemy_turn({e.enemy_turn_auto ? EnemyTurnMode::Automatic : EnemyTurnMode::Fixed, e.enemy_turn,
                         stick_turn_reach(e.analog)});
    host_set_pad_layout(e.pad_layout == "jp" ? PadLayout::Japanese : PadLayout::West);
    touch_button_haptics = e.touch_haptics;
    host_set_analog_options(e.analog);
    // Whole vblanks of the 60 Hz clock, as the console's gate counts.
    // In a room the host's, as every player must run at one speed.
    world_update_vblanks = std::clamp(60 / std::max(coop_room_tick_rate().value_or(e.tick_rate), 1), 2, 5);
    Language language;
    if (language_parse(e.language.c_str(), &language))
        language_preference = language;
    fix_spatial_panning = e.fix_spatial_panning;
}

void apply_audio_settings() {
    sound_set_output_gain(static_cast<float>(current.volume) / 100.0f);
}

void crash_write_settings(verdite::CrashWriter &out) {
    const Settings &s = current;
    out.line("classic mode", s.classic);
    out.line("tick rate", s.tick_rate);
    out.line("frame rate", s.frame_rate);
    out.line("enemy turn automatic", s.enemy_turn_auto);
    out.line("enemy turn percent", static_cast<std::int64_t>(s.enemy_turn * 100.0f));
    out.line("fix ceiling fall", s.fix_ceiling_fall);
    out.line("auto keys", s.auto_keys);
    out.line("reload on death", s.reload_on_death);
    out.line("aspect", s.aspect);
    out.line("resolution", s.resolution);
    out.line("antialiasing", s.antialiasing);
    out.line("depth buffer", s.depth_buffer);
    out.line("pixel lighting", s.pixel_lighting);
    out.line("ambient occlusion", s.ambient_occlusion);
    out.line("fog", s.fog);
    out.line("enhanced distance", s.enhanced_distance);
    out.line("language", s.language);
}
}
