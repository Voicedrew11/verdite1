#pragma once
// What settings.json holds, as plain data. No SDL and no game, so its logic can be tested alone.
#include <kf/platform/analog.h>
#include <string>

namespace kf::ext {
#ifdef __ANDROID__
// A phone: the whole screen, with no status or navigation bar over the picture.
inline constexpr bool default_fullscreen = true;
inline constexpr const char *default_aspect = "window";
#else
inline constexpr bool default_fullscreen = false;
inline constexpr const char *default_aspect = "4:3";
#endif

inline constexpr int frame_rate_unlimited = -1;

struct Settings {
    // Widest first, so the fields pack.
    std::string aspect = default_aspect; // "4:3", "16:9", "16:10", "21:9" or "window"
    std::string pad_layout = "west";   // gamepad face buttons: "west" (the US game's) or "jp"
    std::string language = "en";   // game language when both are set up: "en" or "ja"
    std::string ui_font;        // empty: first available system font
    AnalogOptions analog;       // the sticks, saved as "analog"
    int volume = 100;           // percent
    int resolution = 0;         // 0: native (match the window), else whole multiple of 320x240
    int ao_quality = 1;         // 0 low, 1 medium, 2 high (OcclusionQuality)
    int texture_filter = 3;     // 0 nearest (original), 1 bilinear, 2 three-point, 3 sharp
    int anisotropy = 16;        // 1 (off), 2, 4, 8 or 16 samples along slanted textures
    int enhanced_distance = 0;  // cells past which the picture is the game's own; 0: everywhere
    int fog = 2;                // 0 at the corners (original), 1 per pixel by depth, 2 by distance (FogMode)
    int antialiasing = 1;       // 0 off, 1 FXAA, 2 4x SSAA, 3 9x SSAA
    int frame_rate = 0;         // 20: original (no smoothing), 0: display refresh, -1: unlimited, else cap
    int tick_rate = 15;         // world updates a second: 12, 15, 20 (the gate's) or 30
    int look_limit = 70;        // degrees the camera may tilt up or down (original: 17)
    float enemy_turn = 1.5f;        // fixed: an enemy's turn toward the player, times its own (1: original)
    float ui_scale = 1.0f;
    int setup_seen = 0;             // the first-run setup's version last finished or skipped
    int name_bars = 2;              // above the other players: 0 off, 1 in sight, 2 through walls too (OnlineNameBars)
    int revive_wait_s = 30;         // hosting: the most a down player waits to come back (10, 30 or 60)
    // Classic mode: play as originally shipped. Every enhancement below is
    // overridden (see effective_settings) while the player's values are kept.
    bool classic = false;
    bool fullscreen = default_fullscreen;
    bool vsync = false;
    bool pause_when_open = false;
    bool show_fps = false;
    bool hud_at_edges = true;   // in widescreen, the HUD at the picture's edges, not in its 4:3 box
    bool subpixel = true;       // smooth vertex positions (less polygon jitter)
    bool perspective = true;    // perspective-correct textures (no warping)
    bool depth_buffer = false;  // experimental: fixes polygons showing through walls
    bool true_color = true;     // smooth colour: no 15-bit banding or dither
    bool pixel_lighting = true; // the game's lights per pixel on models
    bool dynamic_lights = true; // torches and fire/lightning spells light nearby surfaces
    bool ambient_occlusion = true;
    bool mipmaps = true;        // distant textures averaged instead of shimmering
    bool detailed_map = true;   // distant map cells keep their detailed meshes
    bool flame_glow = true;
    bool shadows = true;
    bool touch_haptics = true;  // the on-screen buttons click the phone's vibrator
    bool auto_keys = true;          // interacting with a lock uses a carried key
    bool reload_on_death = true;    // a death reloads the last save, not a New Game
    bool enemy_turn_auto = true;    // the enemy turn follows how fast the last input to turn can turn
    bool fix_ceiling_fall = true;   // bug fix: enemies fall after hitting a ceiling mid-jump
    bool fix_spatial_panning = true; // bug fix: flagged sounds get their panning adjustment
    bool item_descriptions = true;  // what an item does, in the game's item menus
    bool update_check = true;   // asks GitHub for a new release at launch, at most once a day
    bool discord = true;        // Discord Rich Presence, while Discord is running
    bool discord_level = true;
    bool discord_location = true;   // floor and place
    bool discord_vitals = true;     // HP and MP
    bool discord_exp = true;        // EXP to the next level
    bool online_messages = false;   // messages on the ground: opt-in alone, always on in a coop room
    bool pickup_feed = true;        // in coop, who in the party found, bought, sold or used what
    bool room_open = true;          // hosting: players new to the world may join
};
} // namespace kf::ext
