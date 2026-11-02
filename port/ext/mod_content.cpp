#include "mod_content.h"

#include "internal.hpp"
#include "mod_content_save.h"
#include "mod_floor_check.h"

#include <verdite/content_ids.h>

#include <kf/game/game.h>
#include <kf/game/menu.h>
#include <kf/game/player.h>
#include <kf/game/save.h>
#include <kf/lib/floor.h>
#include <kf/lib/item.h>
#include <kf/lib/map.h>
#include <kf/platform/content.h>
#include <kf/platform/coop.h>
#include <kf/platform/files.h>
#include <kf/platform/saves.h>

#include <lauxlib.h>
#include <lua.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace kf::ext {
namespace {

constexpr std::size_t name_chars_max = 22;   // as the longest of the disc's names
constexpr std::size_t description_chars_max = 200;
constexpr verdite::ContentIdRange item_ids{KF_ITEM_MOD_FIRST, KF_ITEM_ID_COUNT - KF_ITEM_MOD_FIRST};
constexpr verdite::ContentIdRange floor_ids{KF_FLOOR_MOD_FIRST, KF_FLOOR_MOD_COUNT};
constexpr std::uint8_t fallback_model_item = 41;

using FileBytes = std::shared_ptr<const std::vector<std::uint8_t>>;

struct ModItem {
    std::string owner;
    // ExtraItem holds views of these; a map's values do not move.
    std::string english, japanese, description;
    ExtraItem row;
};

struct ModFloor {
    std::string owner;       // empty once the mod is gone: the files stay, for a player standing there
    FileBytes tim, mixa, mixb, seq;
    std::uint32_t hash = 0;
    ModFloorEntry entry;
};

struct ContentState {
    std::map<std::uint8_t, ModItem> items;
    std::map<std::uint8_t, ModFloor> floors;
    std::array<std::uint32_t, KF_FLOOR_MOD_COUNT> kept_hashes{};   // of records whose mod is not here
    std::optional<ModFloorNeeds> needs;
    std::optional<verdite::ContentIds> ids;
};
ContentState content;

verdite::ContentIds &content_ids() {
    if (!content.ids)
        content.ids.emplace(mods_dir() / ".content_ids");
    return *content.ids;
}

// A key is the mod's: two mods' "ring"s are two items.
std::string owned_key(lua_State *L, std::string_view key) {
    return std::string(verdite::mod_id(L)) + "/" + std::string(key);
}

std::uint32_t fnv1a(std::span<const std::uint8_t> bytes, std::uint32_t hash) {
    for (const std::uint8_t byte : bytes)
        hash = (hash ^ byte) * 16777619u;
    return hash;
}

std::optional<std::vector<std::uint8_t>> disc_file(const std::string &path) {
    DataFile file{};
    if (data_file_open_disc(&file, path.c_str()) != FileResult::Ok)
        return std::nullopt;
    std::vector<std::uint8_t> bytes;
    const FileResult result = data_file_read(&file, bytes, disc_import_limit);
    data_file_close(&file);
    if (result != FileResult::Ok)
        return std::nullopt;
    return bytes;
}

// What the disc's five floors make the game expect, read once.
const ModFloorNeeds *floor_needs() {
    if (content.needs)
        return &*content.needs;
    ModFloorNeeds needs;
    for (int floor = 1; floor <= KF_MAP_SAVED_FLOOR_COUNT; ++floor) {
        const std::string folder = "KF/B" + std::to_string(floor) + "/";
        const auto tim = disc_file(folder + "MIX.TIM");
        const auto mixa = disc_file(folder + "MIXA.DAT");
        const auto mixb = disc_file(folder + "MIXB.DAT");
        const auto seq = disc_file(folder + "SND0.SEQ");
        if (!tim || !mixa || !mixb || !seq) {
            std::printf("[mods] cannot read the disc's floor %d\n", floor);
            return nullptr;
        }
        if (const auto refusal = mod_floor_needs_add(needs, {*tim, *mixa, *mixb, *seq})) {
            std::printf("[mods] the disc's floor %d: %s\n", floor, refusal->c_str());
            return nullptr;
        }
    }
    const auto com = disc_file("KF/COM/COM.DAT");
    if (!com || !mod_floor_needs_read_objects(needs, *com))
        return nullptr;
    content.needs = needs;
    return &*content.needs;
}

// ---- reading a Lua table's fields ----

std::string string_field(lua_State *L, int table, const char *name, std::size_t max_chars) {
    lua_getfield(L, table, name);
    std::size_t size = 0;
    const char *text = lua_tolstring(L, -1, &size);
    if (!text || size == 0)
        luaL_error(L, "kf.content: '%s' must be a string", name);
    if (size > max_chars)
        luaL_error(L, "kf.content: '%s' is longer than %d characters", name, static_cast<int>(max_chars));
    std::string out(text, size);
    lua_pop(L, 1);
    return out;
}

std::string optional_string_field(lua_State *L, int table, const char *name, std::size_t max_chars) {
    lua_getfield(L, table, name);
    const bool present = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return present ? string_field(L, table, name, max_chars) : std::string();
}

std::optional<lua_Integer> optional_integer_field(lua_State *L, int table, const char *name, lua_Integer min, lua_Integer max) {
    lua_getfield(L, table, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return std::nullopt;
    }
    if (!lua_isinteger(L, -1))
        luaL_error(L, "kf.content: '%s' must be an integer", name);
    const lua_Integer value = lua_tointeger(L, -1);
    if (value < min || value > max)
        luaL_error(L, "kf.content: '%s' %I is out of range (%I to %I)", name, value, min, max);
    lua_pop(L, 1);
    return value;
}

// ---- kf.content.item ----

// What the menus can spell: capitals, digits and the marks the disc's names use.
void check_item_name(lua_State *L, const std::string &name) {
    const bool spellable = std::ranges::all_of(name, [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '\'' || c == '-' || c == '.';
    });
    if (!spellable)
        luaL_error(L, "kf.content: an item's name is capitals, digits, spaces and ' - . only: '%s'", name.c_str());
}

void lay_price(lua_State *L, int table, const char *name, std::array<std::array<u16, KF_ITEM_SHOP_COUNT>, KF_ITEM_ID_COUNT> &prices,
               std::uint8_t id) {
    if (const auto price = optional_integer_field(L, table, name, 0, 65535))
        prices[id].fill(static_cast<u16>(*price));
}

int content_item(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const std::string key = string_field(L, 1, "key", 40);
    const std::string english = string_field(L, 1, "name", name_chars_max);
    check_item_name(L, english);
    const std::string japanese = optional_string_field(L, 1, "japanese", name_chars_max * 3);
    const std::string description = optional_string_field(L, 1, "description", description_chars_max);
    const auto model = optional_integer_field(L, 1, "model_item", 0, KF_ITEM_COUNT - 1);
    const auto model_item = static_cast<std::uint8_t>(model.value_or(fallback_model_item));
    if (!item_english_name(model_item))
        luaL_error(L, "kf.content: model_item %d is not a disc item", static_cast<int>(model_item));
    const auto assigned = content_ids().assign("item", owned_key(L, key), item_ids);
    if (!assigned)
        return luaL_error(L, "kf.content: no item id for '%s': the key must be a plain name, and %d ids are all there are", key.c_str(),
                          static_cast<int>(item_ids.count));
    const auto id = static_cast<std::uint8_t>(*assigned);
    ModItem &item = content.items[id];
    item.owner = std::string(verdite::mod_id(L));
    item.english = english;
    item.japanese = japanese.empty() ? english : japanese;
    item.description = description;
    item.row = ExtraItem{.id = id, .english = item.english, .japanese = item.japanese, .description = item.description, .model_item = model_item};
    lay_price(L, 1, "buy", item_buy_prices, id);
    lay_price(L, 1, "sell", item_sell_prices, id);
    lua_pushinteger(L, id);
    return 1;
}

// ---- kf.content.floor ----

std::string folder_path(lua_State *L, int table) {
    std::string folder = string_field(L, table, "folder", 200);
    if (folder.back() != '/')
        folder += '/';
    return folder;
}

FileBytes read_floor_file(lua_State *L, const std::string &folder, const char *name, std::size_t limit) {
    auto bytes = verdite::mod_file_bytes(L, folder + name, limit);
    if (!bytes) {
        luaL_error(L, "kf.content: %s%s is missing, or larger than the disc's largest", folder.c_str(), name);
        return nullptr;
    }
    return std::make_shared<const std::vector<std::uint8_t>>(std::move(*bytes));
}

std::uint32_t floor_files_hash(const ModFloor &floor) {
    std::uint32_t hash = 2166136261u;
    for (const FileBytes *file : {&floor.tim, &floor.mixa, &floor.mixb, &floor.seq})
        hash = fnv1a(**file, hash);
    return hash;
}

int content_floor(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const std::string key = string_field(L, 1, "key", 40);
    const std::string folder = folder_path(L, 1);
    const auto entry_x = optional_integer_field(L, 1, "entry_x", 0, KF_MAP_COLUMNS - 1);
    const auto entry_z = optional_integer_field(L, 1, "entry_z", 0, KF_MAP_ROWS - 1);
    if (!entry_x || !entry_z)
        return luaL_error(L, "kf.content: a floor needs entry_x and entry_z, the cell a warp with none arrives at");
    const ModFloorNeeds *needs = floor_needs();
    if (!needs)
        return luaL_error(L, "kf.content: the disc's own floors cannot be read to check this one against");

    ModFloor floor;
    floor.owner = std::string(verdite::mod_id(L));
    floor.tim = read_floor_file(L, folder, "MIX.TIM", needs->tim_bytes);
    floor.mixa = read_floor_file(L, folder, "MIXA.DAT", needs->mixa_bytes);
    floor.mixb = read_floor_file(L, folder, "MIXB.DAT", needs->mixb_bytes);
    floor.seq = read_floor_file(L, folder, "SND0.SEQ", needs->seq_bytes);
    floor.entry = {static_cast<u8>(entry_x.value_or(0)), static_cast<u8>(entry_z.value_or(0))};
    if (const auto refusal = mod_floor_refusal({*floor.tim, *floor.mixa, *floor.mixb, *floor.seq}, *needs, floor.entry.x, floor.entry.z))
        return luaL_error(L, "kf.content: floor '%s': %s", key.c_str(), refusal->c_str());
    floor.hash = floor_files_hash(floor);

    const auto assigned = content_ids().assign("floor", owned_key(L, key), floor_ids);
    if (!assigned)
        return luaL_error(L, "kf.content: no floor number for '%s': the key must be a plain name, and %d floors are all there are", key.c_str(),
                          static_cast<int>(floor_ids.count));
    const auto id = static_cast<std::uint8_t>(*assigned);
    const auto known = content.floors.find(id);
    const bool changed = known != content.floors.end() && known->second.hash != floor.hash;
    // The records in hand were written against the old files, and the game stops on a saved object it no longer finds.
    if (changed && kf_enum_encode<int>(player_state.progress_state.current_floor) == id)
        return luaL_error(L, "kf.content: floor '%s' changed while the player is on it; leave it first", key.c_str());
    if (changed || content.kept_hashes[id - KF_FLOOR_MOD_FIRST] != floor.hash)
        map_mod_floor_records[id - KF_FLOOR_MOD_FIRST] = {};
    content.kept_hashes[id - KF_FLOOR_MOD_FIRST] = floor.hash;
    content.floors[id] = std::move(floor);
    lua_pushinteger(L, id);
    return 1;
}

} // namespace

std::vector<verdite::ModFunction> mod_content_functions() {
    return {
        verdite::ModFunction{"item", "fun(row: table): integer",
                             "Adds an item: key, name (capitals), optional japanese, description, model_item (a disc item's id), buy and sell. Its id.",
                             content_item},
        {"floor", "fun(row: table): integer",
         "Adds a floor from a folder of the mod's holding MIX.TIM, MIXA.DAT, MIXB.DAT and SND0.SEQ: key, folder, entry_x, entry_z. Its number, for kf.warp.",
         content_floor},
    };
}

void mod_content_forget(std::string_view id) {
    std::erase_if(content.items, [id](const auto &entry) { return entry.second.owner == id; });
    for (auto &[number, floor] : content.floors)
        if (floor.owner == id)
            floor.owner.clear();
}

const ExtraItem *mod_content_item(std::uint8_t id) {
    if (coop_world_in_play())
        return nullptr;
    const auto found = content.items.find(id);
    return found == content.items.end() ? nullptr : &found->second.row;
}

FileBytes mod_content_floor_file(const char *path) {
    constexpr std::string_view prefix = "KF/B";
    const std::string_view name(path);
    if (!name.starts_with(prefix) || name.size() < prefix.size() + 3 || name[prefix.size() + 1] != '/')
        return nullptr;
    const char folder = name[prefix.size()];
    if (folder < 'A' || folder >= 'A' + KF_FLOOR_MOD_COUNT)
        return nullptr;
    const auto found = content.floors.find(static_cast<std::uint8_t>(KF_FLOOR_MOD_FIRST + (folder - 'A')));
    if (found == content.floors.end())
        return nullptr;
    const std::string_view file = name.substr(prefix.size() + 2);
    if (file == "MIX.TIM")
        return found->second.tim;
    if (file == "MIXA.DAT")
        return found->second.mixa;
    if (file == "MIXB.DAT")
        return found->second.mixb;
    if (file == "SND0.SEQ")
        return found->second.seq;
    return nullptr;
}

const ModFloorEntry *mod_content_floor_held(std::uint8_t floor) {
    const auto found = content.floors.find(floor);
    return found == content.floors.end() ? nullptr : &found->second.entry;
}

bool mod_content_floor_offered(std::uint8_t floor) {
    const auto found = content.floors.find(floor);
    return found != content.floors.end() && !found->second.owner.empty();
}

void mod_content_save_write(SaveSlot slot) {
    ContentSave save;
    for (std::size_t bank = 0; bank < KF_ITEM_STOCK_BANK_COUNT; ++bank)
        for (int id = KF_ITEM_MOD_FIRST; id < KF_ITEM_ID_COUNT; ++id)
            if (const std::uint8_t count = item_stock[bank][id])
                save.stock.push_back({static_cast<std::uint8_t>(bank), static_cast<std::uint8_t>(id), std::min<std::uint8_t>(count, KF_ITEM_STACK_CAPACITY)});
    for (std::size_t i = 0; i < map_mod_floor_records.size(); ++i) {
        const auto &records = map_mod_floor_records[i].records;
        if (records[0] == 0)
            continue;
        ContentFloorRow row;
        row.floor = static_cast<std::uint8_t>(KF_FLOOR_MOD_FIRST + i);
        row.files_hash = content.kept_hashes[i];
        std::ranges::copy(records, row.records.begin());
        save.floors.push_back(row);
    }
    const auto bytes = content_save_encode(save);
    const auto result = save_content_write(slot, bytes);
    if (result != SaveFileResult::Ok && result != SaveFileResult::Unavailable)
        std::printf("[mods] cannot save the mods' items and floors (%d)\n", static_cast<int>(result));
}

void mod_content_save_read(SaveSlot slot) {
    for (auto &stock : item_stock)
        std::fill(stock.begin() + KF_ITEM_COUNT, stock.end(), std::uint8_t{0});
    map_mod_floor_records = {};
    std::vector<std::uint8_t> bytes;
    const auto result = save_content_read(slot, bytes);
    if (result != SaveFileResult::Ok) {
        if (result != SaveFileResult::Missing && result != SaveFileResult::Unavailable)
            std::printf("[mods] cannot read the mods' items and floors (%d)\n", static_cast<int>(result));
        return;
    }
    const auto save = content_save_decode(bytes);
    if (!save) {
        std::printf("[mods] the file of the mods' items and floors is damaged; they are left out\n");
        return;
    }
    for (const ContentStockRow &row : save->stock)
        item_stock[row.bank][row.item] = row.count;
    for (const ContentFloorRow &row : save->floors) {
        const std::size_t index = row.floor - KF_FLOOR_MOD_FIRST;
        KfMapSavedFloor floor;
        std::ranges::copy(row.records, floor.records.begin());
        if (!save_system_floor_record_valid(floor, &map_object_state.definitions))
            continue;
        const auto held = content.floors.find(row.floor);
        // A floor whose files changed since this save starts afresh; the game stops on a saved object it no longer finds.
        if (held != content.floors.end() && held->second.hash != row.files_hash)
            continue;
        content.kept_hashes[index] = row.files_hash;
        map_mod_floor_records[index] = floor;
    }
}

} // namespace kf::ext

namespace kf {

bool mod_floor_loadable(u8 floor) {
    return ext::mod_content_floor_held(floor) != nullptr;
}

ModFloorEntry mod_floor_entry(u8 floor) {
    const ModFloorEntry *entry = ext::mod_content_floor_held(floor);
    return entry ? *entry : ModFloorEntry{};
}

} // namespace kf
