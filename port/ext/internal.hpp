#pragma once
#include <kf/ext/ext.h>
#include <kf/platform/analog.h>
#include <verdite/options.h>
#include <filesystem>
#include <string>
#include <vector>

struct cJSON;

namespace kf::ext {
// ---- paths ----
std::filesystem::path user_dir();   // kf::user_folder(), or "user" without one
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
#ifdef __ANDROID__
// A phone: the whole screen, with no status or navigation bar over the picture.
inline constexpr bool default_fullscreen = true;
inline constexpr const char *default_aspect = "window";
#else
inline constexpr bool default_fullscreen = false;
inline constexpr const char *default_aspect = "4:3";
#endif

struct Settings {
    // Classic mode: play as originally shipped. Every enhancement below is
    // overridden (see effective_settings) while the player's values are kept.
    bool classic = false;
    bool fullscreen = default_fullscreen;
    bool vsync = false;
    bool pause_when_open = false;
    bool show_fps = false;
    int volume = 100;           // percent
    std::string aspect = default_aspect; // "4:3", "16:9", "16:10", "21:9" or "window"
    bool hud_at_edges = true;   // in widescreen, the HUD at the picture's edges, not in its 4:3 box
    int resolution = 0;         // 0: native (match the window), else whole multiple of 320x240
    bool subpixel = true;       // smooth vertex positions (less polygon jitter)
    bool perspective = true;    // perspective-correct textures (no warping)
    bool depth_buffer = false;  // experimental: fixes polygons showing through walls
    bool true_color = true;     // smooth colour: no 15-bit banding or dither
    bool pixel_lighting = true; // the game's lights per pixel on models
    bool dynamic_lights = true; // torches and fire/lightning spells light nearby surfaces
    int light_strength = 100;   // percent
    bool light_occlusion = true; // walls block dynamic light
    bool ambient_occlusion = true;
    int ao_strength = 100;      // percent; 100 is Verdite's judged look
    int ao_quality = 1;         // 0 low, 1 medium, 2 high (OcclusionQuality)
    int texture_filter = 3;     // 0 nearest (original), 1 bilinear, 2 three-point, 3 sharp
    bool mipmaps = true;        // distant textures averaged instead of shimmering
    bool detailed_map = true;   // distant map cells keep their detailed meshes
    int anisotropy = 16;        // 1 (off), 2, 4, 8 or 16 samples along slanted textures
    int enhanced_distance = 0;  // cells past which the picture is the game's own; 0: everywhere
    int fog = 2;                // 0 at the corners (original), 1 per pixel by depth, 2 by distance (FogMode)
    int antialiasing = 1;       // 0 off, 1 FXAA, 2 4x SSAA, 3 9x SSAA
    bool flame_glow = true;
    bool shadows = true;
    int shadow_strength = 70;   // percent of a light a shadow removes (game lighting untouched)
    int glow_strength = 100;    // percent
    int frame_rate = 0;         // 20: original (no smoothing), 0: display refresh, -1: unlimited, else cap
    int tick_rate = 15;         // world updates a second: 12, 15, 20 (the gate's) or 30
    int run_speed = 160;        // percent of walking speed while Shift is held
    bool run_toggle = false;    // Shift toggles running instead of being held
    int look_limit = 70;        // degrees the camera may tilt up or down (original: 17)
    std::string pad_layout = "west";   // gamepad face buttons: "west" (the US game's) or "jp"
    bool menu_pointer = true;   // point at and click the game's menus with the mouse
    bool touch_haptics = true;  // the on-screen buttons click the phone's vibrator
    AnalogOptions analog;       // the sticks, saved as "analog"
    std::string language = "en";   // game language when both are set up: "en" or "ja"
    bool auto_keys = true;          // interacting with a lock uses a carried key
    bool enemy_turn_auto = true;    // the enemy turn follows how fast the last input to turn can turn
    float enemy_turn = 1.5f;        // fixed: an enemy's turn toward the player, times its own (1: original)
    bool enemy_track = true;        // fixed, above x1: a pursuing enemy re-aims at the player every update
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
    bool discord = true;        // Discord Rich Presence, while Discord is running
    bool discord_level = true;
    bool discord_location = true;   // floor and place
    bool discord_vitals = true;     // HP and MP
    bool discord_exp = true;        // EXP to the next level
    int setup_seen = 0;             // the first-run setup's version last finished or skipped
};
Settings &settings();
// The settings in effect: the player's, or with Classic mode on, the player's
// with every enhancement set to the original behaviour.
const Settings &effective_settings();
void settings_load();
void settings_mark_dirty();
void settings_save_if_dirty();
// Writes settings.json now (exit, or Android sending the app to the background).
void settings_save();
// mods.<id> object in settings.json, created on demand. Owned by the settings document.
cJSON *settings_mod_entry(const std::string &id);
void apply_display_settings();
void apply_audio_settings();
void apply_control_settings();
void apply_render_settings();   // also on window resize
// The settings as option rows (options.cpp): what the overlay's Settings window
// and Android's settings panel draw.
verdite::OptionTable &options();
verdite::PlatformFeatures platform_features();   // what this build's platform has
void options_poll();   // on every host poll: a file the Language page asked for
// The render target apply_render_settings last set: logical columns and scale.
struct RenderTarget { int columns, scale; };
RenderTarget applied_render_target();

// What an item does (effects only; the menus show equipment stats), or empty.
std::string item_description(std::uint8_t id);
// Draws the description of the item in the game menu's preview panel, if any.
void item_description_draw();

// ---- the first-run setup (setup.cpp) ----
bool setup_available();            // this platform has a screen for it
void setup_begin(Overlay overlay); // at start: shown if due and drawn
void setup_run();                  // from the start, again
bool setup_open();
void setup_draw();                 // inside the ImGui frame (setup_json, setup_command: ext.h)

// ---- Discord Rich Presence ----
void discord_start();     // reads KF_DISCORD and KF_DISCORD_APP
void discord_poll();      // on every host poll: composes the presence and hands it over
void discord_stop();      // takes the presence down
bool discord_forced();    // KF_DISCORD decides for this run
bool discord_enabled();   // the presence is on, by the setting or KF_DISCORD

// ---- automap ----
void automap_draw(int input_context);   // minimap or full map, before the overlay windows
void automap_toggle_full();
bool automap_full_open();

// ---- touch controls ----
void touch_controls_draw();   // inside the ImGui frame
void touch_commands_poll();   // the gear and the map button, on every host poll
void overlay_set_open(bool open);
void overlay_restyle();   // after the interface scale changes

// ---- Android's settings panel (settings_panel.cpp): native views over the game ----
// Shows the panel in place of the overlay's windows; false where there is none.
bool settings_panel_show();
bool settings_panel_open();
void settings_panel_poll();   // on every host poll: the panel's changes in, the new view out

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
