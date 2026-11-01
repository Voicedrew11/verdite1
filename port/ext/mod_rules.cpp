// kf.rules: the fields each rule table offers, the Lua calls, and laying the running mods' values
// over the originals the game loaded. A new field is a row in its table's field list.
#include "mod_rules.h"
#include "mod_rule_values.h"

#include <kf/platform/prelude.h>
#include <kf/game/actor.h>
#include <kf/game/equipment.h>
#include <kf/game/menu.h>
#include <kf/game/player.h>
#include <kf/lib/item_types.h>
#include <kf/lib/player_stats_types.h>
#include <kf/platform/mod_events.h>
#include <lauxlib.h>
#include <lua.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace kf::ext {
namespace {

constexpr std::size_t table_count = 5;
constexpr int first_floor = 1, last_floor = 5;
constexpr int last_growth_level = KF_PLAYER_LEVEL_GROWTH_COUNT; // level 1 is the table's first row
constexpr int first_shop = kf_enum_encode<int>(KF_ITEM_STOCK_FIRST_SHOP);
constexpr int last_shop = first_shop + KF_ITEM_SHOP_COUNT - 1;
constexpr int last_weapon = kf_enum_encode<int>(KF_ITEM_IRON_MASK) - 1;
constexpr int first_armour = kf_enum_encode<int>(KF_ITEM_IRON_MASK);
constexpr int last_armour = kf_enum_encode<int>(KF_ITEM_FEATHER_BOOTS);
constexpr std::int64_t stat_max = 999; // attack and defense, as the player's are held

using Prices = std::array<std::array<u16, KF_ITEM_SHOP_COUNT>, KF_ITEM_COUNT>;

// The tables as the game loaded them, before any mod's values.
struct Originals {
    KfActorDefinitionTable definitions{};
    int definitions_floor = 0; // the floor they belong to; 0 before any
    std::array<KfPlayerLevelGrowth, KF_PLAYER_LEVEL_GROWTH_COUNT> growth{};
    Prices buy_prices{};
    Prices sell_prices{};
    KfWeaponTable weapons{};
    KfArmorTable armour{};
    std::array<bool, table_count> loaded{};
};

struct Rules {
    Originals originals;
    std::map<std::string, RuleValues, std::less<>> by_mod;
    std::uint64_t generation = 0; // counts changes to by_mod
    RuleValues laid;              // what is over the originals now
    // What laid was combined from: nothing while only the originals are laid.
    std::optional<std::vector<std::string>> laid_order;
    std::uint64_t laid_generation = 0;
};
Rules rules;

// ---------- the fields ----------
struct RuleField {
    std::string_view name; // "hp", or "attack.cutting" for a field of the attack table
    std::int64_t min;
    std::int64_t max;
    void (*write)(const RuleKey &key, std::int64_t value); // into the game's table
    void (*restore)(const RuleKey &key);                   // the original back into it
    std::int64_t (*original)(const RuleKey &key);
};

// A field read and written as an integer: game and original give its place in the game's table
// and in the originals.
template <auto game, auto original>
RuleField integer_rule(std::string_view name, std::int64_t min, std::int64_t max) {
    return {name,
            min,
            max,
            [](const RuleKey &key, std::int64_t value) {
                auto &target = game(key);
                target = static_cast<std::remove_reference_t<decltype(target)>>(value);
            },
            [](const RuleKey &key) { game(key) = original(key); },
            [](const RuleKey &key) -> std::int64_t { return original(key); }};
}

std::size_t kind_of(const RuleKey &key) {
    return static_cast<std::size_t>(key.second);
}
std::size_t growth_row(const RuleKey &key) {
    return static_cast<std::size_t>(key.first - 1);
}
std::size_t price_item(const RuleKey &key) {
    return static_cast<std::size_t>(key.first);
}
std::size_t price_shop(const RuleKey &key) {
    return static_cast<std::size_t>(key.second - first_shop);
}
std::size_t weapon_row(const RuleKey &key) {
    return static_cast<std::size_t>(key.first);
}
std::size_t armour_row(const RuleKey &key) {
    return static_cast<std::size_t>(key.first - first_armour);
}

// member gives the field within a record: [](auto &record) -> auto & { return record.field; }.
template <auto member>
RuleField monster_rule(std::string_view name, std::int64_t min, std::int64_t max) {
    return integer_rule<[](const RuleKey &key) -> auto & {
        return member(actor_state.definitions.entries[kind_of(key)]);
    },
                        [](const RuleKey &key) -> auto & {
                            return member(rules.originals.definitions.entries[kind_of(key)]);
                        }>(name, min, max);
}
template <auto member>
RuleField level_rule(std::string_view name, std::int64_t min, std::int64_t max) {
    return integer_rule<[](const RuleKey &key) -> auto & {
        return member(player_level_growth_table[growth_row(key)]);
    },
                        [](const RuleKey &key) -> auto & {
                            return member(rules.originals.growth[growth_row(key)]);
                        }>(name, min, max);
}
template <auto member>
RuleField weapon_rule(std::string_view name, std::int64_t min, std::int64_t max) {
    return integer_rule<[](const RuleKey &key) -> auto & {
        return member(weapon_records.entries[weapon_row(key)]);
    },
                        [](const RuleKey &key) -> auto & {
                            return member(rules.originals.weapons.entries[weapon_row(key)]);
                        }>(name, min, max);
}
template <auto member>
RuleField armour_rule(std::string_view name, std::int64_t min, std::int64_t max) {
    return integer_rule<[](const RuleKey &key) -> auto & {
        return member(armor_records.entries[armour_row(key)]);
    },
                        [](const RuleKey &key) -> auto & {
                            return member(rules.originals.armour.entries[armour_row(key)]);
                        }>(name, min, max);
}
// prices gives the table: item_buy_prices and the original buy prices, or the sell prices.
template <auto game, auto original> RuleField price_rule(std::string_view name) {
    return integer_rule<
        [](const RuleKey &key) -> auto & { return game()[price_item(key)][price_shop(key)]; },
        [](const RuleKey &key) -> auto & { return original()[price_item(key)][price_shop(key)]; }>(
        name, 0, 65535);
}

// A definition's drop: -1 for none, which the game keeps as KF_OBJECT_NONE (or 99, disabled).
std::int64_t drop_item_of(KfObjectId drop) {
    const auto id = kf_enum_encode<std::int64_t>(drop);
    return id < KF_ITEM_COUNT ? id : -1;
}
RuleField drop_item_rule() {
    return {
        "drop_item",
        -1,
        KF_ITEM_COUNT - 1,
        [](const RuleKey &key, std::int64_t value) {
            actor_state.definitions.entries[kind_of(key)].action_parameters.drop_object =
                value < 0 ? KF_OBJECT_NONE : kf_enum_decode<KfObjectId>(static_cast<u8>(value));
        },
        [](const RuleKey &key) {
            actor_state.definitions.entries[kind_of(key)].action_parameters.drop_object =
                rules.originals.definitions.entries[kind_of(key)].action_parameters.drop_object;
        },
        [](const RuleKey &key) {
            return drop_item_of(
                rules.originals.definitions.entries[kind_of(key)].action_parameters.drop_object);
        }};
}

const std::array<RuleField, 13> monster_fields = {
    monster_rule<[](auto &d) -> auto & { return d.initial_health; }>("hp", 1, 65535),
    monster_rule<[](auto &d) -> auto & { return d.experience_reward; }>("experience", 0, 32767),
    monster_rule<[](auto &d) -> auto & { return d.gold_drop_limit; }>("gold", 0, 65535),
    drop_item_rule(),
    monster_rule<[](auto &d) -> auto & { return d.action_parameters.drop_chance; }>("drop_chance",
                                                                                    0, 255),
    monster_rule<[](auto &d) -> auto & {
        return d.attack_components[KF_COMBAT_COMPONENT_CUTTING];
    }>("attack.cutting", 0, stat_max),
    monster_rule<[](auto &d) -> auto & {
        return d.attack_components[KF_COMBAT_COMPONENT_STRIKING];
    }>("attack.striking", 0, stat_max),
    monster_rule<[](auto &d) -> auto & {
        return d.attack_components[KF_COMBAT_COMPONENT_PIERCING];
    }>("attack.piercing", 0, stat_max),
    monster_rule<[](auto &d) -> auto & { return d.defenses[KF_COMBAT_COMPONENT_CUTTING]; }>(
        "defense.cutting", 0, stat_max),
    monster_rule<[](auto &d) -> auto & { return d.defenses[KF_COMBAT_COMPONENT_STRIKING]; }>(
        "defense.striking", 0, stat_max),
    monster_rule<[](auto &d) -> auto & { return d.defenses[KF_COMBAT_COMPONENT_PIERCING]; }>(
        "defense.piercing", 0, stat_max),
    monster_rule<[](auto &d) -> auto & { return d.defenses[KF_COMBAT_COMPONENT_HOLY]; }>(
        "defense.holy", 0, stat_max),
    monster_rule<[](auto &d) -> auto & { return d.defenses[KF_COMBAT_COMPONENT_FIRE]; }>(
        "defense.fire", 0, stat_max),
};

const std::size_t drop_item_field = static_cast<std::size_t>(
    std::ranges::find(monster_fields, std::string_view{"drop_item"}, &RuleField::name) -
    monster_fields.begin());

const std::array<RuleField, 5> level_fields = {
    level_rule<[](auto &g) -> auto & { return g.maximum_hp; }>("max_hp", 1, KF_PLAYER_VITAL_MAX),
    level_rule<[](auto &g) -> auto & { return g.maximum_mp; }>("max_mp", 0, KF_PLAYER_VITAL_MAX),
    level_rule<[](auto &g) -> auto & { return g.physical_power_step; }>("power_step", 0,
                                                                        KF_PLAYER_POWER_MAX),
    level_rule<[](auto &g) -> auto & { return g.magic_step; }>("magic_step", 0,
                                                               KF_PLAYER_POWER_MAX),
    level_rule<[](auto &g) -> auto & { return g.experience_threshold; }>("experience", 1,
                                                                         KF_PLAYER_EXPERIENCE_MAX),
};

const std::array<RuleField, 2> price_fields = {
    price_rule<[]() -> auto & { return item_buy_prices; },
               []() -> auto & { return rules.originals.buy_prices; }>("buy"),
    price_rule<[]() -> auto & { return item_sell_prices; },
               []() -> auto & { return rules.originals.sell_prices; }>("sell"),
};

const std::array<RuleField, 2> weapon_fields = {
    weapon_rule<[](auto &w) -> auto & { return w.hp_regen_interval; }>("hp_every", 0, 65535),
    weapon_rule<[](auto &w) -> auto & { return w.mp_regen_interval; }>("mp_every", 0, 65535),
};

const std::array<RuleField, 2> armour_fields = {
    armour_rule<[](auto &a) -> auto & { return a.hp_regen_interval; }>("hp_every", 0, 65535),
    armour_rule<[](auto &a) -> auto & { return a.hp_drain_interval; }>("hp_drain_every", 0, 65535),
};

// ---------- the tables ----------
std::vector<RuleKey> monster_keys() {
    std::vector<RuleKey> keys;
    if (rules.originals.definitions_floor == 0)
        return keys;
    for (int kind = 0; kind < KF_ACTOR_DEFINITION_COUNT; ++kind)
        keys.push_back({RulesTable::ActorDefinitions, rules.originals.definitions_floor, kind});
    return keys;
}
std::vector<RuleKey> level_keys() {
    std::vector<RuleKey> keys;
    for (int level = 1; level <= last_growth_level; ++level)
        keys.push_back({RulesTable::LevelGrowth, level, 0});
    return keys;
}
std::vector<RuleKey> price_keys() {
    std::vector<RuleKey> keys;
    for (int item = 0; item < KF_ITEM_COUNT; ++item)
        for (int shop = first_shop; shop <= last_shop; ++shop)
            keys.push_back({RulesTable::ShopPrices, item, shop});
    return keys;
}
std::vector<RuleKey> weapon_keys() {
    std::vector<RuleKey> keys;
    for (int item = 0; item <= last_weapon; ++item)
        keys.push_back({RulesTable::WeaponRecords, item, 0});
    return keys;
}
std::vector<RuleKey> armour_keys() {
    std::vector<RuleKey> keys;
    for (int item = first_armour; item <= last_armour; ++item)
        keys.push_back({RulesTable::ArmourRecords, item, 0});
    return keys;
}

// The floor's definitions are the floor's; a load of another floor replaces them.
void capture_definitions() {
    const int floor = kf_enum_encode<int>(player_state.progress_state.current_floor);
    rules.originals.definitions = actor_state.definitions;
    rules.originals.definitions_floor = floor >= first_floor && floor <= last_floor ? floor : 0;
}
void capture_growth() {
    rules.originals.growth = player_level_growth_table;
}
void capture_prices() {
    rules.originals.buy_prices = item_buy_prices;
    rules.originals.sell_prices = item_sell_prices;
}
void capture_weapons() {
    rules.originals.weapons = weapon_records;
}
void capture_armour() {
    rules.originals.armour = armor_records;
}

struct RuleTableSpec {
    std::span<const RuleField> fields;
    std::vector<RuleKey> (*keys_loaded)(); // the records the game's table holds now
    void (*capture)();                     // the game's table, just loaded, as the originals
};

// In RulesTable's order.
const std::array<RuleTableSpec, table_count> table_specs = {{
    {monster_fields, monster_keys, capture_definitions},
    {level_fields, level_keys, capture_growth},
    {price_fields, price_keys, capture_prices},
    {weapon_fields, weapon_keys, capture_weapons},
    {armour_fields, armour_keys, capture_armour},
}};

std::size_t table_index(RulesTable table) {
    return kf_enum_encode<std::size_t>(table);
}
const RuleTableSpec &spec_of(RulesTable table) {
    return table_specs.at(table_index(table));
}

std::optional<std::int64_t> laid_value(const RuleFields *set, std::size_t field) {
    if (!set)
        return std::nullopt;
    const auto found = set->find(field);
    return found == set->end() ? std::nullopt : std::optional{found->second};
}

// Each field of each record the game holds: the laid value where a mod set one, else the original.
void lay_table(RulesTable table) {
    if (!rules.originals.loaded.at(table_index(table)))
        return;
    const RuleTableSpec &spec = spec_of(table);
    for (const RuleKey &key : spec.keys_loaded()) {
        const RuleFields *set = rules.laid.find(key);
        for (std::size_t i = 0; i < spec.fields.size(); ++i) {
            if (const auto value = laid_value(set, i))
                spec.fields[i].write(key, *value);
            else
                spec.fields[i].restore(key);
        }
    }
}

void lay_all(RuleValues values) {
    rules.laid = std::move(values);
    for (std::size_t table = 0; table < table_count; ++table)
        lay_table(kf_enum_decode<RulesTable>(static_cast<u8>(table)));
}

// ---------- the Lua calls ----------
const RuleField *find_field(std::span<const RuleField> fields, std::string_view name) {
    const auto found =
        std::ranges::find_if(fields, [name](const RuleField &field) { return field.name == name; });
    return found == fields.end() ? nullptr : &*found;
}

bool is_group(std::span<const RuleField> fields, std::string_view name) {
    return std::ranges::any_of(fields, [name](const RuleField &field) {
        return field.name.size() > name.size() && field.name.starts_with(name) &&
               field.name[name.size()] == '.';
    });
}

[[noreturn]] void field_error(lua_State *L, int argument, const char *message) {
    luaL_argerror(L, argument, message);
    std::abort(); // luaL_argerror does not return
}

// The value on top of the stack as the named field's, checked against its range.
void read_field(lua_State *L, int argument, std::span<const RuleField> fields,
                const std::string &name, RuleFields &out) {
    const RuleField *field = find_field(fields, name);
    if (!field)
        field_error(L, argument, lua_pushfstring(L, "there is no field '%s'", name.c_str()));
    int is_integer = 0;
    const lua_Integer value = lua_tointegerx(L, -1, &is_integer);
    if (!is_integer)
        field_error(L, argument, lua_pushfstring(L, "field '%s' must be an integer", name.c_str()));
    if (value < field->min || value > field->max)
        field_error(L, argument,
                    lua_pushfstring(L, "%s %I is out of range (%I to %I)", name.c_str(), value,
                                    static_cast<lua_Integer>(field->min),
                                    static_cast<lua_Integer>(field->max)));
    out[static_cast<std::size_t>(field - fields.data())] = value;
}

// The fields of the table at index, under prefix ("attack.") when it is a group's.
void read_fields(lua_State *L, int argument, int index, std::span<const RuleField> fields,
                 const std::string &prefix, RuleFields &out) {
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING)
            field_error(L, argument, "field names must be strings");
        const std::string name = prefix + lua_tostring(L, -2);
        if (lua_istable(L, -1) && prefix.empty() && is_group(fields, name))
            read_fields(L, argument, lua_gettop(L), fields, name + ".", out);
        else if (is_group(fields, name))
            field_error(L, argument,
                        lua_pushfstring(L, "field '%s' must be a table", name.c_str()));
        else
            read_field(L, argument, fields, name, out);
        lua_pop(L, 1);
    }
}

RuleFields check_fields(lua_State *L, int argument, RulesTable table) {
    luaL_checktype(L, argument, LUA_TTABLE);
    RuleFields fields;
    read_fields(L, argument, lua_absindex(L, argument), spec_of(table).fields, "", fields);
    return fields;
}

int check_in(lua_State *L, int argument, const char *what, int min, int max) {
    const lua_Integer value = luaL_checkinteger(L, argument);
    if (value < min || value > max)
        field_error(L, argument,
                    lua_pushfstring(L, "%s %I is not %d to %d", what, value, min, max));
    return static_cast<int>(value);
}

RuleKey check_monster(lua_State *L) {
    const int floor = check_in(L, 1, "floor", first_floor, last_floor);
    return {RulesTable::ActorDefinitions, floor,
            check_in(L, 2, "kind", 0, KF_ACTOR_DEFINITION_COUNT - 1)};
}
RuleKey check_level(lua_State *L) {
    return {RulesTable::LevelGrowth, check_in(L, 1, "level", 1, last_growth_level), 0};
}
RuleKey check_price(lua_State *L) {
    const int item = check_in(L, 1, "item", 0, KF_ITEM_COUNT - 1);
    return {RulesTable::ShopPrices, item, check_in(L, 2, "shop", first_shop, last_shop)};
}
// A weapon's record or an armour's (not an accessory's, which has none).
RuleKey check_regen(lua_State *L) {
    const int item = check_in(L, 1, "item", 0, KF_ITEM_COUNT - 1);
    if (item <= last_weapon)
        return {RulesTable::WeaponRecords, item, 0};
    if (item >= first_armour && item <= last_armour)
        return {RulesTable::ArmourRecords, item, 0};
    field_error(
        L, 1,
        lua_pushfstring(L, "item %d has no weapon or armour record (0 to %d)", item, last_armour));
}

// Stores the calling mod's values for a record; they are laid when the callback returns.
int set_rule(lua_State *L, const RuleKey &key, int values_argument) {
    RuleFields fields = check_fields(L, values_argument, key.table);
    // The gold coin as a drop has no amount, so it would fall as a coin worth nothing.
    const auto drop = fields.find(drop_item_field);
    if (key.table == RulesTable::ActorDefinitions && drop != fields.end() &&
        drop->second == kf_enum_encode<std::int64_t>(KF_ITEM_GOLD_COIN))
        field_error(L, values_argument, "drop_item 39 is the gold coin; set gold instead");
    rules.by_mod[std::string(verdite::mod_id(L))].set(key, fields);
    ++rules.generation;
    return 0;
}

int rules_set_monster(lua_State *L) {
    return set_rule(L, check_monster(L), 3);
}
int rules_set_level(lua_State *L) {
    return set_rule(L, check_level(L), 2);
}
int rules_set_price(lua_State *L) {
    return set_rule(L, check_price(L), 3);
}
int rules_set_regen(lua_State *L) {
    return set_rule(L, check_regen(L), 2);
}

int rules_clear(lua_State *L) {
    rules_forget(verdite::mod_id(L));
    return 0;
}

// The record's original values as a table shaped like the setter's, or nil before the game loads
// it.
int push_original(lua_State *L, const RuleKey &key) {
    const bool monster_loaded =
        key.table != RulesTable::ActorDefinitions || key.first == rules.originals.definitions_floor;
    if (!rules.originals.loaded.at(table_index(key.table)) || !monster_loaded) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 0);
    const int record = lua_gettop(L);
    for (const RuleField &field : spec_of(key.table).fields) {
        const auto dot = field.name.find('.');
        const std::string name(dot == std::string_view::npos ? field.name
                                                             : field.name.substr(dot + 1));
        int into = record;
        if (dot != std::string_view::npos) {
            const std::string group(field.name.substr(0, dot));
            if (lua_getfield(L, record, group.c_str()) != LUA_TTABLE) {
                lua_pop(L, 1);
                lua_createtable(L, 0, 0);
                lua_pushvalue(L, -1);
                lua_setfield(L, record, group.c_str());
            }
            into = lua_gettop(L);
        }
        lua_pushinteger(L, static_cast<lua_Integer>(field.original(key)));
        lua_setfield(L, into, name.c_str());
        lua_settop(L, record);
    }
    return 1;
}

int rules_original_monster(lua_State *L) {
    return push_original(L, check_monster(L));
}
int rules_original_level(lua_State *L) {
    return push_original(L, check_level(L));
}
int rules_original_price(lua_State *L) {
    return push_original(L, check_price(L));
}
int rules_original_regen(lua_State *L) {
    return push_original(L, check_regen(L));
}

// ---------- the definitions file ----------
// The values table's type: "{hp?: integer, attack?: {cutting?: integer, ...}}"; mark is "?" for a
// table whose fields may be absent, "" for one that has them all.
std::string values_type(std::span<const RuleField> fields, std::string_view mark) {
    std::string type;
    std::string_view open_group;
    for (const RuleField &field : fields) {
        const auto dot = field.name.find('.');
        const std::string_view group =
            dot == std::string_view::npos ? std::string_view{} : field.name.substr(0, dot);
        const std::string_view name =
            dot == std::string_view::npos ? field.name : field.name.substr(dot + 1);
        if (group != open_group) {
            if (!open_group.empty())
                type += "}";
            if (!type.empty())
                type += ", ";
            if (!group.empty())
                type += std::string(group) + std::string(mark) + ": {";
            open_group = group;
        } else if (!type.empty()) {
            type += ", ";
        }
        type += std::string(name) + std::string(mark) + ": integer";
    }
    if (!open_group.empty())
        type += "}";
    return "{" + type + "}";
}

// set_regen takes a weapon's fields or an armour's.
std::vector<RuleField> regen_fields() {
    std::vector<RuleField> fields(weapon_fields.begin(), weapon_fields.end());
    for (const RuleField &field : armour_fields)
        if (!find_field(fields, field.name))
            fields.push_back(field);
    return fields;
}

// The functions' types for the definitions file, kept for the host's views of them.
std::string_view kept(std::string text) {
    static std::set<std::string, std::less<>> types;
    return *types.insert(std::move(text)).first;
}
std::string_view setter_type(std::string_view arguments, std::span<const RuleField> fields) {
    return kept("fun(" + std::string(arguments) + ", values: " + values_type(fields, "?") + ")");
}
// mark: "?" when a record has only some of the fields (a weapon's or an armour's).
std::string_view getter_type(std::string_view arguments, std::span<const RuleField> fields,
                             std::string_view mark) {
    return kept("fun(" + std::string(arguments) + "): " + values_type(fields, mark) + "?");
}

} // namespace

verdite::ModTable rules_table() {
    using verdite::ModFunction;
    constexpr std::string_view monster = "floor: integer, kind: integer";
    constexpr std::string_view price = "item: integer, shop: integer";
    const std::vector<RuleField> regen = regen_fields();
    return {
        .path = "kf.rules",
        .doc = "The calling mod's values for the game's rule tables, laid over the game's own when "
               "the callback "
               "returns; per field, the last mod in load order wins.",
        .functions =
            {ModFunction{
                 "set_monster", setter_type(monster, monster_fields),
                 "A monster kind's numbers on a floor; an awake monster keeps the HP it woke with.",
                 rules_set_monster},
             {"set_level", setter_type("level: integer", level_fields),
              "What reaching a level gives (1: a new game's start); later level-ups only.",
              rules_set_level},
             {"set_price", setter_type(price, price_fields),
              "An item's price in a shop (its stock bank, 1 or 2).", rules_set_price},
             {"set_regen", setter_type("item: integer", regen),
              "A weapon's or armour's updates between points: hp_every, mp_every (weapons), "
              "hp_drain_every "
              "(armour); 0 for none.",
              rules_set_regen},
             {"clear", "fun()", "Drops the calling mod's values.", rules_clear},
             {"original_monster", getter_type(monster, monster_fields, ""),
              "The game's own numbers for a kind, or nil when that floor is not the one loaded.",
              rules_original_monster},
             {"original_level", getter_type("level: integer", level_fields, ""),
              "The game's own numbers for a level.", rules_original_level},
             {"original_price", getter_type(price, price_fields, ""), "The game's own prices.",
              rules_original_price},
             {"original_regen", getter_type("item: integer", regen, "?"),
              "The game's own intervals: a weapon's hp_every and mp_every, an armour's hp_every "
              "and "
              "hp_drain_every.",
              rules_original_regen}},
        .fields = {},
    };
}

void rules_forget(std::string_view id) {
    if (const auto found = rules.by_mod.find(id); found != rules.by_mod.end()) {
        rules.by_mod.erase(found);
        ++rules.generation;
    }
}

void rules_lay(std::span<const std::string> running_in_load_order) {
    std::erase_if(rules.by_mod, [running_in_load_order](const auto &entry) {
        return std::ranges::find(running_in_load_order, entry.first) == running_in_load_order.end();
    });
    const bool same_order =
        rules.laid_order && std::ranges::equal(*rules.laid_order, running_in_load_order);
    if (same_order && rules.laid_generation == rules.generation)
        return;
    std::vector<const RuleValues *> in_order;
    for (const std::string &id : running_in_load_order)
        if (const auto found = rules.by_mod.find(id); found != rules.by_mod.end())
            in_order.push_back(&found->second);
    rules.laid_order.emplace(running_in_load_order.begin(), running_in_load_order.end());
    rules.laid_generation = rules.generation;
    lay_all(combine_rule_values(in_order));
}

void rules_lay_originals() {
    if (!rules.laid_order)
        return;
    rules.laid_order.reset();
    lay_all({});
}

} // namespace kf::ext

namespace kf {
void mods_rules_reapply(RulesTable table) {
    const std::size_t index = ext::table_index(table);
    ext::spec_of(table).capture();
    ext::rules.originals.loaded.at(index) = true;
    ext::lay_table(table);
}
} // namespace kf
