#include "mod_automap.h"

#include "internal.hpp"
#include "online_pose.h"
#include <kf/platform/automap.h>
#include <lauxlib.h>
#include <lua.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <optional>
#include <vector>

namespace kf::ext {
namespace {

// AutomapCell, AutomapHazard and AutomapMarkerKind as the mods name them, in their order.
constexpr std::array<const char *, 12> cell_names{
    "void", "wall", "floor", "half_x_ge_z", "half_sum_le", "half_z_ge_x", "half_sum_ge",
    "door", "door_open", "chasm", "harp_bridge", "harp_bridge_raised"};
static_assert(cell_names.size() == static_cast<std::size_t>(AutomapCell::HarpBridgeRaised) + 1);
constexpr std::array<const char *, 4> hazard_names{"none", "pit", "poison", "bottomless"};
static_assert(hazard_names.size() == static_cast<std::size_t>(AutomapHazard::Bottomless) + 1);
constexpr std::array<const char *, 10> marker_names{
    "door", "locked_door", "warp", "save_point", "fountain", "npc", "item", "chest", "enemy", "harp_bridge"};
static_assert(marker_names.size() == static_cast<std::size_t>(AutomapMarkerKind::HarpBridge) + 1);

const AutomapSnapshot &map() { return automap_snapshot(); }

// The cell's index in the snapshot, or nothing off the map.
std::optional<std::size_t> cell_index(lua_State *L) {
    const lua_Integer x = luaL_checkinteger(L, 1);
    const lua_Integer z = luaL_checkinteger(L, 2);
    if (x < 0 || x >= automap_columns || z < 0 || z >= automap_rows)
        return std::nullopt;
    return static_cast<std::size_t>(z * automap_columns + x);
}

template <typename Enum, std::size_t count>
const char *name_of(const std::array<const char *, count> &names, Enum value) {
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : names[0];
}

int automap_cell(lua_State *L) {
    const auto index = cell_index(L);
    lua_pushstring(L, index ? name_of(cell_names, map().cells[*index]) : "void");
    return 1;
}

int automap_hazard(lua_State *L) {
    const auto index = cell_index(L);
    lua_pushstring(L, index ? name_of(hazard_names, map().hazards[*index]) : "none");
    return 1;
}

int automap_seen(lua_State *L) {
    const lua_Integer x = luaL_checkinteger(L, 1);
    const lua_Integer z = luaL_checkinteger(L, 2);
    const bool on_map = x >= 0 && x < automap_columns && z >= 0 && z < automap_rows;
    lua_pushboolean(L, on_map && map().floor != 0 && kf::automap_seen(map().floor, static_cast<int>(x), static_cast<int>(z)));
    return 1;
}

void set_number(lua_State *L, const char *key, lua_Number value) {
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
}

void push_marker(lua_State *L, const AutomapMarker &marker) {
    lua_createtable(L, 0, 5);
    lua_pushstring(L, name_of(marker_names, marker.kind));
    lua_setfield(L, -2, "kind");
    lua_pushinteger(L, marker.label);
    lua_setfield(L, -2, "label");
    lua_createtable(L, static_cast<int>(std::size(marker.items)), 0);
    lua_Integer count = 0;
    for (const u8 item : marker.items)
        if (item != automap_no_item) {
            lua_pushinteger(L, item);
            lua_rawseti(L, -2, ++count);
        }
    lua_setfield(L, -2, "items");
    set_number(L, "x", marker.x);
    set_number(L, "z", marker.z);
}

int automap_markers(lua_State *L) {
    const AutomapSnapshot &snapshot = map();
    const int count = std::clamp(snapshot.marker_count, 0, automap_marker_capacity);
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i) {
        push_marker(L, snapshot.markers[static_cast<std::size_t>(i)]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

void push_map_floor(lua_State *L) { lua_pushinteger(L, map().floor); }
void push_owns_map(lua_State *L) { lua_pushboolean(L, map().owns_map); }
void push_serial(lua_State *L) { lua_pushinteger(L, map().serial); }
void push_player_x(lua_State *L) { lua_pushnumber(L, map().player_x); }
void push_player_z(lua_State *L) { lua_pushnumber(L, map().player_z); }
void push_player_yaw(lua_State *L) { lua_pushnumber(L, map().player_yaw); }

int online_players_list(lua_State *L) {
    const std::vector<OnlinePose> players = online_players();
    lua_createtable(L, static_cast<int>(players.size()), 0);
    lua_Integer count = 0;
    for (const OnlinePose &pose : players) {
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, pose.floor);
        lua_setfield(L, -2, "floor");
        set_number(L, "x", pose.x);
        set_number(L, "z", pose.z);
        set_number(L, "yaw", pose.yaw);
        lua_rawseti(L, -2, ++count);
    }
    return 1;
}

} // namespace

verdite::ModTable automap_table() {
    using verdite::ModFunction;
    return {.path = "kf.automap",
            .doc = "The floor as the automap sees it, published each update. Cells are 1 unit; x east, z north. "
                   "Off the map a cell is \"void\", with no hazard, and not seen.",
            .functions =
                {ModFunction{"cell", "fun(x: integer, z: integer): string",
                             "\"void\", \"wall\", \"floor\", \"half_x_ge_z\", \"half_sum_le\", \"half_z_ge_x\", "
                             "\"half_sum_ge\", \"door\", \"door_open\", \"chasm\", \"harp_bridge\" or "
                             "\"harp_bridge_raised\".",
                             automap_cell},
                 {"hazard", "fun(x: integer, z: integer): \"none\"|\"pit\"|\"poison\"|\"bottomless\"", "",
                  automap_hazard},
                 {"seen", "fun(x: integer, z: integer): boolean",
                  "The player has seen the cell on this floor; kept with each save.", automap_seen},
                 {"markers",
                  "fun(): {kind: string, label: integer, items: integer[], x: number, z: number}[]",
                  "Doors, warps, save points, fountains, people, items, chests, enemies and Harp bridges, each "
                  "at its point in cells.",
                  automap_markers}},
            .fields = {{"floor", "integer", "1 to 5; 0 when no floor is loaded.", push_map_floor, nullptr},
                       {"owns_map", "boolean", "The Keeper's or Magician's map is in the pack.", push_owns_map,
                        nullptr},
                       {"serial", "integer", "Counts up on every publish.", push_serial, nullptr},
                       {"player_x", "number", "In cells.", push_player_x, nullptr},
                       {"player_z", "number", "In cells.", push_player_z, nullptr},
                       {"player_yaw", "number", "Radians; 0 faces north (+z), increasing turns west (-x).",
                        push_player_yaw, nullptr}}};
}

verdite::ModTable online_table() {
    using verdite::ModFunction;
    return {.path = "kf.online",
            .doc = "The online room.",
            .functions = {ModFunction{"players", "fun(): {floor: integer, x: number, z: number, yaw: number}[]",
                                      "The other players in the room, in cells and radians as kf.automap.",
                                      online_players_list}},
            .fields = {}};
}

} // namespace kf::ext
