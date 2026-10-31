// KF1's side of the Lua mods (verdite/mods.h, docs/MODDING.md): the kf, kf.player and ui tables,
// the Mods panel, and the mods' settings in settings.json. A new field or function is a row in
// api_tables; the host writes the editors' definitions file (mods/kf.d.lua) from the same rows.
#include "internal.hpp"
#include <cJSON.h>
#include <imgui.h>
#include <verdite/mods.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <cstdio>
#include <memory>
#include <type_traits>
#include <utility>
#include <kf/platform/prelude.h>
#include <kf/game/player.h>
#include <kf/game/player_motion.h>
#include <kf/lib/floor.h>
#include <kf/lib/map.h>
#include <kf/lib/map_data.h>
#include <kf/lib/math.h>
#include <kf/platform/fuzz.h>
#include <kf/platform/host.h>
#include <kf/platform/play_overrides.h>
#include "mod_play.h"
#include <lauxlib.h>
#include <lua.h>
#include <map>
#include <set>

namespace kf::ext {
namespace {
constexpr const char *api_version = "0.3";

std::unique_ptr<verdite::ModHost> host;
// Each running mod's kf.play and kf.game values, by id, and what they combine to.
std::map<std::string, ModPlay, std::less<>> mod_plays;
ModPlayCombined combined_play;
bool in_imgui_frame;             // ui.* is only valid while drawing
Uint64 last_scan;
std::uint64_t frame_counter;
int input_context;
std::array<bool, SDL_SCANCODE_COUNT> keys_now{}, keys_before{};
constexpr std::array<const char *, 4> context_names{"opening", "gameplay", "menu", "scripted"};

// ---------- kf.player ----------
template <typename T> auto as_integer(T value) {
    if constexpr (std::is_enum_v<T>)
        return static_cast<std::underlying_type_t<T>>(value);
    else
        return value;
}

template <auto field> void push_integer(lua_State *L) {
    lua_pushinteger(L, static_cast<lua_Integer>(as_integer(field())));
}

// Writes go straight into player_state, checked against the field's range.
template <auto field> void write_integer(lua_State *L, int index) {
    auto &target = field();
    using Target = std::remove_reference_t<decltype(target)>;
    using Stored = decltype(as_integer(target));
    const lua_Integer value = luaL_checkinteger(L, index);
    if (!std::in_range<Stored>(value))
        luaL_error(L, "%I is out of range (%I to %I)", value,
                   static_cast<lua_Integer>(std::numeric_limits<Stored>::min()),
                   static_cast<lua_Integer>(std::numeric_limits<Stored>::max()));
    target = static_cast<Target>(static_cast<Stored>(value));
}

template <auto field> verdite::ModField integer_field(std::string_view name, std::string_view doc = {}) {
    return {name, "integer", doc, push_integer<field>, write_integer<field>};
}

template <auto field> verdite::ModField read_only_integer(std::string_view name, std::string_view doc) {
    return {name, "integer", doc, push_integer<field>, nullptr};
}

void push_floor(lua_State *L) {
    lua_pushinteger(L, kf_enum_encode<lua_Integer>(player_state.progress_state.current_floor));
}
void push_highest_floor(lua_State *L) {
    lua_pushinteger(L, kf_enum_encode<lua_Integer>(player_state.progress_state.highest_floor));
}
void push_map_variant(lua_State *L) {
    lua_pushinteger(L, kf_enum_encode<lua_Integer>(player_state.map_variant));
}

// The yaw is any integer, stored in one turn (4096 units) as the game keeps it.
void write_yaw(lua_State *L, int index) {
    const lua_Integer yaw = luaL_checkinteger(L, index);
    player_state.camera_rotation.vy = static_cast<s16>(yaw & KF_ANGLE_WRAP_MASK);
}

void write_pitch(lua_State *L, int index) {
    const lua_Integer pitch = luaL_checkinteger(L, index);
    if (pitch < -KF_PLAYER_CAMERA_PITCH_LIMIT || pitch > KF_PLAYER_CAMERA_PITCH_LIMIT)
        luaL_error(L, "pitch %I is out of range (-%d to %d)", pitch, KF_PLAYER_CAMERA_PITCH_LIMIT,
                   KF_PLAYER_CAMERA_PITCH_LIMIT);
    player_state.camera_rotation.vx = static_cast<s16>(pitch);
}

const std::vector<verdite::ModField> &player_fields() {
    static const std::vector<verdite::ModField> fields = {
        read_only_integer<[]() -> auto & { return player_state.camera_position.vx; }>(
            "x", "World units, 2000 to a cell."),
        read_only_integer<[]() -> auto & { return player_state.camera_position.vy; }>(
            "y", "World units; the eye height."),
        read_only_integer<[]() -> auto & { return player_state.camera_position.vz; }>(
            "z", "World units, 2000 to a cell."),
        {"yaw", "integer", "Facing, 0 to 4095; a write wraps.",
         push_integer<[]() -> auto & { return player_state.camera_rotation.vy; }>, write_yaw},
        {"pitch", "integer", "Looking up or down, -191 to 191.",
         push_integer<[]() -> auto & { return player_state.camera_rotation.vx; }>, write_pitch},
        read_only_integer<[]() -> auto & { return player_state.motion_state.map_cell.x; }>(
            "cell_x", "The cell column the player is in."),
        read_only_integer<[]() -> auto & { return player_state.motion_state.map_cell.z; }>(
            "cell_z", "The cell row the player is in."),
        {"map_variant", "integer", "The floor's variant (0 default, 1 floor 5's entry).", push_map_variant, nullptr},
        {"highest_floor", "integer", "The deepest floor reached.", push_highest_floor, nullptr},
        integer_field<[]() -> auto & { return player_state.vitals.current_hp; }>("hp"),
        integer_field<[]() -> auto & { return player_state.vitals.maximum_hp; }>("max_hp"),
        integer_field<[]() -> auto & { return player_state.vitals.current_mp; }>("mp"),
        integer_field<[]() -> auto & { return player_state.vitals.maximum_mp; }>("max_mp"),
        integer_field<[]() -> auto & { return player_state.gold; }>("gold"),
        integer_field<[]() -> auto & { return player_state.experience; }>("experience"),
        integer_field<[]() -> auto & { return player_state.next_level_experience; }>("next_level_experience"),
        integer_field<[]() -> auto & { return player_state.progress_state.level; }>("level"),
        integer_field<[]() -> auto & { return player_state.physical_power; }>("physical_power"),
        integer_field<[]() -> auto & { return player_state.magic; }>("magic"),
        integer_field<[]() -> auto & { return player_state.base_physical_power; }>("base_physical_power"),
        integer_field<[]() -> auto & { return player_state.base_magic; }>("base_magic"),
        integer_field<[]() -> auto & { return player_state.physical_power_training; }>("physical_power_training"),
        integer_field<[]() -> auto & { return player_state.magic_training; }>("magic_training"),
        integer_field<[]() -> auto & { return player_state.magic_charge; }>("magic_charge"),
        integer_field<[]() -> auto & { return player_state.status_effect_flags; }>(
            "status_flags", "Bits: 1 curse, 2 darkness, 4 poison, 8 slowed, 16 fire defense boost."),
        integer_field<[]() -> auto & { return player_state.cutting_attack; }>("cutting_attack"),
        integer_field<[]() -> auto & { return player_state.striking_attack; }>("striking_attack"),
        integer_field<[]() -> auto & { return player_state.piercing_attack; }>("piercing_attack"),
        integer_field<[]() -> auto & { return player_state.holy_attack; }>("holy_attack"),
        integer_field<[]() -> auto & { return player_state.fire_attack; }>("fire_attack"),
        integer_field<[]() -> auto & { return player_state.cutting_defense; }>("cutting_defense"),
        integer_field<[]() -> auto & { return player_state.striking_defense; }>("striking_defense"),
        integer_field<[]() -> auto & { return player_state.piercing_defense; }>("piercing_defense"),
        integer_field<[]() -> auto & { return player_state.poison_resistance; }>("poison_resistance"),
        integer_field<[]() -> auto & { return player_state.magic_defense; }>("magic_defense"),
        integer_field<[]() -> auto & { return player_state.fire_defense; }>("fire_defense"),
        integer_field<[]() -> auto & { return player_state.curse_timer; }>("curse_timer"),
        integer_field<[]() -> auto & { return player_state.darkness_timer; }>("darkness_timer"),
        integer_field<[]() -> auto & { return player_state.poison_timer; }>("poison_timer"),
        integer_field<[]() -> auto & { return player_state.slowed_timer; }>("slowed_timer"),
        {"floor", "integer", "The floor the player is on.", push_floor, nullptr},
    };
    return fields;
}

// ---------- kf.warp: queued, run between updates ----------
struct WarpRequest {
    KfFloorId floor = KF_FLOOR_1;
    std::optional<std::array<int, 2>> cell;   // x, z
    std::optional<int> yaw;
};
std::optional<WarpRequest> warp_request;   // the last call wins

KfFloorId check_floor(lua_State *L, int index) {
    const lua_Integer floor = luaL_checkinteger(L, index);
    if (floor < 1 || floor > 5)
        luaL_error(L, "floor %I is not 1 to 5", floor);
    return kf_enum_decode<KfFloorId>(static_cast<int>(floor));
}

// A cell of the map, from the Lua values at index and index + 1.
std::array<int, 2> check_map_cell(lua_State *L, int index) {
    const lua_Integer x = luaL_checkinteger(L, index);
    const lua_Integer z = luaL_checkinteger(L, index + 1);
    if (x < 0 || x >= KF_MAP_COLUMNS || z < 0 || z >= KF_MAP_ROWS)
        luaL_error(L, "cell %I, %I is off the map (0 to %d)", x, z, KF_MAP_COLUMNS - 1);
    return {static_cast<int>(x), static_cast<int>(z)};
}

// Off the map counts as blocked.
bool cell_is_blocked(int x, int z) {
    if (x < 0 || x >= KF_MAP_COLUMNS || z < 0 || z >= KF_MAP_ROWS)
        return true;
    return map_collision_grid.cells[z][x] == KF_MAP_CELL_BLOCKED;
}

// The cell and yaw of a warp are both optional; a cell is both coordinates or neither.
std::optional<std::array<int, 2>> optional_map_cell(lua_State *L, int index) {
    if (lua_isnoneornil(L, index) && lua_isnoneornil(L, index + 1))
        return std::nullopt;
    return check_map_cell(L, index);
}

int kf_warp(lua_State *L) {
    WarpRequest request;
    request.floor = check_floor(L, 1);
    request.cell = optional_map_cell(L, 2);
    if (!lua_isnoneornil(L, 4))
        request.yaw = static_cast<int>(luaL_checkinteger(L, 4) & KF_ANGLE_WRAP_MASK);
    const bool same_floor = request.floor == player_state.progress_state.current_floor;
    if (same_floor && request.cell && cell_is_blocked((*request.cell)[0], (*request.cell)[1]))
        luaL_error(L, "cell %d, %d is blocked", (*request.cell)[0], (*request.cell)[1]);
    warp_request = request;
    return 0;
}

// Another floor's grid is loaded by the warp, so its cell is checked after it.
std::optional<std::array<int, 2>> landing_cell(const WarpRequest &request) {
    if (!request.cell)
        return first_open_cell();
    const auto [x, z] = *request.cell;
    if (!cell_is_blocked(x, z))
        return request.cell;
    std::printf("[mods] warp: cell %d, %d is blocked on floor %d; moving to the first open cell\n", x, z,
                kf_enum_encode<int>(request.floor));
    return first_open_cell();
}

void apply_warp(const WarpRequest &request) {
    if (request.floor != player_state.progress_state.current_floor)
        player_warp_change_floor(request.floor, request.floor == KF_FLOOR_5 ? KF_FLOOR5_ENTRY_VARIANT : KF_MAP_VARIANT_DEFAULT);
    if (const auto cell = landing_cell(request))
        player_move_to_cell((*cell)[0], (*cell)[1]);
    if (request.yaw)
        player_state.camera_rotation.vy = static_cast<s16>(*request.yaw);
}

// ---------- kf.world ----------
void push_world_map_variant(lua_State *L) {
    lua_pushinteger(L, kf_enum_encode<lua_Integer>(player_state.map_variant));
}

int world_cell_blocked(lua_State *L) {
    const auto [x, z] = check_map_cell(L, 1);
    lua_pushboolean(L, cell_is_blocked(x, z));
    return 1;
}

// A story flag, read and written as a boolean.
template <auto flag> void push_script_flag(lua_State *L) {
    lua_pushboolean(L, flag() == KF_MAP_SCRIPT_SET);
}
template <auto flag> void write_script_flag(lua_State *L, int index) {
    luaL_checktype(L, index, LUA_TBOOLEAN);
    flag() = lua_toboolean(L, index) ? KF_MAP_SCRIPT_SET : KF_MAP_SCRIPT_UNSET;
}
template <auto flag> verdite::ModField script_flag(std::string_view name) {
    return {name, "boolean", "", push_script_flag<flag>, write_script_flag<flag>};
}

void push_revealed_pieces(lua_State *L) {
    lua_pushinteger(L, map_runtime_state.world_state.floor3.revealed_piece_count);
}
void write_revealed_pieces(lua_State *L, int index) {
    const lua_Integer count = luaL_checkinteger(L, index);
    if (count < 0 || count > KF_MAP_FLOOR3_REQUIRED_REVEALS)
        luaL_error(L, "floor3_revealed_pieces %I is out of range (0 to %d)", count, KF_MAP_FLOOR3_REQUIRED_REVEALS);
    map_runtime_state.world_state.floor3.revealed_piece_count = static_cast<u8>(count);
}

const std::vector<verdite::ModField> &world_fields() {
    static const std::vector<verdite::ModField> fields = {
        {"floor", "integer", "The floor the player is on.", push_floor, nullptr},
        {"map_variant", "integer", "The floor's variant.", push_world_map_variant, nullptr},
        script_flag<[]() -> auto & { return map_runtime_state.world_state.floor1.passage_opened; }>(
            "floor1_passage_opened"),
        script_flag<[]() -> auto & { return map_runtime_state.world_state.floor1.revival_enabled; }>(
            "floor1_revival_enabled"),
        script_flag<[]() -> auto & { return map_runtime_state.world_state.floor5.character_arrived; }>(
            "floor5_character_arrived"),
        script_flag<[]() -> auto & { return map_runtime_state.world_state.floor5.weapon_transformed; }>(
            "floor5_weapon_transformed"),
        script_flag<[]() -> auto & { return map_runtime_state.world_state.floor5.boss_encounter_started; }>(
            "floor5_boss_encounter_started"),
        script_flag<[]() -> auto & { return map_runtime_state.world_state.floor5.boss_defeat; }>(
            "floor5_boss_defeated"),
        {"floor3_revealed_pieces", "integer", "Pieces revealed on floor 3, 0 to 4.", push_revealed_pieces,
         write_revealed_pieces},
    };
    return fields;
}

// ---------- kf.play and kf.game: the calling mod's own values ----------
const ModPlay &play_of(lua_State *L) {
    static const ModPlay defaults;
    const auto found = mod_plays.find(verdite::mod_id(L));
    return found == mod_plays.end() ? defaults : found->second;
}
ModPlay &play_for_write(lua_State *L) {
    return mod_plays[std::string(verdite::mod_id(L))];
}

template <bool ModPlay::*member> void push_switch(lua_State *L) {
    lua_pushboolean(L, play_of(L).*member);
}
template <bool ModPlay::*member> void write_switch(lua_State *L, int index) {
    luaL_checktype(L, index, LUA_TBOOLEAN);
    play_for_write(L).*member = lua_toboolean(L, index) != 0;
}
template <float ModPlay::*member> void push_speed(lua_State *L) {
    lua_pushnumber(L, static_cast<lua_Number>(play_of(L).*member));
}
template <float ModPlay::*member> void write_speed(lua_State *L, int index) {
    const auto speed = static_cast<float>(luaL_checknumber(L, index));
    if (!std::isfinite(speed) || speed < min_mod_speed || speed > max_mod_speed)
        luaL_error(L, "speed %f is outside %f to %f", static_cast<double>(speed),
                   static_cast<double>(min_mod_speed), static_cast<double>(max_mod_speed));
    play_for_write(L).*member = speed;
}

template <bool ModPlay::*member> verdite::ModField switch_field(std::string_view name, std::string_view doc) {
    return {name, "boolean", doc, push_switch<member>, write_switch<member>};
}
template <float ModPlay::*member> verdite::ModField speed_field(std::string_view name, std::string_view doc) {
    return {name, "number", doc, push_speed<member>, write_speed<member>};
}

const std::vector<verdite::ModField> &play_fields() {
    static const std::vector<verdite::ModField> fields = {
        switch_field<&ModPlay::noclip>("noclip", "Walls do not stop the player; height still follows the floor."),
        switch_field<&ModPlay::invincible>("invincible", "HP losses are ignored, so no death."),
        switch_field<&ModPlay::peaceful>("peaceful", "Enemies never attack the player."),
        speed_field<&ModPlay::move_speed>("move_speed", "Walking distance, 0.25 to 8."),
        speed_field<&ModPlay::turn_speed>("turn_speed", "Turning speed, 0.25 to 8."),
    };
    return fields;
}

const std::vector<verdite::ModField> &game_fields() {
    static const std::vector<verdite::ModField> fields = {
        speed_field<&ModPlay::game_speed>("speed", "Game time at this multiple of real time, 0.25 to 8."),
        switch_field<&ModPlay::paused>("paused", "The game stops; on_update does not run, so unpause from on_draw."),
    };
    return fields;
}

// Recomputes what the game sees from the running mods' values; a mod that is not running any more
// (unloaded, failed) loses its own.
void refresh_mod_play() {
    std::set<std::string, std::less<>> running;
    if (host)
        for (const verdite::ModInfo &mod : host->mods())
            if (mod.state == verdite::ModState::Running)
                running.insert(mod.id);
    std::erase_if(mod_plays, [&running](const auto &entry) { return !running.contains(entry.first); });
    std::vector<ModPlay> values;
    values.reserve(mod_plays.size());
    for (const auto &entry : mod_plays)
        values.push_back(entry.second);
    combined_play = combine_mod_play(values);
    host_set_game_speed(combined_play.game_speed);
}

// The host discarded a mod's Lua state (reload, disable, failure, or the host's own destruction).
// host is already null while ~ModHost runs, so refresh_mod_play does not call into it then.
void forget_mod_play(std::string_view id) {
    if (const auto found = mod_plays.find(id); found != mod_plays.end())
        mod_plays.erase(found);
    refresh_mod_play();
}

// ---------- kf.* ----------
int kf_log(lua_State *L) {
    std::string text;
    const int count = lua_gettop(L);
    for (int i = 1; i <= count; ++i) {
        if (i > 1)
            text += '\t';
        text += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    verdite::mod_log(L, text);
    return 0;
}
int kf_frame(lua_State *L) { lua_pushinteger(L, static_cast<lua_Integer>(frame_counter)); return 1; }
int kf_time(lua_State *L) { lua_pushnumber(L, static_cast<double>(SDL_GetTicks()) / 1000.0); return 1; }
int kf_context(lua_State *L) {
    lua_pushstring(L, context_names[static_cast<std::size_t>(std::clamp(input_context, 0, 3))]);
    return 1;
}
SDL_Scancode check_key(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    const SDL_Scancode code = SDL_GetScancodeFromName(name);
    if (code == SDL_SCANCODE_UNKNOWN)
        luaL_error(L, "unknown key '%s' (use SDL key names such as \"F5\", \"G\", \"Left Shift\")", name);
    return code;
}
int kf_key_down(lua_State *L) {
    const auto code = check_key(L);
    lua_pushboolean(L, !overlay_captures_input() && keys_now[code]);
    return 1;
}
int kf_key_pressed(lua_State *L) {
    const auto code = check_key(L);
    lua_pushboolean(L, !overlay_captures_input() && keys_now[code] && !keys_before[code]);
    return 1;
}
int kf_player_fields(lua_State *L) {
    const auto &fields = player_fields();
    lua_createtable(L, static_cast<int>(fields.size()), 0);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        lua_pushlstring(L, fields[i].name.data(), fields[i].name.size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    return 1;
}
int push_view(lua_State *L, const ViewRect &view) {
    lua_pushnumber(L, view.x); lua_pushnumber(L, view.y);
    lua_pushnumber(L, view.w); lua_pushnumber(L, view.h);
    return 4;
}
int kf_view(lua_State *L) { return push_view(L, game_view()); }
int kf_view_original(lua_State *L) { return push_view(L, original_view()); }
int kf_save_config(lua_State *L) {
    verdite::mod_config_changed(L);
    return 0;
}
void push_api_version(lua_State *L) { lua_pushstring(L, api_version); }

// ---------- ui.* (ImGui) ----------
void require_frame(lua_State *L) {
    if (!in_imgui_frame)
        luaL_error(L, "ui.* functions only work inside on_draw or on_settings");
}
ImU32 check_color(lua_State *L, int first, ImU32 fallback) {
    if (lua_isnoneornil(L, first))
        return fallback;
    const auto channel = [&](int i, double otherwise) {
        return static_cast<float>(luaL_optnumber(L, first + i, otherwise));
    };
    return ImGui::GetColorU32(ImVec4(channel(0, 1), channel(1, 1), channel(2, 1), channel(3, 1)));
}
void mark_changed(lua_State *L, bool changed) {
    if (changed)
        verdite::mod_config_changed(L);
}
int ui_text(lua_State *L) { require_frame(L); ImGui::TextUnformatted(luaL_checkstring(L, 1)); return 0; }
int ui_text_colored(lua_State *L) {
    require_frame(L);
    const ImVec4 color(static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)),
                       static_cast<float>(luaL_checknumber(L, 3)), 1.0f);
    ImGui::TextColored(color, "%s", luaL_checkstring(L, 4));
    return 0;
}
int ui_button(lua_State *L) { require_frame(L); lua_pushboolean(L, ImGui::Button(luaL_checkstring(L, 1))); return 1; }
int ui_checkbox(lua_State *L) {
    require_frame(L);
    bool value = lua_toboolean(L, 2);
    mark_changed(L, ImGui::Checkbox(luaL_checkstring(L, 1), &value));
    lua_pushboolean(L, value);
    return 1;
}
int ui_slider_int(lua_State *L) {
    require_frame(L);
    int value = static_cast<int>(luaL_checkinteger(L, 2));
    mark_changed(L, slider_input(luaL_checkstring(L, 1), &value, static_cast<int>(luaL_checkinteger(L, 3)),
                                 static_cast<int>(luaL_checkinteger(L, 4))));
    lua_pushinteger(L, value);
    return 1;
}
int ui_slider_float(lua_State *L) {
    require_frame(L);
    float value = static_cast<float>(luaL_checknumber(L, 2));
    mark_changed(L, slider_input(luaL_checkstring(L, 1), &value, static_cast<float>(luaL_checknumber(L, 3)),
                                 static_cast<float>(luaL_checknumber(L, 4)), luaL_optstring(L, 5, "%.2f")));
    lua_pushnumber(L, value);
    return 1;
}
int ui_combo(lua_State *L) {
    require_frame(L);
    const char *label = luaL_checkstring(L, 1);
    int index = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<std::string> items;
    for (lua_Integer i = 1; lua_rawgeti(L, 3, i) == LUA_TSTRING; ++i) {
        items.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    const char *preview = index >= 0 && index < static_cast<int>(items.size()) ? items[index].c_str() : "";
    if (ImGui::BeginCombo(label, preview)) {
        for (int i = 0; i < static_cast<int>(items.size()); ++i)
            if (ImGui::Selectable(items[i].c_str(), i == index)) {
                index = i;
                mark_changed(L, true);
            }
        ImGui::EndCombo();
    }
    lua_pushinteger(L, index + 1);
    return 1;
}
int ui_separator(lua_State *L) { require_frame(L); ImGui::Separator(); return 0; }
int ui_same_line(lua_State *L) { require_frame(L); ImGui::SameLine(); return 0; }
int ui_spacing(lua_State *L) { require_frame(L); ImGui::Spacing(); return 0; }
int ui_tooltip(lua_State *L) { require_frame(L); ImGui::SetItemTooltip("%s", luaL_checkstring(L, 1)); return 0; }
int ui_begin_window(lua_State *L) {
    require_frame(L);
    const bool no_input = lua_toboolean(L, 2);
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;
    if (no_input && !overlay_open())
        flags |= ImGuiWindowFlags_NoInputs;
    lua_pushboolean(L, ImGui::Begin(luaL_checkstring(L, 1), nullptr, flags));
    return 1;
}
int ui_end_window(lua_State *L) { require_frame(L); ImGui::End(); return 0; }
int ui_draw_text(lua_State *L) {
    require_frame(L);
    const ImVec2 at(static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)));
    const char *text = luaL_checkstring(L, 3);
    const ImU32 color = check_color(L, 4, IM_COL32_WHITE);
    // An explicit pixel size keeps HUD text independent of the interface scale.
    const float size = static_cast<float>(luaL_optnumber(L, 8, hud_font_size));
    ImFont *font = ImGui::GetFont();
    ImDrawList *list = ImGui::GetForegroundDrawList();
    list->AddText(font, size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), text);
    list->AddText(font, size, at, color, text);
    return 0;
}
int ui_draw_rect(lua_State *L) {
    require_frame(L);
    const float x = static_cast<float>(luaL_checknumber(L, 1)), y = static_cast<float>(luaL_checknumber(L, 2));
    const float w = static_cast<float>(luaL_checknumber(L, 3)), h = static_cast<float>(luaL_checknumber(L, 4));
    const ImU32 color = check_color(L, 5, IM_COL32_WHITE);
    const bool filled = lua_isnoneornil(L, 9) || lua_toboolean(L, 9);
    ImDrawList *list = ImGui::GetForegroundDrawList();
    if (filled)
        list->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), color);
    else
        list->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), color);
    return 0;
}
// Maps the PS1's 320x240 picture coordinates onto the window (the original area
// sits centred in a widescreen picture).
int ui_game_to_screen(lua_State *L) {
    const auto view = original_view();
    lua_pushnumber(L, view.x + luaL_checknumber(L, 1) * view.w / 320.0);
    lua_pushnumber(L, view.y + luaL_checknumber(L, 2) * view.h / 240.0);
    return 2;
}

std::vector<verdite::ModTable> api_tables() {
    using verdite::ModFunction;
    constexpr std::string_view view_type = "fun(): number, number, number, number";
    return {
        {.path = "kf",
         .doc = "King's Field.",
         .functions =
             {ModFunction{"log", "fun(...: any)", "Prints [id] and the values to the console and kf.log.", kf_log},
              {"context", "fun(): \"opening\"|\"gameplay\"|\"menu\"|\"scripted\"", "What the game is showing.",
               kf_context},
              {"frame", "fun(): integer", "Presented frames so far.", kf_frame},
              {"time", "fun(): number", "Seconds since start.", kf_time},
              {"key_down", "fun(name: string): boolean", "An SDL key name held; false while the overlay is open.",
               kf_key_down},
              {"key_pressed", "fun(name: string): boolean", "As key_down, on the first frame only.", kf_key_pressed},
              {"player_fields", "fun(): string[]", "The names of kf.player's fields.", kf_player_fields},
              {"view", view_type, "x, y, w, h of the game picture in window pixels.", kf_view},
              {"view_original", view_type, "x, y, w, h of the original 320x240 area.", kf_view_original},
              {"save_config", "fun()", "Saves config when the callback returns (as mod.save_config).",
               kf_save_config},
              {"warp", "fun(floor: integer, cell_x?: integer, cell_z?: integer, yaw?: integer)",
               "Queued, run between updates: moves the player to a floor and cell, or the first open cell.",
               kf_warp}},
         .fields = {{"api_version", "string", "The API's version.", push_api_version, nullptr}}},
        {.path = "kf.player",
         .doc = "The player's state, read and written in the game's player_state.",
         .functions = {},
         .fields = player_fields()},
        {.path = "kf.world",
         .doc = "The floor's map and its story flags.",
         .functions =
             {ModFunction{"cell_blocked", "fun(x: integer, z: integer): boolean",
                          "True for a wall or rock cell; an error off the map.", world_cell_blocked}},
         .fields = world_fields()},
        {.path = "kf.play",
         .doc = "The calling mod's play switches and speeds; the game combines every running mod's.",
         .functions = {},
         .fields = play_fields()},
        {.path = "kf.game",
         .doc = "The calling mod's game speed and pause; the game combines every running mod's.",
         .functions = {},
         .fields = game_fields()},
        {.path = "ui",
         .doc = "Dear ImGui, inside on_draw and on_settings only. Colours are 0-1.",
         .functions =
             {ModFunction{"text", "fun(text: string)", "", ui_text},
              {"text_colored", "fun(r: number, g: number, b: number, text: string)", "", ui_text_colored},
              {"button", "fun(label: string): boolean", "True when clicked.", ui_button},
              {"checkbox", "fun(label: string, value: boolean): boolean", "", ui_checkbox},
              {"slider_int", "fun(label: string, value: integer, min: integer, max: integer): integer", "",
               ui_slider_int},
              {"slider_float", "fun(label: string, value: number, min: number, max: number, format?: string): number",
               "", ui_slider_float},
              {"combo", "fun(label: string, index: integer, items: string[]): integer", "1-based.", ui_combo},
              {"separator", "fun()", "", ui_separator},
              {"same_line", "fun()", "", ui_same_line},
              {"spacing", "fun()", "", ui_spacing},
              {"tooltip", "fun(text: string)", "For the item before it.", ui_tooltip},
              {"begin_window", "fun(title: string, click_through?: boolean): boolean",
               "Always call end_window after it.", ui_begin_window},
              {"end_window", "fun()", "", ui_end_window},
              {"draw_text",
               "fun(x: number, y: number, text: string, r?: number, g?: number, b?: number, a?: number, size?: number)",
               "Window pixels, over everything; size in pixels (18).", ui_draw_text},
              {"draw_rect",
               "fun(x: number, y: number, w: number, h: number, r?: number, g?: number, b?: number, a?: number, "
               "filled?: boolean)",
               "", ui_draw_rect},
              {"game_to_screen", "fun(x: number, y: number): number, number",
               "The PS1's 320x240 coordinates to window pixels.", ui_game_to_screen}},
         .fields = {}},
    };
}

// ---------- the mods' settings, in settings.json ----------
void replace_item(cJSON *object, const char *key, cJSON *value) {
    if (cJSON_GetObjectItemCaseSensitive(object, key))
        cJSON_ReplaceItemInObjectCaseSensitive(object, key, value);
    else
        cJSON_AddItemToObject(object, key, value);
}

std::optional<verdite::ModStored> read_stored(std::string_view id) {
    const cJSON *entry = settings_mod_entry(std::string(id));
    verdite::ModStored stored;
    stored.enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "enabled"));
    const cJSON *config = cJSON_GetObjectItemCaseSensitive(entry, "config");
    if (cJSON_IsObject(config)) {
        const std::unique_ptr<char, void (*)(void *)> text(cJSON_PrintUnformatted(config), cJSON_free);
        if (text)
            stored.config_json = text.get();
    }
    return stored;
}

void write_stored(std::string_view id, const verdite::ModStored &stored) {
    cJSON *entry = settings_mod_entry(std::string(id));
    replace_item(entry, "enabled", cJSON_CreateBool(stored.enabled));
    cJSON *config = cJSON_ParseWithLength(stored.config_json.data(), stored.config_json.size());
    replace_item(entry, "config", config ? config : cJSON_CreateObject());
    settings_mark_dirty();
}

// First launch: the example mods shipped beside the executable, copied disabled.
void copy_example_mods() {
    std::error_code error;
    if (std::filesystem::exists(mods_dir(), error))
        return;
    const char *base = SDL_GetBasePath();
    if (!base)
        return;
    const auto bundled = std::filesystem::path(reinterpret_cast<const char8_t *>(base)) / "mods";
    if (!std::filesystem::is_directory(bundled, error))
        return;
    std::filesystem::copy(bundled, mods_dir(), std::filesystem::copy_options::recursive, error);
    if (error)
        std::printf("[mods] cannot copy the example mods: %s\n", error.message().c_str());
    else
        std::printf("[mods] copied the example mods to %s (all disabled)\n", mods_dir().string().c_str());
}

void draw_mod_row(const verdite::ModInfo &mod) {
    ImGui::PushID(mod.id.c_str());
    bool enabled = mod.enabled;
    if (ImGui::Checkbox("##enabled", &enabled)) {
        if (enabled)
            host->enable(mod.id);
        else
            host->disable(mod.id);
    }
    ImGui::SameLine();
    ImGui::Text("%s", mod.name.c_str());
    if (!mod.version.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("v%s", mod.version.c_str());
    }
    if (!mod.author.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("by %s", mod.author.c_str());
    }
    ImGui::SameLine();
    if (mod.state == verdite::ModState::Failed)
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "error");
    else if (mod.state == verdite::ModState::Running)
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "running");
    ImGui::Indent();
    if (!mod.description.empty())
        ImGui::TextWrapped("%s", mod.description.c_str());
    if (!mod.error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.45f, 1.0f), "%s", mod.error.c_str());
    if (mod.state == verdite::ModState::Running && mod.has_settings && ImGui::TreeNode("Settings")) {
        host->call(mod.id, verdite::ModCallback::Settings);
        ImGui::TreePop();
    }
    ImGui::Unindent();
    ImGui::Separator();
    ImGui::PopID();
}
}

void mods_start() {
    copy_example_mods();
    verdite::ModHostConfig config;
    config.mods_dir = mods_dir();
    config.api_version = api_version;
    config.api = api_tables();
    config.definitions_name = "kf.d.lua";
    config.read_stored = read_stored;
    config.write_stored = write_stored;
    config.unloaded = forget_mod_play;
    host =std::make_unique<verdite::ModHost>(std::move(config));
    host->scan();
    last_scan = SDL_GetTicks();
}

void mods_shutdown() {
    host.reset();
    mod_plays.clear();
    refresh_mod_play();
}

void mods_between_updates() {
    if (!warp_request)
        return;
    const WarpRequest request = *warp_request;
    warp_request.reset();
    apply_warp(request);
}

bool mods_pause_requested() {
    return combined_play.paused;
}

void mods_poll() {
    if (!host || SDL_GetTicks() - last_scan < 500)
        return;
    last_scan = SDL_GetTicks();
    host->scan();
    refresh_mod_play();
}

void mods_frame(int context) {
    ++frame_counter;
    input_context = context;
    int count = 0;
    const bool *state = SDL_GetKeyboardState(&count);
    keys_before = keys_now;
    for (int i = 0; i < SDL_SCANCODE_COUNT; ++i)
        keys_now[static_cast<std::size_t>(i)] = i < count && state[i];
    if (!host)
        return;
    host->call_each(verdite::ModCallback::Update);
    refresh_mod_play();
}

void mods_draw_hud() {
    if (!host)
        return;
    // Mod windows use the base font size whatever the interface scale is.
    ImGui::PushFont(nullptr, hud_font_size / ImGui::GetStyle().FontScaleMain);
    in_imgui_frame = true;
    for (const verdite::ModInfo &mod : host->mods()) {
        if (mod.state != verdite::ModState::Running)
            continue;
        ImGui::PushID(mod.id.c_str());
        host->call(mod.id, verdite::ModCallback::Draw);
        ImGui::PopID();
    }
    in_imgui_frame = false;
    ImGui::PopFont();
    refresh_mod_play();
}

int mods_loaded_count() {
    return host ? static_cast<int>(host->running_count()) : 0;
}

void mods_draw_panel() {
    if (!host)
        return;
    const auto folder = mods_dir().string();
    ImGui::TextDisabled("Folder: %s", folder.c_str());
    if (ImGui::Button("Open mods folder"))
        SDL_OpenURL(("file:///" + mods_dir().generic_string()).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Reload all"))
        host->reload_all();
    ImGui::SameLine();
    ImGui::TextDisabled("(saving a mod's files reloads it automatically)");
    ImGui::Separator();
    const auto list = host->mods();
    if (list.empty())
        ImGui::TextWrapped("No mods yet. Create a folder in the mods folder with a mod.json and main.lua.");
    in_imgui_frame = true;
    for (const verdite::ModInfo &mod : list)
        draw_mod_row(mod);
    in_imgui_frame = false;
    refresh_mod_play();
}
}

namespace kf {
const PlayOverrides &host_play_overrides() {
    return ext::combined_play.overrides;
}
} // namespace kf
