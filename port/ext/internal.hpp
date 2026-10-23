#pragma once
#include <kf/ext/ext.h>
#include <filesystem>
#include <string>
#include <vector>

struct cJSON;

namespace kf::ext {
// ---- paths ----
std::filesystem::path user_dir();   // %APPDATA%\KingsField\SLPS00017 (SDL pref path)
std::filesystem::path mods_dir();

// ---- console ----
struct ConsoleLine {
    std::string text;
    std::uint64_t serial;
};
// Copies lines with serial > after into out; returns the newest serial.
std::uint64_t console_copy(std::uint64_t after, std::vector<ConsoleLine> &out);
void console_clear();
void console_pump();   // drains captured output on the main thread

// ---- settings (settings.json in user_dir) ----
struct Settings {
    // Classic mode: play as originally shipped. Every enhancement below is
    // overridden (see effective_settings) while the player's values are kept.
    bool classic = false;
    bool fullscreen = false;
    bool vsync = false;
    bool pause_when_open = false;
    bool show_fps = false;
    int volume = 100;           // percent
    std::string aspect = "4:3"; // "4:3", "16:9", "16:10", "21:9" or "window"
    int resolution = 0;         // 0: native (match the window), else whole multiple of 320x240
    bool subpixel = true;       // smooth vertex positions (less polygon jitter)
    bool perspective = true;    // perspective-correct textures (no warping)
    bool depth_buffer = false;  // experimental: fixes polygons showing through walls
    bool true_color = true;     // smooth colour: no 15-bit banding or dither
    bool dynamic_lights = true; // torches and fire/lightning spells light nearby surfaces
    int light_strength = 100;   // percent
    bool light_occlusion = true; // walls block dynamic light
    bool ambient_occlusion = true;
    int ao_strength = 100;      // percent
    int texture_filter = 0;     // 0 nearest (original), 1 bilinear, 2 three-point
    int antialiasing = 1;       // 0 off, 1 FXAA, 2 4x SSAA, 3 9x SSAA
    bool flame_glow = true;
    bool shadows = true;
    int shadow_strength = 70;   // percent of a light a shadow removes (game lighting untouched)
    int glow_strength = 100;    // percent
    int frame_rate = 0;         // 20: original (no smoothing), 0: display refresh, -1: unlimited, else cap
    int run_speed = 160;        // percent of walking speed while Shift is held
    bool run_toggle = false;    // Shift toggles running instead of being held
    int look_limit = 70;        // degrees the camera may tilt up or down (original: 17)
    std::string language = "en";   // game language when both are set up: "en" or "ja"
    bool auto_keys = true;          // interacting with a lock uses a carried key
    bool fix_ceiling_fall = true;   // bug fix: enemies fall after hitting a ceiling mid-jump
    bool fix_spatial_panning = true; // bug fix: flagged sounds get their panning adjustment
    bool minimap = true;
    int minimap_corner = 1;     // 0 top right, 1 bottom right, 2 top left, 3 bottom left
    int minimap_size = 28;      // percent of the picture's height
    int minimap_zoom = 8;      // cells from the centre to the edge
    int minimap_opacity = 85;   // percent
    bool map_fog = true;        // only draw what has been seen
    bool map_pause = true;      // the full map pauses the game
    bool map_keep_music = true; // ...but the music keeps playing
    bool map_items = false;     // cheat markers
    bool map_chests = false;
    bool map_enemies = false;
    bool map_key_hints = false; // cheat: name the key a locked door or chest needs
    float ui_scale = 1.0f;
    bool item_descriptions = true;  // what an item does, in the game's item menus
    std::string ui_font;        // empty: first available system font
};
Settings &settings();
// The settings in effect: the player's, or with Classic mode on, the player's
// with every enhancement set to the original behaviour.
const Settings &effective_settings();
void settings_load();
void settings_mark_dirty();
void settings_save_if_dirty();
// mods.<id> object in settings.json, created on demand. Owned by the settings document.
cJSON *settings_mod_entry(const std::string &id);
void apply_display_settings();
void apply_audio_settings();
void apply_control_settings();
void apply_render_settings();   // also on window resize

// What an item does (effects only; the menus show equipment stats), or empty.
std::string item_description(std::uint8_t id);
// Draws the description of the item in the game menu's preview panel, if any.
void item_description_draw();

// ---- automap ----
void automap_draw(int input_context);   // minimap or full map, before the overlay windows
void automap_toggle_full();
bool automap_full_open();
void automap_settings_page(bool &changed);

// ---- mods ----
void mods_start();
void mods_shutdown();
void mods_poll();                 // hot reload
void mods_frame(int input_context);
void mods_draw_hud();             // each enabled mod's on_draw, inside the ImGui frame
void mods_draw_panel();           // the Mods window contents
int mods_loaded_count();

// ---- overlay helpers shared with the mod API ----
// Slider with a typed-value box beside it. `committed` (optional) is set when an
// edit finishes: slider released, or Enter / focus change in the box.
bool slider_input(const char *label, float *value, float min, float max, const char *format,
                  float snap = 0.0f, bool *committed = nullptr);
bool slider_input(const char *label, int *value, int min, int max, const char *format = "%d",
                  bool *committed = nullptr);
inline constexpr float hud_font_size = 18.0f;   // mod drawing ignores the interface scale
struct ViewRect { float x, y, w, h; };
ViewRect game_view();             // the whole presented picture (wider than 4:3 in widescreen)
ViewRect original_view();         // the original 320x240 area within it
bool overlay_captures_input();
SDL_Window *window();
}
