// settings.json: display/audio/overlay options plus a "mods" object holding each
// mod's enabled flag and config table. Unknown keys are preserved.
#include "internal.hpp"
#include <cJSON.h>
#include <kf/audio/sound.h>
#include <kf/platform/host.h>
#include <kf/platform/input.h>
#include <kf/platform/language_runtime.h>
#include <kf/renderer/renderer.h>
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
}

std::filesystem::path user_dir() {
    static std::filesystem::path path = [] {
        char *pref = SDL_GetPrefPath("KingsField", "SLPS00017");
        std::filesystem::path result = pref ? std::filesystem::path(reinterpret_cast<const char8_t *>(pref)) : "user";
        SDL_free(pref);
        return result;
    }();
    return path;
}

std::filesystem::path mods_dir() { return user_dir() / "mods"; }

Settings &settings() { return current; }

void settings_load() {
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
    current.resolution = static_cast<int>(read_number(display, "resolution", current.resolution));
    current.subpixel = read_bool(display, "subpixel", current.subpixel);
    current.perspective = read_bool(display, "perspective", current.perspective);
    current.depth_buffer = read_bool(display, "depth_buffer", current.depth_buffer);
    current.true_color = read_bool(display, "true_color", current.true_color);
    current.dynamic_lights = read_bool(display, "dynamic_lights", current.dynamic_lights);
    current.light_strength = static_cast<int>(read_number(display, "light_strength", current.light_strength));
    current.light_occlusion = read_bool(display, "light_occlusion", current.light_occlusion);
    current.ambient_occlusion = read_bool(display, "ambient_occlusion", current.ambient_occlusion);
    current.ao_strength = static_cast<int>(read_number(display, "ao_strength", current.ao_strength));
    current.texture_filter = static_cast<int>(read_number(display, "texture_filter", current.texture_filter));
    current.antialiasing = static_cast<int>(read_number(display, "antialiasing", current.antialiasing));
    current.flame_glow = read_bool(display, "flame_glow", current.flame_glow);
    current.shadows = read_bool(display, "shadows", current.shadows);
    current.shadow_strength = static_cast<int>(read_number(display, "shadow_strength", current.shadow_strength));
    current.glow_strength = static_cast<int>(read_number(display, "glow_strength", current.glow_strength));
    current.frame_rate = static_cast<int>(read_number(display, "frame_rate", current.frame_rate));
    const cJSON *audio = cJSON_GetObjectItemCaseSensitive(document, "audio");
    current.volume = static_cast<int>(read_number(audio, "volume", current.volume));
    const cJSON *controls = cJSON_GetObjectItemCaseSensitive(document, "controls");
    current.run_speed = static_cast<int>(read_number(controls, "run_speed", current.run_speed));
    current.run_toggle = read_bool(controls, "run_toggle", current.run_toggle);
    current.look_limit = static_cast<int>(read_number(controls, "look_limit", current.look_limit));
    const cJSON *gameplay = cJSON_GetObjectItemCaseSensitive(document, "gameplay");
    current.language = read_string(gameplay, "language", current.language);
    current.classic = read_bool(gameplay, "classic_mode", current.classic);
    current.auto_keys = read_bool(gameplay, "auto_use_keys", current.auto_keys);
    current.fix_ceiling_fall = read_bool(gameplay, "fix_ceiling_fall", current.fix_ceiling_fall);
    current.fix_spatial_panning = read_bool(gameplay, "fix_spatial_panning", current.fix_spatial_panning);
    const cJSON *map = cJSON_GetObjectItemCaseSensitive(document, "map");
    current.minimap = read_bool(map, "minimap", current.minimap);
    current.minimap_corner = std::clamp(static_cast<int>(read_number(map, "minimap_corner", current.minimap_corner)), 0, 3);
    current.minimap_size = std::clamp(static_cast<int>(read_number(map, "minimap_size", current.minimap_size)), 10, 50);
    current.minimap_zoom = std::clamp(static_cast<int>(read_number(map, "minimap_zoom", current.minimap_zoom)), 4, 30);
    current.minimap_opacity = std::clamp(static_cast<int>(read_number(map, "minimap_opacity", current.minimap_opacity)), 20, 100);
    current.map_fog = read_bool(map, "fog_of_war", current.map_fog);
    current.map_pause = read_bool(map, "pause_game", current.map_pause);
    current.map_keep_music = read_bool(map, "keep_music", current.map_keep_music);
    current.map_items = read_bool(map, "show_items", current.map_items);
    current.map_chests = read_bool(map, "show_chests", current.map_chests);
    current.map_enemies = read_bool(map, "show_enemies", current.map_enemies);
    current.map_key_hints = read_bool(map, "key_hints", current.map_key_hints);
    const cJSON *overlay = cJSON_GetObjectItemCaseSensitive(document, "overlay");
    current.pause_when_open = read_bool(overlay, "pause_when_open", current.pause_when_open);
    current.ui_scale = static_cast<float>(read_number(overlay, "ui_scale", current.ui_scale));
    current.ui_font = read_string(overlay, "font", current.ui_font);
    current.item_descriptions = read_bool(overlay, "item_descriptions", current.item_descriptions);
}

void settings_mark_dirty() {
    if (!dirty)
        dirty_since = SDL_GetTicks();
    dirty = true;
}

void settings_save_if_dirty() {
    // Debounced: sliders dragged for a second write once.
    if (!dirty || !document || SDL_GetTicks() - dirty_since < 500)
        return;
    dirty = false;
    cJSON *display = object_entry(document, "display");
    write_value(display, "fullscreen", cJSON_CreateBool(current.fullscreen));
    write_value(display, "vsync", cJSON_CreateBool(current.vsync));
    write_value(display, "show_fps", cJSON_CreateBool(current.show_fps));
    write_value(display, "aspect", cJSON_CreateString(current.aspect.c_str()));
    write_value(display, "resolution", cJSON_CreateNumber(current.resolution));
    write_value(display, "subpixel", cJSON_CreateBool(current.subpixel));
    write_value(display, "perspective", cJSON_CreateBool(current.perspective));
    write_value(display, "depth_buffer", cJSON_CreateBool(current.depth_buffer));
    write_value(display, "true_color", cJSON_CreateBool(current.true_color));
    write_value(display, "dynamic_lights", cJSON_CreateBool(current.dynamic_lights));
    write_value(display, "light_strength", cJSON_CreateNumber(current.light_strength));
    write_value(display, "light_occlusion", cJSON_CreateBool(current.light_occlusion));
    write_value(display, "ambient_occlusion", cJSON_CreateBool(current.ambient_occlusion));
    write_value(display, "ao_strength", cJSON_CreateNumber(current.ao_strength));
    write_value(display, "texture_filter", cJSON_CreateNumber(current.texture_filter));
    write_value(display, "antialiasing", cJSON_CreateNumber(current.antialiasing));
    write_value(display, "flame_glow", cJSON_CreateBool(current.flame_glow));
    write_value(display, "shadows", cJSON_CreateBool(current.shadows));
    write_value(display, "shadow_strength", cJSON_CreateNumber(current.shadow_strength));
    write_value(display, "glow_strength", cJSON_CreateNumber(current.glow_strength));
    write_value(display, "frame_rate", cJSON_CreateNumber(current.frame_rate));
    cJSON *audio = object_entry(document, "audio");
    write_value(audio, "volume", cJSON_CreateNumber(current.volume));
    cJSON *controls = object_entry(document, "controls");
    write_value(controls, "run_speed", cJSON_CreateNumber(current.run_speed));
    write_value(controls, "run_toggle", cJSON_CreateBool(current.run_toggle));
    write_value(controls, "look_limit", cJSON_CreateNumber(current.look_limit));
    cJSON *gameplay = object_entry(document, "gameplay");
    write_value(gameplay, "language", cJSON_CreateString(current.language.c_str()));
    write_value(gameplay, "classic_mode", cJSON_CreateBool(current.classic));
    write_value(gameplay, "auto_use_keys", cJSON_CreateBool(current.auto_keys));
    write_value(gameplay, "fix_ceiling_fall", cJSON_CreateBool(current.fix_ceiling_fall));
    write_value(gameplay, "fix_spatial_panning", cJSON_CreateBool(current.fix_spatial_panning));
    cJSON *map = object_entry(document, "map");
    write_value(map, "minimap", cJSON_CreateBool(current.minimap));
    write_value(map, "minimap_corner", cJSON_CreateNumber(current.minimap_corner));
    write_value(map, "minimap_size", cJSON_CreateNumber(current.minimap_size));
    write_value(map, "minimap_zoom", cJSON_CreateNumber(current.minimap_zoom));
    write_value(map, "minimap_opacity", cJSON_CreateNumber(current.minimap_opacity));
    write_value(map, "fog_of_war", cJSON_CreateBool(current.map_fog));
    write_value(map, "pause_game", cJSON_CreateBool(current.map_pause));
    write_value(map, "keep_music", cJSON_CreateBool(current.map_keep_music));
    write_value(map, "show_items", cJSON_CreateBool(current.map_items));
    write_value(map, "show_chests", cJSON_CreateBool(current.map_chests));
    write_value(map, "show_enemies", cJSON_CreateBool(current.map_enemies));
    write_value(map, "key_hints", cJSON_CreateBool(current.map_key_hints));
    cJSON *overlay = object_entry(document, "overlay");
    write_value(overlay, "pause_when_open", cJSON_CreateBool(current.pause_when_open));
    write_value(overlay, "ui_scale", cJSON_CreateNumber(current.ui_scale));
    write_value(overlay, "font", cJSON_CreateString(current.ui_font.c_str()));
    write_value(overlay, "item_descriptions", cJSON_CreateBool(current.item_descriptions));
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
    classic.resolution = 1;           // 320x240, scaled up to the window
    classic.subpixel = false;         // polygon wiggle
    classic.perspective = false;      // affine texture warping
    classic.depth_buffer = false;
    classic.true_color = false;       // 15-bit colour with dithering
    classic.dynamic_lights = false;
    classic.ambient_occlusion = false;
    classic.texture_filter = 0;
    classic.antialiasing = 0;
    classic.flame_glow = false;
    classic.shadows = false;
    classic.frame_rate = 20;
    classic.run_speed = 100;          // no run key
    classic.look_limit = 17;
    classic.auto_keys = false;
    classic.fix_ceiling_fall = false;
    classic.fix_spatial_panning = false;
    classic.minimap = false;
    classic.item_descriptions = false;
    return classic;
}

void apply_display_settings() {
    if (SDL_Window *handle = window())
        SDL_SetWindowFullscreen(handle, current.fullscreen);
    SDL_GL_SetSwapInterval(current.vsync ? 1 : 0);
}

void apply_render_settings() {
    const Settings &e = effective_settings();
    render_subpixel = e.subpixel;
    render_perspective = e.perspective;
    render_depth_buffer = e.depth_buffer;
    render_true_color = e.true_color;
    render_dynamic_lights = e.dynamic_lights;
    render_light_strength = static_cast<float>(e.light_strength) / 100.0f;
    render_light_occlusion = e.light_occlusion;
    render_ambient_occlusion = e.ambient_occlusion;
    render_ao_strength = static_cast<float>(e.ao_strength) / 100.0f;
    render_texture_filter = e.texture_filter;
    render_fxaa = e.antialiasing == 1;
    render_glow = e.flame_glow ? static_cast<float>(e.glow_strength) / 100.0f : 0.0f;
    render_shadows = e.shadows ? static_cast<float>(e.shadow_strength) / 100.0f : 0.0f;
    render_supersample = e.antialiasing == 2 ? 2 : e.antialiasing == 3 ? 3 : 1;
    render_smoothing = e.frame_rate != 20;
    render_frame_limit = e.frame_rate == 20 ? 0 : e.frame_rate;
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
    Renderer *renderer = host_renderer();
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
    if (renderer->logical_width == columns && renderer->scale == scale)
        return;
    if (renderer_configure(renderer, columns, scale))
        std::printf("[display] render target %dx%d (%d columns, %dx)\n", columns * scale, render_height * scale, columns, scale);
    else
        std::printf("[display] cannot create a %dx%d render target\n", columns * scale, render_height * scale);
}

void apply_control_settings() {
    const Settings &e = effective_settings();
    classic_mode = current.classic;
    player_run_speed = std::clamp(e.run_speed, 100, 250);
    player_run_toggle = e.run_toggle;
    // Degrees to 1/4096 turns; never tighter than the original limit, and short of straight up/down.
    const int degrees = std::clamp(e.look_limit, 17, 85);
    player_pitch_limit = std::max(191, degrees * 4096 / 360);
    fix_actor_ceiling_fall = e.fix_ceiling_fall;
    auto_use_keys = e.auto_keys;
    Language language;
    if (language_parse(e.language.c_str(), &language))
        language_preference = language;
    fix_spatial_panning = e.fix_spatial_panning;
}

void apply_audio_settings() {
    sound_set_output_gain(static_cast<float>(current.volume) / 100.0f);
}
}
