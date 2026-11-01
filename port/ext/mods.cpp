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
#include <filesystem>
#include <fstream>
#include <string>
#include <limits>
#include <cstdio>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <kf/platform/prelude.h>
#include <kf/game/actor.h>
#include <kf/game/animation_cache.h>
#include <kf/game/collision.h>
#include <kf/game/effect.h>
#include <kf/game/magic.h>
#include <kf/game/player.h>
#include <kf/game/player_motion.h>
#include <kf/lib/equipment_types.h>
#include <kf/lib/floor.h>
#include <kf/lib/item.h>
#include <kf/lib/item_types.h>
#include <kf/lib/map.h>
#include <kf/lib/map_data.h>
#include <kf/lib/math.h>
#include <kf/platform/coop.h>
#include <kf/platform/fuzz.h>
#include <kf/platform/host.h>
#include <kf/platform/input.h>
#include <kf/platform/mod_events.h>
#include <kf/platform/play_overrides.h>
#include "coop_party.h"
#include "mod_play.h"
#include "mod_rules.h"
#include <lauxlib.h>
#include <lua.h>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <variant>
#include <vector>

namespace kf::ext {
namespace {
constexpr const char *api_version = "0.6";

std::unique_ptr<verdite::ModHost> host;
// Each running mod's kf.play and kf.game values, by id, and what they combine to.
std::map<std::string, ModPlay, std::less<>> mod_plays;
ModPlayCombined combined_play;
bool in_imgui_frame;             // ui.* is only valid while drawing
u8 last_floor;                   // the floor last loaded (0 before any), for on_floor_change
Uint64 last_scan;
std::uint64_t frame_counter;
int input_context;
std::array<bool, SDL_SCANCODE_COUNT> keys_now{}, keys_before{};
constexpr std::array<const char *, 4> context_names{"opening", "gameplay", "menu", "scripted"};

// Mods are off in coop: their Lua states stay loaded, and no mod code runs until the game is back.
bool mods_paused() { return coop_world_in_play(); }
bool mods_active() { return host && !mods_paused(); }

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

// The running mods' ids, in load order (the host's listed order).
std::vector<std::string> running_mods() {
    std::vector<std::string> running;
    if (host)
        for (const verdite::ModInfo &mod : host->mods())
            if (mod.state == verdite::ModState::Running)
                running.push_back(mod.id);
    return running;
}

// Recomputes what the game sees from the running mods' values (kf.play, kf.game, kf.rules); a mod
// that is not running any more (unloaded, failed) loses its own.
void refresh_mod_play() {
    const std::vector<std::string> running = running_mods();
    std::erase_if(mod_plays,
                  [&running](const auto &entry) { return std::ranges::find(running, entry.first) == running.end(); });
    // In coop no mod's values apply; they come back when the single-player game does.
    std::vector<ModPlay> values;
    if (!mods_paused()) {
        values.reserve(mod_plays.size());
        for (const auto &entry : mod_plays)
            values.push_back(entry.second);
        rules_lay(running);
    } else {
        rules_lay_originals();
    }
    combined_play = combine_mod_play(values);
    host_set_game_speed(combined_play.game_speed);
}

// The host discarded a mod's Lua state (reload, disable, failure, or the host's own destruction).
// host is already null while ~ModHost runs, so refresh_mod_play does not call into it then.
void forget_mod_play(std::string_view id) {
    if (const auto found = mod_plays.find(id); found != mod_plays.end())
        mod_plays.erase(found);
    rules_forget(id);
    refresh_mod_play();
}

// ---------- kf.actors: the monsters on the floor ----------
// The kinds placed on the floor as it loaded: the models the floor's map loaded are the ones its
// placements name, so these are the kinds kf.actors.spawn may make.
std::array<bool, KF_ACTOR_DEFINITION_COUNT> kinds_on_floor{};

void record_kinds_on_floor() {
    kinds_on_floor.fill(false);
    for (const KfActor &actor : actor_state.actors)
        if (actor.slot_state != KF_ACTOR_SLOT_FREE && actor.definition_id < kinds_on_floor.size())
            kinds_on_floor[actor.definition_id] = true;
}

// A floor is loaded and the game shows it, so the actors are the floor's to read and change.
bool floor_in_play() {
    return last_floor != 0
        && (input_context == static_cast<int>(InputContext::Gameplay) || input_context == static_cast<int>(InputContext::Menu));
}

void check_floor_in_play(lua_State *L) {
    if (!floor_in_play())
        luaL_error(L, "kf.actors works in gameplay or a menu, with a floor loaded");
}

// Listed: in use and not disabled, as the game's own update and drawing see it.
bool actor_listed(const KfActor &actor) {
    return actor.slot_state != KF_ACTOR_SLOT_FREE && actor.lifecycle != KF_ACTOR_LIFECYCLE_DISABLED;
}

bool slot_listed(lua_Integer slot) {
    return slot >= 0 && slot < static_cast<lua_Integer>(actor_state.actors.size())
        && actor_listed(actor_state.actors[static_cast<std::size_t>(slot)]);
}

std::size_t check_listed_slot(lua_State *L, int index) {
    const lua_Integer slot = luaL_checkinteger(L, index);
    if (!slot_listed(slot))
        luaL_error(L, "actor slot %I is not listed", slot);
    return static_cast<std::size_t>(slot);
}

bool actor_dying(const KfActor &actor) {
    return actor.action == KF_ACTOR_ACTION_DYING || actor.action == KF_ACTOR_ACTION_POST_DEATH;
}

// The action as the mods name it: the attacks share one name, and the rest are "other".
struct ActionName {
    KfActorAction action;
    const char *name;
};
constexpr std::array<ActionName, 11> action_names = {{
    {KF_ACTOR_ACTION_IDLE, "idle"},
    {KF_ACTOR_ACTION_WANDER, "wander"},
    {KF_ACTOR_ACTION_PURSUE, "pursue"},
    {KF_ACTOR_ACTION_RETREAT, "retreat"},
    {KF_ACTOR_ACTION_MELEE_ATTACK, "attack"},
    {KF_ACTOR_ACTION_JUMP_ATTACK, "attack"},
    {KF_ACTOR_ACTION_SPECIAL_ATTACK, "attack"},
    {KF_ACTOR_ACTION_MULTI_HIT_ATTACK, "attack"},
    {KF_ACTOR_ACTION_HIT_REACTION, "hurt"},
    {KF_ACTOR_ACTION_DYING, "dying"},
    {KF_ACTOR_ACTION_POST_DEATH, "dying"},
}};

const char *action_name(KfActorAction action) {
    for (const ActionName &entry : action_names)
        if (entry.action == action)
            return entry.name;
    return "other";
}

void set_integer(lua_State *L, const char *key, lua_Integer value) {
    lua_pushinteger(L, value);
    lua_setfield(L, -2, key);
}

void push_actor(lua_State *L, std::size_t slot) {
    const KfActor &actor = actor_state.actors[slot];
    lua_createtable(L, 0, 10);
    set_integer(L, "slot", static_cast<lua_Integer>(slot));
    set_integer(L, "kind", actor.definition_id);
    set_integer(L, "hp", actor.health);
    set_integer(L, "x", actor.position.vx);
    set_integer(L, "y", actor.position.vy);
    set_integer(L, "z", actor.position.vz);
    set_integer(L, "yaw", actor.rotation.vy & KF_ANGLE_WRAP_MASK);
    set_integer(L, "cell_x", actor.cell_x);
    set_integer(L, "cell_z", actor.cell_z);
    lua_pushstring(L, action_name(actor.action));
    lua_setfield(L, -2, "action");
}

// The definition's numbers as the game reads them; drop_item is -1 when the kind drops nothing.
void push_kind(lua_State *L, std::size_t kind) {
    const KfActorDefinition &definition = actor_state.definitions.entries[kind];
    const KfObjectId drop = definition.action_parameters.drop_object;
    const bool drops = drop != KF_OBJECT_NONE && drop != KF_MAP_OBJECT_DROP_DISABLED;
    lua_createtable(L, 0, 5);
    set_integer(L, "kind", static_cast<lua_Integer>(kind));
    set_integer(L, "hp", definition.initial_health);
    set_integer(L, "experience", definition.experience_reward);
    set_integer(L, "gold", definition.gold_drop_limit);
    set_integer(L, "drop_item", drops ? kf_enum_encode<lua_Integer>(drop) : -1);
}

// A world position in units, on the map and in a cell that is not blocked, at the floor's height.
VECTOR check_open_position(lua_State *L, int index) {
    const lua_Integer x = luaL_checkinteger(L, index);
    const lua_Integer z = luaL_checkinteger(L, index + 1);
    const lua_Integer width = static_cast<lua_Integer>(KF_MAP_COLUMNS) * KF_MAP_TILE_SIZE;
    const lua_Integer depth = static_cast<lua_Integer>(KF_MAP_ROWS) * KF_MAP_TILE_SIZE;
    if (x < 0 || x >= width || z < 0 || z >= depth)
        luaL_error(L, "position %I, %I is off the map", x, z);
    if (cell_is_blocked(static_cast<int>(x / KF_MAP_TILE_SIZE), static_cast<int>(z / KF_MAP_TILE_SIZE)))
        luaL_error(L, "position %I, %I is in a blocked cell", x, z);
    VECTOR position = {static_cast<s32>(x), 0, static_cast<s32>(z)};
    position.vy = map_floor_height_at_position(&position);
    return position;
}

// An optional yaw, wrapped to a turn; nothing when the argument is absent.
std::optional<s16> optional_yaw(lua_State *L, int index) {
    if (lua_isnoneornil(L, index))
        return std::nullopt;
    return static_cast<s16>(luaL_checkinteger(L, index) & KF_ANGLE_WRAP_MASK);
}

// An active actor holds occupancy on its cell (the update takes it off and puts it back around
// each action); a dormant, waiting or disabled one holds none.
void release_occupancy(KfActor &actor) {
    if (actor.lifecycle == KF_ACTOR_LIFECYCLE_ACTIVE)
        collision_adjust_cell_occupancy(actor.cell_x, actor.cell_z, -1);
}

// Moves an actor the way the game moves one: its occupancy follows it from the old cell to the new.
void place_actor(KfActor &actor, const VECTOR &position) {
    const bool active = actor.lifecycle == KF_ACTOR_LIFECYCLE_ACTIVE;
    release_occupancy(actor);
    // As actor_set_position (not declared in a header): the position and its cell.
    actor.position = position;
    actor.cell_x = static_cast<u16>(position.vx / KF_MAP_TILE_SIZE);
    actor.cell_z = static_cast<u16>(position.vz / KF_MAP_TILE_SIZE);
    if (active)
        collision_adjust_cell_occupancy(actor.cell_x, actor.cell_z, 1);
}

// As the end of a death animation does: the actor leaves the game's update and its occupancy with it.
void disable_actor(KfActor &actor) {
    release_occupancy(actor);
    actor.lifecycle = KF_ACTOR_LIFECYCLE_DISABLED;
}

int kf_actors_list(lua_State *L) {
    check_floor_in_play(L);
    lua_createtable(L, 0, 0);
    lua_Integer count = 0;
    for (std::size_t slot = 0; slot < actor_state.actors.size(); ++slot) {
        if (!actor_listed(actor_state.actors[slot]))
            continue;
        push_actor(L, slot);
        lua_rawseti(L, -2, ++count);
    }
    return 1;
}

int kf_actors_get(lua_State *L) {
    check_floor_in_play(L);
    const lua_Integer slot = luaL_checkinteger(L, 1);
    if (!slot_listed(slot)) {
        lua_pushnil(L);
        return 1;
    }
    push_actor(L, static_cast<std::size_t>(slot));
    return 1;
}

int kf_actors_kinds(lua_State *L) {
    check_floor_in_play(L);
    lua_createtable(L, 0, 0);
    lua_Integer count = 0;
    for (std::size_t kind = 0; kind < kinds_on_floor.size(); ++kind) {
        if (!kinds_on_floor[kind])
            continue;
        push_kind(L, kind);
        lua_rawseti(L, -2, ++count);
    }
    return 1;
}

int kf_actors_set_hp(lua_State *L) {
    check_floor_in_play(L);
    const std::size_t slot = check_listed_slot(L, 1);
    KfActor &actor = actor_state.actors[slot];
    const lua_Integer hp = luaL_checkinteger(L, 2);
    if (hp < 1 || hp > 65535)
        luaL_error(L, "hp %I is out of range (1 to 65535; kill takes an actor down)", hp);
    if (actor_dying(actor))
        luaL_error(L, "actor slot %I is dying", static_cast<lua_Integer>(slot));
    actor.health = static_cast<u16>(hp);
    return 0;
}

int kf_actors_move(lua_State *L) {
    check_floor_in_play(L);
    KfActor &actor = actor_state.actors[check_listed_slot(L, 1)];
    place_actor(actor, check_open_position(L, 2));
    if (const auto yaw = optional_yaw(L, 4))
        actor.rotation.vy = *yaw;
    return 0;
}

// The game's own kill: the death animation plays and the drops fall as usual, and no experience.
// An actor already dying is left to finish.
int kf_actors_kill(lua_State *L) {
    check_floor_in_play(L);
    KfActor &actor = actor_state.actors[check_listed_slot(L, 1)];
    if (actor_dying(actor))
        return 0;
    actor.health = 0;
    const KfActorDefinition &definition = actor_state.definitions.entries[actor.definition_id];
    if (actor.lifecycle == KF_ACTOR_LIFECYCLE_ACTIVE
        && definition.action_animations[KF_ACTOR_ANIM_SLOT_DEATH] != KF_ANIMATION_CLIP_NONE) {
        actor_set_action(&actor, KF_ACTOR_ACTION_DYING);
        return 0;
    }
    disable_actor(actor);
    return 0;
}

// Takes an actor out of the floor without a death: its slot is free again, and so is its cell.
int kf_actors_remove(lua_State *L) {
    check_floor_in_play(L);
    KfActor &actor = actor_state.actors[check_listed_slot(L, 1)];
    release_occupancy(actor);
    // The cached vertices belong to the actor; releasing the record also clears its pointer.
    if (actor.animation_cache)
        animation_cache_release(actor.animation_cache);
    actor.slot_state = KF_ACTOR_SLOT_FREE;
    actor.lifecycle = KF_ACTOR_LIFECYCLE_DORMANT;
    return 0;
}

// Spawns a kind in use on this floor, as the game's spawners do: the first free slot, pursuing.
int kf_actors_spawn(lua_State *L) {
    check_floor_in_play(L);
    const lua_Integer kind = luaL_checkinteger(L, 1);
    if (kind < 0 || kind >= KF_ACTOR_DEFINITION_COUNT || !kinds_on_floor[static_cast<std::size_t>(kind)])
        luaL_error(L, "kind %I is not on this floor", kind);
    const VECTOR position = check_open_position(L, 2);
    const auto yaw = optional_yaw(L, 4);
    const auto free = std::ranges::find_if(actor_state.actors,
        [](const KfActor &actor) { return actor.slot_state == KF_ACTOR_SLOT_FREE; });
    if (free == actor_state.actors.end()) {
        lua_pushnil(L);
        return 1;
    }
    const auto slot = static_cast<std::size_t>(free - actor_state.actors.begin());
    const SVECTOR rotation = {0, 0, 0};
    actor_pool_spawn(static_cast<u8>(kind), &position, &rotation);
    KfActor &actor = actor_state.actors[slot];
    if (actor.slot_state == KF_ACTOR_SLOT_FREE || actor.definition_id != kind) {
        lua_pushnil(L);
        return 1;
    }
    if (yaw)
        actor.rotation.vy = *yaw;   // the spawn takes a random yaw
    lua_pushinteger(L, static_cast<lua_Integer>(slot));
    return 1;
}

// ---------- kf.items: the player's pack ----------
u8 &pack_count(u8 item) {
    return item_stock[kf_enum_encode<u8>(KF_ITEM_STOCK_PLAYER)][item];
}

// An item the pack can name: 0 to 79 with a name in the menus.
u8 check_named_item(lua_State *L, int index) {
    const lua_Integer item = luaL_checkinteger(L, index);
    if (item < 0 || item >= KF_ITEM_COUNT || !item_english_name(static_cast<u8>(item)))
        luaL_error(L, "item %I is not a named item (0 to 79)", item);
    return static_cast<u8>(item);
}

int kf_items_count(lua_State *L) {
    const lua_Integer item = luaL_checkinteger(L, 1);
    if (item < 0 || item >= KF_ITEM_COUNT)
        luaL_error(L, "item %I is not 0 to 79", item);
    lua_pushinteger(L, pack_count(static_cast<u8>(item)));
    return 1;
}

// A count above a stack gives or takes all of it either way, so it is held to one stack.
int check_amount(lua_State *L, int index) {
    const lua_Integer amount = luaL_checkinteger(L, index);
    if (amount < 1)
        luaL_error(L, "amount %I is below 1", amount);
    return static_cast<int>(std::min<lua_Integer>(amount, KF_ITEM_STACK_CAPACITY));
}

int kf_items_give(lua_State *L) {
    u8 &count = pack_count(check_named_item(L, 1));
    const int total = std::min(count + check_amount(L, 2), static_cast<int>(KF_ITEM_STACK_CAPACITY));
    count = static_cast<u8>(total);
    lua_pushinteger(L, total);
    return 1;
}

int kf_items_take(lua_State *L) {
    u8 &count = pack_count(check_named_item(L, 1));
    const int left = std::max(count - check_amount(L, 2), 0);
    count = static_cast<u8>(left);
    lua_pushinteger(L, left);
    return 1;
}

int kf_items_name(lua_State *L) {
    const lua_Integer item = luaL_checkinteger(L, 1);
    const char *name = item >= 0 && item < KF_ITEM_COUNT ? item_english_name(static_cast<u8>(item)) : nullptr;
    if (name)
        lua_pushstring(L, name);
    else
        lua_pushnil(L);
    return 1;
}

// ---------- kf.equipped and kf.equip: what the player wears ----------
// The slots the mods name, with coop_party.h's order for coop_item_fits. The weapon is not an armour
// slot: the game puts it on with player_equip_weapon, which loads its model.
struct EquipmentSlot {
    const char *name;
    std::size_t worn_index;
    std::optional<KfEquipmentSlot> armour;
    KfObjectId KfPlayerState::*worn;
};
constexpr std::array<EquipmentSlot, 7> equipment_slots = {{
    {"weapon", 0, std::nullopt, &KfPlayerState::equipped_weapon_id},
    {"head", 1, KF_EQUIPMENT_SLOT_HEAD, &KfPlayerState::equipped_head_armor_id},
    {"body", 2, KF_EQUIPMENT_SLOT_BODY, &KfPlayerState::equipped_body_armor_id},
    {"shield", 3, KF_EQUIPMENT_SLOT_SHIELD, &KfPlayerState::equipped_shield_id},
    {"arm", 4, KF_EQUIPMENT_SLOT_ARM, &KfPlayerState::equipped_arm_armor_id},
    {"leg", 5, KF_EQUIPMENT_SLOT_LEG, &KfPlayerState::equipped_leg_armor_id},
    {"accessory", 6, KF_EQUIPMENT_SLOT_ACCESSORY, &KfPlayerState::equipped_accessory_id},
}};

std::size_t check_equipment_slot(lua_State *L, int index) {
    const char *name = luaL_checkstring(L, index);
    const auto found = std::ranges::find_if(equipment_slots,
        [name](const EquipmentSlot &slot) { return std::strcmp(slot.name, name) == 0; });
    if (found == equipment_slots.end())
        luaL_error(L, "there is no equipment slot '%s'", name);
    return static_cast<std::size_t>(found - equipment_slots.begin());
}

// A worn item, which must fit the slot and be carried.
std::uint8_t check_worn_item(lua_State *L, int index, std::size_t entry) {
    const lua_Integer item = luaL_checkinteger(L, index);
    if (item < 0 || item >= KF_ITEM_COUNT)
        luaL_error(L, "item %I is not 0 to 79", item);
    const auto id = static_cast<std::uint8_t>(item);
    if (!coop_item_fits(equipment_slots[entry].worn_index, id))
        luaL_error(L, "item %I does not fit the %s slot", item, equipment_slots[entry].name);
    if (pack_count(id) < 1)
        luaL_error(L, "the player does not carry item %I", item);
    return id;
}

// An equip waits for the next update, as a warp does; nil takes the item off.
struct EquipRequest {
    std::size_t entry;
    std::optional<std::uint8_t> item;
};
std::vector<EquipRequest> equip_requests;

int kf_equipped(lua_State *L) {
    const EquipmentSlot &slot = equipment_slots[check_equipment_slot(L, 1)];
    const KfObjectId worn = player_state.*slot.worn;
    if (worn == KF_OBJECT_NONE)
        lua_pushnil(L);
    else
        lua_pushinteger(L, kf_enum_encode<lua_Integer>(worn));
    return 1;
}

int kf_equip(lua_State *L) {
    const std::size_t entry = check_equipment_slot(L, 1);
    std::optional<std::uint8_t> item;
    if (!lua_isnoneornil(L, 2))
        item = check_worn_item(L, 2, entry);
    // A slot keeps only its latest request, so the queue holds at most one per slot, in call order.
    std::erase_if(equip_requests, [entry](const EquipRequest &request) { return request.entry == entry; });
    equip_requests.push_back({entry, item});
    return 0;
}

// Puts an item on or takes one off, as the equipment menu does. Its stock is not changed: a worn
// item stays in the pack's count.
void apply_equip(const EquipRequest &request) {
    const EquipmentSlot &slot = equipment_slots[request.entry];
    if (request.item && pack_count(*request.item) < 1) {
        std::printf("[mods] equip %s: the player no longer carries item %d\n", slot.name, *request.item);
        return;
    }
    const KfObjectId item = request.item ? kf_enum_decode<KfObjectId>(*request.item) : KF_OBJECT_NONE;
    if (!slot.armour) {
        player_equip_weapon(item);
        return;
    }
    player_set_equipment_slot(item, *slot.armour);
    // Full Plate covers the arms and legs, so the game takes those off, as its menu does.
    if (slot.armour == KF_EQUIPMENT_SLOT_BODY && request.item == kf_enum_encode<u8>(KF_ITEM_FULL_PLATE)) {
        player_set_equipment_slot(KF_OBJECT_NONE, KF_EQUIPMENT_SLOT_ARM);
        player_set_equipment_slot(KF_OBJECT_NONE, KF_EQUIPMENT_SLOT_LEG);
    }
}

// ---------- kf.magic: the player's spells ----------
KfEffectKind check_spell(lua_State *L, int index) {
    const lua_Integer spell = luaL_checkinteger(L, index);
    if (spell < 0 || spell >= KF_MAGIC_PLAYER_COUNT)
        luaL_error(L, "spell %I is not 0 to 8", spell);
    return kf_enum_decode<KfEffectKind>(static_cast<u8>(spell));
}

int kf_magic_learned(lua_State *L) {
    const KfEffectKind spell = check_spell(L, 1);
    lua_pushboolean(L, effect_state.magic.entries[kf_enum_encode<u8>(spell)].learned == KF_MAGIC_LEARNED);
    return 1;
}

int kf_magic_set_learned(lua_State *L) {
    const KfEffectKind spell = check_spell(L, 1);
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    const bool learned = lua_toboolean(L, 2) != 0;
    effect_state.magic.entries[kf_enum_encode<u8>(spell)].learned = learned ? KF_MAGIC_LEARNED : KF_MAGIC_UNLEARNED;
    // An unlearned spell is deselected, as the spell menu does.
    if (!learned && player_state.selected_magic_id == spell)
        player_select_magic(KF_MAGIC_NONE);
    return 0;
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
               kf_warp},
              {"equipped", "fun(slot: \"weapon\"|\"head\"|\"body\"|\"arm\"|\"leg\"|\"shield\"|\"accessory\"): integer?",
               "The item id worn in a slot, or nil.", kf_equipped},
              {"equip", "fun(slot: \"weapon\"|\"head\"|\"body\"|\"arm\"|\"leg\"|\"shield\"|\"accessory\", item?: integer)",
               "Wears a carried item that fits the slot, or takes it off (nil). Done between updates.", kf_equip}},
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
        {.path = "kf.actors",
         .doc = "The monsters on the floor. A slot is listed when it is in use and not disabled; a call on an "
                "unlisted slot is an error, except get. Gameplay or a menu, with a floor loaded.",
         .functions =
             {ModFunction{"list",
                          "fun(): {slot: integer, kind: integer, hp: integer, x: integer, y: integer, z: integer, "
                          "yaw: integer, cell_x: integer, cell_z: integer, action: string}[]",
                          "The listed actors, in slot order. Positions in world units, 2000 to a cell.",
                          kf_actors_list},
              {"get",
               "fun(slot: integer): {slot: integer, kind: integer, hp: integer, x: integer, y: integer, z: integer, "
               "yaw: integer, cell_x: integer, cell_z: integer, action: string}?",
               "The actor in a slot, or nil when it is not listed.", kf_actors_get},
              {"kinds", "fun(): {kind: integer, hp: integer, experience: integer, gold: integer, drop_item: integer}[]",
               "The monster kinds placed on this floor. drop_item is -1 for none.", kf_actors_kinds},
              {"set_hp", "fun(slot: integer, hp: integer)", "Sets an actor's HP, 1 to 65535; not one that is dying.",
               kf_actors_set_hp},
              {"move", "fun(slot: integer, x: integer, z: integer, yaw?: integer)",
               "Moves an actor to a world position in an open cell; its height is the floor's.", kf_actors_move},
              {"kill", "fun(slot: integer)",
               "Kills an actor as the game does: it dies, drops as usual, and gives no experience.", kf_actors_kill},
              {"remove", "fun(slot: integer)", "Takes an actor out without a death or a drop; its slot and cell are free.",
               kf_actors_remove},
              {"spawn", "fun(kind: integer, x: integer, z: integer, yaw?: integer): integer?",
               "Spawns a kind placed on this floor at a world position; the slot, or nil when the pool is full.",
               kf_actors_spawn}},
         .fields = {}},
        {.path = "kf.items",
         .doc = "The player's pack: stacks of 99 for each item id (0 to 79).",
         .functions =
             {ModFunction{"count", "fun(item: integer): integer", "How many of an item the pack holds.", kf_items_count},
              {"give", "fun(item: integer, n: integer): integer",
               "Adds n of a named item, up to a stack; the new count.", kf_items_give},
              {"take", "fun(item: integer, n: integer): integer", "Takes n of a named item, down to 0; the new count.",
               kf_items_take},
              {"name", "fun(item: integer): string?", "The item's English name, or nil for an id with none.",
               kf_items_name}},
         .fields = {}},
        {.path = "kf.magic",
         .doc = "The player's spells, ids 0 to 8 as KfEffectKind (the spell menu lists 4 to 8).",
         .functions =
             {ModFunction{"learned", "fun(spell: integer): boolean", "Whether the player has learned a spell.",
                          kf_magic_learned},
              {"set_learned", "fun(spell: integer, learned: boolean)",
               "Learns or unlearns a spell; an unlearned spell that is selected is deselected.",
               kf_magic_set_learned}},
         .fields = {}},
        rules_table(),
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

// ---------- the events the game raises (kf/platform/mod_events.h) ----------
verdite::ModEventField integer_field(std::string_view name, std::string_view doc) {
    return {.name = name, .type = "integer", .doc = doc};
}
verdite::ModEventField writable_field(std::string_view name, std::string_view doc, std::int64_t min, std::int64_t max) {
    return {.name = name, .type = "integer", .doc = doc, .writable = true, .min = min, .max = max};
}
verdite::ModEventField boolean_field(std::string_view name, std::string_view doc) {
    return {.name = name, .type = "boolean", .doc = doc};
}
verdite::ModEventField writable_boolean_field(std::string_view name, std::string_view doc) {
    return {.name = name, .type = "boolean", .doc = doc, .writable = true};
}
verdite::ModEventField string_field(std::string_view name, std::string_view doc) {
    return {.name = name, .type = "string", .doc = doc};
}

std::vector<verdite::ModEvent> event_table() {
    return {
        {.name = "on_damage",
         .doc = "The player or an actor loses HP: a blow, or the player's poison or equipment drain.",
         .fields = {string_field("target", "\"player\" or \"actor\"."),
                    integer_field("actor", "The actor's slot; -1 for the player."),
                    integer_field("kind", "The actor's definition id; -1 for the player."),
                    writable_field("amount", "HP lost; 0 for none.", 0, 65535),
                    boolean_field("by_player", "The player made the blow; false for the player's own damage."),
                    string_field("cause", "\"blow\", \"poison\" or \"equipment\".")}},
        {.name = "on_kill",
         .doc = "A blow killed an actor.",
         .fields = {integer_field("actor", "The actor's slot."),
                    integer_field("kind", "The actor's definition id."),
                    writable_field("experience", "EXP the player gets; 0 unless the player made the kill.", 0, 32767),
                    boolean_field("by_player", "The player made the killing blow.")}},
        {.name = "on_drop",
         .doc = "A dying actor drops its gold and item.",
         .fields = {integer_field("actor", "The actor's slot."),
                    integer_field("kind", "The actor's definition id."),
                    writable_field("gold", "The gold that falls; 0 for none.", 0, 65535),
                    writable_field("item", "The item that falls; -1 for none. 39, the gold coin, falls as none.", -1,
                                   KF_ITEM_COUNT - 1)}},
        {.name = "on_level_up",
         .doc = "The player reached a new level.",
         .fields = {integer_field("level", "The new level.")}},
        {.name = "on_item_pickup",
         .doc = "The player takes an item from the ground or a container.",
         .fields = {integer_field("item", "The item id."),
                    writable_boolean_field("cancel", "true leaves the item where it lies.")}},
        {.name = "on_map_load",
         .doc = "A floor's map was loaded.",
         .fields = {integer_field("floor", "1 to 5."),
                    integer_field("map_variant", "0 the default, 1 floor 5's entry.")}},
        {.name = "on_floor_change",
         .doc = "The floor differs from the one last loaded.",
         .fields = {integer_field("from", "The floor last loaded; 0 before any."),
                    integer_field("to", "The floor now loaded.")}},
        {.name = "on_dialogue",
         .doc = "A dialogue page is shown.",
         .fields = {integer_field("floor", "1 to 5."),
                    integer_field("character", "The character's id."),
                    integer_field("stage", "The dialogue's stage."),
                    integer_field("page", "The page within the stage.")}},
        {.name = "on_shop",
         .doc = "A shop opens.",
         .fields = {integer_field("bank", "The shop's stock bank."),
                    writable_boolean_field("cancel", "true keeps the shop closed.")}},
        {.name = "on_death",
         .doc = "The player dies, before the revival or restart.",
         .fields = {}},
        {.name = "on_save",
         .doc = "The game is saved, after its save data is written.",
         .fields = {integer_field("slot", "1 to 3 the game's slots, 4 the quick save, 5 the game set aside in coop.")}},
        {.name = "on_load_game",
         .doc = "A save is loaded, after its save data is read.",
         .fields = {integer_field("slot", "1 to 3 the game's slots, 4 the quick save, 5 the game set aside in coop.")}},
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

// The example mods already offered, one folder name a line, in mods/.examples: an example is
// copied once, so one the user deletes stays deleted, and a new one reaches an existing folder.
std::set<std::string> offered_examples(const std::filesystem::path &list) {
    std::set<std::string> names;
    std::ifstream in(list);
    for (std::string line; std::getline(in, line);)
        if (!line.empty())
            names.insert(line);
    return names;
}

void write_offered_examples(const std::filesystem::path &list, const std::set<std::string> &names) {
    std::ofstream out(list, std::ios::trunc);
    for (const std::string &name : names)
        out << name << '\n';
    if (!out)
        std::printf("[mods] cannot write %s\n", list.string().c_str());
}

// The example mods shipped beside the executable, each copied disabled the first time it is seen.
void copy_example_mods() {
    const char *base = SDL_GetBasePath();
    if (!base)
        return;
    const auto bundled = std::filesystem::path(reinterpret_cast<const char8_t *>(base)) / "mods";
    std::error_code error;
    if (!std::filesystem::is_directory(bundled, error))
        return;
    std::filesystem::create_directories(mods_dir(), error);
    const auto list = mods_dir() / ".examples";
    auto offered = offered_examples(list);
    const auto before = offered.size();
    for (const auto &entry : std::filesystem::directory_iterator(bundled, error)) {
        const std::string name = entry.path().filename().string();
        if (!entry.is_directory(error) || offered.contains(name))
            continue;
        offered.insert(name);
        const auto target = mods_dir() / name;
        if (std::filesystem::exists(target, error))
            continue;
        std::filesystem::copy(entry.path(), target, std::filesystem::copy_options::recursive, error);
        if (error)
            std::printf("[mods] cannot copy the example mod %s: %s\n", name.c_str(), error.message().c_str());
        else
            std::printf("[mods] copied the example mod %s to %s (disabled)\n", name.c_str(), target.string().c_str());
    }
    if (offered.size() != before)
        write_offered_examples(list, offered);
}

void draw_mod_row(const verdite::ModInfo &mod) {
    ImGui::PushID(mod.id.c_str());
    const bool paused = mods_paused();
    bool enabled = mod.enabled;
    ImGui::BeginDisabled(paused);
    if (ImGui::Checkbox("##enabled", &enabled)) {
        if (enabled)
            host->enable(mod.id);
        else
            host->disable(mod.id);
    }
    ImGui::EndDisabled();
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
    // A disabled mod has no state label; a SameLine with nothing after it would put the
    // description on the name's line.
    if (mod.state == verdite::ModState::Failed) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "error");
    } else if (mod.state == verdite::ModState::Running) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "running");
    }
    ImGui::Indent();
    if (!mod.description.empty())
        ImGui::TextWrapped("%s", mod.description.c_str());
    if (!mod.error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.45f, 1.0f), "%s", mod.error.c_str());
    ImGui::BeginDisabled(paused);
    if (mod.state == verdite::ModState::Running && mod.has_settings && ImGui::TreeNode("Settings")) {
        if (!paused)
            host->call(mod.id, verdite::ModCallback::Settings);
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
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
    config.events = event_table();
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
    last_floor = 0;
    refresh_mod_play();
}

void mods_between_updates() {
    // Coop may have just put its world in: the rules come off before it updates.
    refresh_mod_play();
    // Requests queued before coop began are dropped, not applied to the coop world.
    const bool apply = !mods_paused();
    if (warp_request) {
        const WarpRequest request = *warp_request;
        warp_request.reset();
        if (apply)
            apply_warp(request);
    }
    if (apply)
        for (const EquipRequest &request : equip_requests)
            apply_equip(request);
    equip_requests.clear();
}

bool mods_pause_requested() {
    return mods_active() && combined_play.paused;
}

// A file saved in coop reloads on the first scan after leaving it.
void mods_poll() {
    if (!host || mods_paused() || SDL_GetTicks() - last_scan < 500)
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
    if (!mods_paused())
        host->call_each(verdite::ModCallback::Update);
    // Also while paused, so the game sees the defaults from the first frame of coop.
    refresh_mod_play();
}

void mods_draw_hud() {
    if (!mods_active())
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
    return mods_active() ? static_cast<int>(host->running_count()) : 0;
}

void mods_draw_panel() {
    if (!host)
        return;
    if (mods_paused())
        ImGui::TextWrapped("Mods are off in coop. They come back when you leave the room.");
    const auto folder = mods_dir().string();
    ImGui::TextDisabled("Folder: %s", folder.c_str());
    if (ImGui::Button("Open mods folder"))
        SDL_OpenURL(("file:///" + mods_dir().generic_string()).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(mods_paused());
    if (ImGui::Button("Reload all"))
        host->reload_all();
    ImGui::EndDisabled();
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
    static const PlayOverrides defaults;
    return ext::mods_paused() ? defaults : ext::combined_play.overrides;
}

namespace {
// The host checks a writable field when it reads it back, so a value outside the field's range
// would fail the mod even if the mod changed nothing. Such an event is not raised.
bool holds(std::int64_t value, std::int64_t min, std::int64_t max) {
    return value >= min && value <= max;
}

// Runs the handlers, then takes the kf.play and kf.game values they set.
void raise_event(std::string_view event, std::span<verdite::ModEventValue> values) {
    ext::host->raise(event, values);
    ext::refresh_mod_play();
}

// on_damage on the player, from cause; returns the loss the mods left.
s32 raise_player_damage(std::string_view cause, s32 loss_hp) {
    if (!ext::mods_active() || !holds(loss_hp, 0, 65535))
        return loss_hp;
    std::array<verdite::ModEventValue, 6> values{std::string_view{"player"}, std::int64_t{-1}, std::int64_t{-1},
                                                 std::int64_t{loss_hp}, false, cause};
    raise_event("on_damage", values);
    return static_cast<s32>(std::get<std::int64_t>(values[3]));
}

// The cause on_damage names for each HpDrain.
constexpr std::array<std::string_view, 2> drain_causes{"poison", "equipment"};

// Raises an event whose last field is cancel; true when no mod cancelled.
bool raise_cancellable(std::string_view event, std::span<verdite::ModEventValue> values) {
    raise_event(event, values);
    return !std::get<bool>(values.back());
}
} // namespace

s32 mods_player_damaged(s32 loss_hp) {
    return raise_player_damage("blow", loss_hp);
}

s32 mods_player_drained(HpDrain cause, s32 loss_hp) {
    return raise_player_damage(drain_causes.at(kf_enum_encode<std::size_t>(cause)), loss_hp);
}

s32 mods_actor_damaged(const ActorHit &hit) {
    if (!ext::mods_active() || !holds(hit.damage_hp, 0, 65535))
        return hit.damage_hp;
    std::array<verdite::ModEventValue, 6> values{std::string_view{"actor"}, std::int64_t{hit.actor_index},
                                                 std::int64_t{hit.definition_id}, std::int64_t{hit.damage_hp},
                                                 hit.by_player, std::string_view{"blow"}};
    raise_event("on_damage", values);
    return static_cast<s32>(std::get<std::int64_t>(values[3]));
}

s16 mods_actor_killed(const ActorHit &hit, s16 experience) {
    // Only the player's kill gives experience, so only then is it offered to the mods.
    const std::int64_t offered = hit.by_player ? experience : 0;
    if (!ext::mods_active() || !holds(offered, 0, 32767))
        return experience;
    std::array<verdite::ModEventValue, 4> values{std::int64_t{hit.actor_index}, std::int64_t{hit.definition_id},
                                                 offered, hit.by_player};
    raise_event("on_kill", values);
    if (!hit.by_player)
        return experience;
    return static_cast<s16>(std::get<std::int64_t>(values[2]));
}

// The gold coin as an item has no amount, so a mod's item 39 falls as none; the game's own is kept.
ActorDrop mods_actor_drops(const ActorDrop &drop) {
    if (!ext::mods_active() || !holds(drop.item, -1, KF_ITEM_COUNT - 1))
        return drop;
    std::array<verdite::ModEventValue, 4> values{std::int64_t{drop.actor_index}, std::int64_t{drop.definition_id},
                                                 std::int64_t{drop.gold}, std::int64_t{drop.item}};
    raise_event("on_drop", values);
    ActorDrop left = drop;
    left.gold = static_cast<u16>(std::get<std::int64_t>(values[2]));
    left.item = static_cast<s16>(std::get<std::int64_t>(values[3]));
    if (left.item == kf_enum_encode<s16>(KF_ITEM_GOLD_COIN) && left.item != drop.item)
        left.item = -1;
    return left;
}

void mods_level_up(u8 level) {
    if (!ext::mods_active())
        return;
    std::array<verdite::ModEventValue, 1> values{std::int64_t{level}};
    raise_event("on_level_up", values);
}

bool mods_item_picked_up(u16 item) {
    if (!ext::mods_active())
        return true;
    std::array<verdite::ModEventValue, 2> values{std::int64_t{item}, false};
    return raise_cancellable("on_item_pickup", values);
}

// In coop last_floor keeps the single-player floor, so on_floor_change after leaving compares with
// the floor the game was on before it, not a coop floor.
void mods_floor_loaded(u8 floor, u8 map_variant) {
    ext::record_kinds_on_floor();   // the kinds this floor's placements use, for kf.actors
    if (ext::mods_paused())
        return;
    const u8 from = ext::last_floor;
    ext::last_floor = floor;
    if (!ext::host)
        return;
    std::array<verdite::ModEventValue, 2> loaded{std::int64_t{floor}, std::int64_t{map_variant}};
    raise_event("on_map_load", loaded);
    if (floor == from)
        return;
    std::array<verdite::ModEventValue, 2> changed{std::int64_t{from}, std::int64_t{floor}};
    raise_event("on_floor_change", changed);
}

void mods_dialogue(u8 floor, u8 character, u8 stage, u8 page) {
    if (!ext::mods_active())
        return;
    std::array<verdite::ModEventValue, 4> values{std::int64_t{floor}, std::int64_t{character},
                                                 std::int64_t{stage}, std::int64_t{page}};
    raise_event("on_dialogue", values);
}

bool mods_shop_opened(u8 bank) {
    if (!ext::mods_active())
        return true;
    std::array<verdite::ModEventValue, 2> values{std::int64_t{bank}, false};
    return raise_cancellable("on_shop", values);
}

void mods_player_died() {
    if (!ext::mods_active())
        return;
    raise_event("on_death", {});
}

void mods_new_game() {
    if (ext::mods_active())
        ext::host->load_save_tables("{}");
}

void mods_game_saved(SaveSlot slot) {
    if (!ext::mods_active())
        return;
    const auto result = save_mods_write(slot, ext::host->save_tables());
    if (result != SaveFileResult::Ok && result != SaveFileResult::Unavailable)
        std::printf("[mods] cannot save the mods' save data (%d)\n", static_cast<int>(result));
    std::array<verdite::ModEventValue, 1> values{kf_enum_encode<std::int64_t>(slot)};
    raise_event("on_save", values);
}

void mods_game_loaded(SaveSlot slot) {
    if (!ext::mods_active())
        return;
    std::string json;
    const auto result = save_mods_read(slot, json);
    if (result != SaveFileResult::Ok) {
        json = "{}";
        if (result != SaveFileResult::Missing && result != SaveFileResult::Unavailable)
            std::printf("[mods] cannot read the mods' save data (%d); starting empty\n", static_cast<int>(result));
    }
    ext::host->load_save_tables(json);
    std::array<verdite::ModEventValue, 1> values{kf_enum_encode<std::int64_t>(slot)};
    raise_event("on_load_game", values);
}
} // namespace kf
