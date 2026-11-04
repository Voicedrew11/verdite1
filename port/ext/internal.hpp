#pragma once
#include <kf/ext/ext.h>
#include <kf/platform/analog.h>
#include <kf/platform/language.h>
#include <verdite/setup.h>
#include "coop_profile.h"
#include "online_pose.h"
#include "setting_rows.h"
#include <kf/platform/saves.h>
#include <verdite/mod_saved.h>
#include <verdite/mods.h>
#include <verdite/options.h>
#include <verdite/pickup_feed.h>
#include <verdite/title_menu.h>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
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
void options_rebuild();   // the table again, for rows that changed: invalidates every view of it
verdite::PlatformFeatures platform_features();   // what this build's platform has

// ---- the SETTINGS page in the game's menu (kf/platform/settings_page.h) ----
bool settings_page_holding();   // it has changes not yet saved: the settings file waits
void settings_page_abandon();   // puts its changes back, as quitting with it open does
void options_poll();   // on every host poll: a file the Language page asked for
// The render target apply_render_settings last set: logical columns and scale.
struct RenderTarget { int columns, scale; };
RenderTarget applied_render_target();

// What an item does (effects only; the menus show equipment stats), or empty.
std::string item_description(std::uint8_t id);
// Draws the description of the item in the game menu's preview panel, if any.
void item_description_draw();

// ---- the first-run setup (setup.cpp) ----
void setup_begin(Overlay overlay); // at start: shown if due and drawn
void settings_page_check();         // at start: the page's words against the menu font and its boxes
void setup_run();                  // from the start, again
bool setup_open();
bool setup_shows_game();           // open, and the game is seen behind it
void setup_poll();                 // on every host poll: a native panel's controls in
void setup_draw();                 // inside the ImGui frame (setup_json, setup_command: ext.h)

// ---- Discord Rich Presence ----
void discord_start();     // reads KF_DISCORD and KF_DISCORD_APP
void discord_poll();      // on every host poll: composes the presence and hands it over
void discord_stop();      // takes the presence down
bool discord_forced();    // KF_DISCORD decides for this run
bool discord_enabled();   // the presence is on, by the setting or KF_DISCORD

// ---- telling the player about a new release (update_notice.cpp) ----
void update_notice_start();   // asks GitHub on its own thread, in a build for players or by KF_UPDATE_CHECK=force
void update_notice_poll();    // on every host poll: shows the notice once, at the title
bool update_notice_open();    // its window covers the game
void update_notice_draw();    // inside the ImGui frame
void update_notice_draw_badge();           // inside the menu bar
float update_notice_badge_width_px();      // 0 when there is no badge
std::string update_notice_setting_note();  // what the setting's row says; empty when there is no release

// ---- messages on the ground (messages_board.cpp, docs/MESSAGES_PLAN.md) ----
void messages_poll();            // on every host poll: the floor's board, entered and left
// The message stone (extra_items.cpp's row): offered while a floor is entered, and its menu, which
// leaves a message on the player's cell and lists their own.
bool messages_stone_offered();
std::unique_ptr<class ItemMenu> messages_stone_open();
// The Verdite service the online play uses (online.cpp), for the messages' board.
std::string online_service_url();

// ---- online play (online.cpp, on online/client) ----
// Opt-in: nothing touches the network until a room is joined, from the overlay's Online window
// or --online-room (online_join_at_start). The service is the deployed one, or KF_ONLINE_SERVICE
// (a local wrangler dev). Peers must have the same build (KF_VERSION and KF_BUILD_COMMIT).
void online_start();   // joins the room given on the command line, if any
void online_poll();    // on every host poll: the session's events (our pose goes out from online_update)
void online_stop();    // leaves the room
void online_draw_panel();   // the Online window: room code, Join/Leave, who is here
#ifdef __ANDROID__
// Points OpenSSL at the phone's root certificates (online_android_roots.cpp), for the service's wss://.
void online_trust_system_roots();
#endif
// Whether this platform may be a room's first, its host: not Android (docs/COOP_PLAN.md, "Fixed").
// KF_TEST_NO_HOSTING=1 refuses it anywhere.
bool online_may_host();
// Leaves the room as the app goes to the background (docs/COOP_PLAN.md, "Fixed").
void online_went_to_background();
// The other players in the room who are up, as last heard from, for the automap.
std::vector<OnlinePose> online_players();
// Every other player in the room as last played back, down or up, by peer.
std::map<std::uint64_t, OnlinePose> online_room_poses();
// A join asked for by the title menu: as the room's first (hosting) or after someone (joining).
// One that finds the room the other way round leaves at once. A listed_as lists the room under
// that host name (a public code only).
enum class RoomRole : std::uint8_t { First, Later };
void online_join_from_title(std::string_view room, RoomRole expected, std::string_view listed_as);
// NeedsEnglish: coop draws players' names with the English font in either language, and the
// English game files were missing; they are being added (language_task_state).
enum class OnlineFailure : std::uint8_t { None, NoHost, InUse, Full, OtherBuild, Unreachable, NoOnline, NeedsEnglish, RoomClosed, SentAway, Other };
struct OnlineStatus {
    bool joining = false;   // asked, not yet in the room
    bool in_room = false;
    OnlineFailure failure = OnlineFailure::None;   // why the last session ended or never began
};
OnlineStatus online_status();
// The list of public rooms of this build, open while the title shows it. online_rooms copies the
// latest into rooms and says whether it changed since the last call.
void online_list_open();
void online_list_close();
bool online_rooms(std::vector<verdite::TitleRoom> &rooms, verdite::RoomListState &state);
// CONTROL packets for coop: to one peer, or every open peer.
void online_send_control(std::uint64_t peer, std::span<const std::uint8_t> bytes);
void online_broadcast_control(std::span<const std::uint8_t> bytes);
// To every peer on the STATE channel (unreliable): a floor owner's actors.
void online_broadcast_state(std::span<const std::uint8_t> bytes);
// This machine's peer in the room; 0 outside one.
std::uint64_t online_self_peer();
// How many name bars the game has drawn, for the agent channel's tests.
std::uint64_t online_name_bars_drawn();
// Leaves the room once the session's events are handled, saying why.
void online_leave_room(std::string_view why);
// The same, for a host that turned this player away, so the title can say which way.
void online_turned_away(std::string_view why, OnlineFailure failure);

// ---- coop (coop.cpp): the coop world played in a room (docs/COOP_PLAN.md) ----
// What online.cpp hears of the room.
// host_peer: the room's first peer, whose welcome alone a guest takes; 0 when this machine is first
// and hosts.
void coop_room_joined(std::string_view room, std::uint64_t host_peer);
void coop_peer_open(std::uint64_t peer);
void coop_peer_closed(std::uint64_t peer);
void coop_control_packet(std::uint64_t peer, std::span<const std::uint8_t> bytes);
void coop_room_left();   // left, dropped or failed: this player's own game comes back
// The pickup feed's lines now, oldest first, and a line's player's name.
std::vector<verdite::PickupShown> coop_feed_lines();
std::string coop_player_name(std::uint32_t player);
// A player's name by its id (CoopPlayerId).
std::string coop_player_name_of(std::uint64_t player);
// The title's coop start: once the room is joined, whether the world can go in (the host at once,
// a guest once the host welcomed it); then the world goes in at the first point between updates
// without setting a game aside, and leaving it goes back to the title.
bool coop_ready_from_title();
void coop_start_from_title();
// A game a crashed run left set aside stays in its file this run (the title loaded a slot).
void coop_keep_leftover_aside();
// This player's profile (coop_profile.h), kept in the user folder; a new one is kept at once and
// goes to the others from the next room.
CoopProfile coop_own_profile();
void coop_set_own_profile(const CoopProfile &profile);
// The profile of the player on `peer`, as the room's roster has it.
CoopProfile coop_profile_of_peer(std::uint64_t peer);
// ---- the title menu (title.cpp) ----
// A coop session started from the title ended: back to the title at the next point between updates.
void title_return();
// A line for the Online window: whose world this is, or empty outside coop.
std::string coop_panel_line();
// ---- the room's settings (coop.cpp), which the host chooses (docs/COOP_PLAN.md, "Multiplayer settings") ----
inline constexpr std::size_t coop_guest_slots = 3;   // the service's room holds 4 players
// Whether this machine hosts the room it is playing in, or is a guest in one.
bool coop_hosting();
bool coop_follows_host();
// A setting the room follows changed: a host sends it to the others.
void coop_room_settings_changed();
// The host's tick rate while this machine is a guest told it; else nothing, and its own applies.
std::optional<int> coop_room_tick_rate();
// The most a down player waits to come back: the host's, told or its own.
int coop_revive_wait_s();
// A guest of this room by its slot, which it keeps while it stays: its name, empty when there is none.
std::string coop_guest_name(std::size_t slot);
// Host: sends that guest out of the room, and refuses them for the rest of this session.
void coop_send_away(std::size_t slot);
// ---- the multiplayer settings, for the title's pages (settings_page.cpp) ----
enum class TitleCoopRows : std::uint8_t { Own, Room };
std::vector<verdite::TitleModSetting> title_coop_settings(TitleCoopRows rows, Language language);
void title_coop_setting_step(TitleCoopRows rows, std::size_t row, int delta);

// ---- automap ----
// The English name of an item as the map labels it, or nullptr for an id with none.
const char *item_english_name(std::uint8_t id);

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
#ifdef __ANDROID__
void android_call_activity(const char *method);   // a no-argument method of VerditeActivity
#endif

// ---- Android's setup panel (setup_panel.cpp; null elsewhere) ----
verdite::SetupScreen *setup_panel_screen();
std::vector<std::string> setup_panel_take_controls();   // the panel's taps, as Setup::command lines

// ---- mods ----
void mods_start();
void mods_shutdown();
void mods_poll();                 // hot reload
void mods_frame(int input_context);
void mods_draw_hud();             // each enabled mod's on_draw, inside the ImGui frame
void mods_draw_panel();           // the Mods window contents
int mods_loaded_count();
bool mods_pause_requested();      // kf.game.paused, combined over the running mods
bool mods_pause_keeps_music();    // ...and every pausing mod keeps the music (kf.game.music_while_paused)
bool mods_take_mouse();           // kf.game.mouse: the cursor shows and the game gets no mouse input
// A mod by id, as the menus switch it, and its config. Nothing changes while mods are off in coop.
void mods_set_enabled(std::string_view id, bool enabled);
std::vector<verdite::ModInfo> mods_list();   // the installed mods, for the title menu's MODS page
// What a save's list of mods says now: how many it names, and how they differ from those running.
// Nothing when the save does not say (a save from before the list).
struct SaveModInfo {
    std::size_t mods = 0;
    std::vector<verdite::SavedModDifference> differences;
};
SaveModInfo mods_save_info(SaveSlot slot);
std::optional<verdite::ModConfigValue> mods_config_value(std::string_view id, std::string_view key);
void mods_set_config_value(std::string_view id, std::string_view key, const verdite::ModConfigValue &value);

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
