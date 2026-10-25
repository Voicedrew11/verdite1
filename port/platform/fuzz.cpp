#include <kf/platform/fuzz.h>
#include <kf/platform/automap.h>
#include <kf/platform/host.h>
#include <kf/platform/saves.h>
#include <kf/game/actor.h>
#include <kf/game/collision.h>
#include <kf/game/effect.h>
#include <kf/game/equipment.h>
#include <kf/game/graphics.h>
#include <kf/game/player.h>
#include <kf/game/resources.h>
#include <kf/lib/item.h>
#include <kf/lib/map.h>
#include <kf/lib/map_data.h>
#include <kf/lib/random.h>
#include <verdite/agent.h>
#include <verdite/fuzz.h>
#include <verdite/state_trace.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace kf {
namespace {

constexpr int floor_count = 5;
constexpr s32 world_size = KF_MAP_COLUMNS * KF_MAP_TILE_SIZE;
constexpr u16 max_hp_cap = 9999, max_mp_cap = 999, strong_max = 999;
constexpr u8 item_count_cap = 99;
// Ids the game never uses (main's give-all and menu census, 2026-10-22).
constexpr std::array<int, 11> unused_items = {12, 18, 25, 31, 65, 66, 71, 72, 77, 78, 79};
constexpr int kit_spells = 9;
constexpr int stuck_updates = 600;        // the position unchanged this long: moved to an open cell
constexpr int settle_after_change = 60;   // updates without checks after a floor change or a death
constexpr int open_after_warp = 2;        // updates after a floor warp before the move to an open cell

u32 button(Button b) { return button_mask(b); }

// The policy's presses in game-clock ticks (60 a second), from main's Fuzz.cs at about four
// ticks to its frame.
std::vector<verdite::FuzzPress> play_presses() {
    return {
        {"walk", button(Button::Up), 180, 20, 240},
        {"walk-left", Button::Up | Button::Left, 60, 20, 240},
        {"walk-right", Button::Up | Button::Right, 60, 20, 240},
        {"back-up", button(Button::Down), 60, 12, 80},
        {"turn-left", button(Button::Left), 70, 8, 80},
        {"turn-right", button(Button::Right), 70, 8, 80},
        {"strafe-left", button(Button::StrafeLeft), 40, 12, 100},
        {"strafe-right", button(Button::StrafeRight), 40, 12, 100},
        {"look-up", button(Button::LookUp), 20, 4, 24},
        {"look-down", button(Button::LookDown), 20, 4, 24},
        {"attack", button(Button::Attack), 120, 4, 12},
        {"magic", button(Button::Magic), 40, 4, 12},
        {"examine", button(Button::Confirm), 100, 4, 12},
        {"menu", button(Button::Back), 20, 6, 6},
        {"start", button(Button::Start), 5, 6, 6},
        {"pause", button(Button::Select), 3, 6, 6},
        {"idle", 0, 92, 4, 60},
    };
}

std::vector<verdite::FuzzPress> menu_presses() {
    return {
        {"menu-down", button(Button::Down), 35, 6, 6},
        {"menu-up", button(Button::Up), 25, 6, 6},
        {"menu-right", button(Button::Right), 12, 6, 6},
        {"menu-left", button(Button::Left), 8, 6, 6},
        {"menu-confirm", button(Button::Confirm), 12, 6, 6},
        {"menu-back", button(Button::Back), 8, 6, 6},
    };
}

struct Options {
    std::uint64_t seed = 0;
    bool kit = false, strong = false, godmode = false, load = false;
    int area = 0;   // 0 none, -1 random
    std::string coverage = "fuzz-coverage.txt";
    std::set<std::string> skip;
};

struct Fuzz {
    Options options;
    bool active = false;
    QuickRequest quick = QuickRequest::None;
    bool quick_save_known = false, quick_save_exists = false;
    std::uint64_t updates = 0, open_at_update = 0;
    bool set_up = false;
    KfFloorId last_floor{};
    KfMapVariant last_variant{};
    VECTOR last_position{};
    int still_updates = 0;
    std::uint64_t next_coverage_tick = 0;
    std::set<std::tuple<int, int, int>> cells;
    std::set<int> floors, items;
    std::optional<verdite::StateTrace> trace;
};
Fuzz fuzz;

const char *env(const char *name) {
    const char *value = std::getenv(name);
    return value && *value ? value : nullptr;
}

long env_number(const char *name, long fallback) {
    const char *value = env(name);
    if (!value)
        return fallback;
    char *end = nullptr;
    const long number = std::strtol(value, &end, 10);
    return *end ? fallback : number;
}

bool env_on(const char *name) { return verdite::agent_switch_from(std::getenv(name)); }

std::vector<std::string> split_list(const char *text) {
    std::vector<std::string> out;
    std::string item;
    for (const char *c = text; c && *c; ++c) {
        if (*c == ',') {
            if (!item.empty())
                out.push_back(item);
            item.clear();
        } else {
            item += *c;
        }
    }
    if (!item.empty())
        out.push_back(item);
    return out;
}

// --- the game's state, as the invariants and macros read it

bool floor_valid(KfFloorId floor) {
    const auto n = kf_enum_encode<int>(floor);
    return n >= 1 && n <= floor_count;
}

template <typename T, std::size_t N>
std::optional<std::size_t> index_in(const T *pointer, const T (&table)[N]) {
    if (pointer < table || pointer >= table + N)
        return std::nullopt;
    return static_cast<std::size_t>(pointer - table);
}

template <typename T, std::size_t N>
std::optional<std::size_t> index_in(const T *pointer, const std::array<T, N> &table) {
    if (pointer < table.data() || pointer >= table.data() + N)
        return std::nullopt;
    return static_cast<std::size_t>(pointer - table.data());
}

// What is wrong with a pointer that must be null or point into its table.
template <typename T, typename Table>
std::optional<std::string> outside(const char *name, const T *pointer, const Table &table) {
    if (!pointer || index_in(pointer, table))
        return std::nullopt;
    return std::string(name) + " points outside its table";
}

bool cell_blocked(int x, int z) {
    return map_collision_grid.cells[z][x] == KF_MAP_CELL_BLOCKED;
}

// A cell the player can stand in with room round it: floor in the automap's terms (not a
// chasm, a half cell or a door), and its four neighbours walkable too.
bool cell_open(const AutomapSnapshot &map, int x, int z) {
    auto floor = [&](int cx, int cz) {
        return cx >= 0 && cz >= 0 && cx < automap_columns && cz < automap_rows &&
               map.cells[cz * automap_columns + cx] == AutomapCell::Floor;
    };
    return floor(x, z) && floor(x - 1, z) && floor(x + 1, z) && floor(x, z - 1) && floor(x, z + 1);
}

std::optional<std::string> random_open_cell(verdite::FuzzRandom &random) {
    const auto &map = automap_snapshot();
    if (map.floor != kf_enum_encode<int>(player_state.progress_state.current_floor))
        return std::nullopt;
    std::vector<int> open;
    for (int z = 0; z < automap_rows; ++z)
        for (int x = 0; x < automap_columns; ++x)
            if (cell_open(map, x, z))
                open.push_back(z * automap_columns + x);
    if (open.empty())
        return std::nullopt;
    const int cell = open[random.below(static_cast<u32>(open.size()))];
    return std::to_string(cell % automap_columns) + " " + std::to_string(cell / automap_columns);
}

// --- macros

bool run_warp(std::string_view args) {
    const int floor = std::atoi(std::string(args).c_str());
    if (floor < 1 || floor > floor_count)
        return false;
    const auto id = kf_enum_decode<KfFloorId>(floor);
    player_warp_change_floor(id, id == KF_FLOOR_5 ? KF_FLOOR5_ENTRY_VARIANT : KF_MAP_VARIANT_DEFAULT);
    // The cell is kept from the last floor; it may be rock here.
    fuzz.open_at_update = fuzz.updates + open_after_warp;
    return true;
}

std::optional<std::string> choose_warp(verdite::FuzzRandom &random) {
    const int current = kf_enum_encode<int>(player_state.progress_state.current_floor);
    int floor = random.between(1, floor_count - 1);
    if (floor >= current)
        ++floor;
    return std::to_string(floor);
}

bool run_goto(std::string_view args) {
    int x = -1, z = -1;
    if (std::sscanf(std::string(args).c_str(), "%d %d", &x, &z) != 2 || x < 0 || z < 0 ||
        x >= KF_MAP_COLUMNS || z >= KF_MAP_ROWS)
        return false;
    // Floor 5's own teleport reloads its variant's creatures (CHR<variant>.MIM); the other
    // floors have no such file, so the player is moved as that teleport moves them.
    if (player_state.progress_state.current_floor == KF_FLOOR_5) {
        player_warp_same_floor(player_state.map_variant, x, z);
        return true;
    }
    const auto cell = player_state.motion_state.map_cell;
    collision_adjust_cell_occupancy(cell.x, cell.z, -1);
    player_state.camera_position.vx = x * KF_MAP_TILE_SIZE + KF_MAP_TILE_CENTER;
    player_state.camera_position.vz = z * KF_MAP_TILE_SIZE + KF_MAP_TILE_CENTER;
    player_sync_position_to_map();
    return true;
}

bool run_quick(QuickRequest request) {
    if (classic_mode)
        return false;
    fuzz.quick = request;
    if (request == QuickRequest::Save)
        fuzz.quick_save_exists = true;
    return true;
}

// Not between a floor warp and the move to an open cell after it: the warp keeps the last
// floor's cell, which may be rock here, and a save there would load the player into rock.
bool between_warp_and_open_cell() { return fuzz.open_at_update != 0; }

std::optional<std::string> choose_quick_save(verdite::FuzzRandom &) {
    return classic_mode || between_warp_and_open_cell() ? std::nullopt : std::optional<std::string>("");
}

std::optional<std::string> choose_quick_load(verdite::FuzzRandom &) {
    if (!fuzz.quick_save_known) {
        std::array<u8, save_card_bytes> card{};
        fuzz.quick_save_known = true;
        fuzz.quick_save_exists = save_card_read(SaveCard::Quick, card) == SaveFileResult::Ok;
    }
    return classic_mode || !fuzz.quick_save_exists || between_warp_and_open_cell() ? std::nullopt
                                                                                  : std::optional<std::string>("");
}

bool run_kit(std::string_view) {
    auto &held = item_stock[kf_enum_encode<u8>(KF_ITEM_STOCK_PLAYER)];
    for (int id = 0; id < KF_ITEM_COUNT; ++id)
        if (std::find(unused_items.begin(), unused_items.end(), id) == unused_items.end() && held[id] == 0)
            held[id] = 1;
    for (int spell = 0; spell < kit_spells; ++spell)
        effect_state.magic.entries[spell].learned = KF_MAGIC_LEARNED;
    return true;
}

bool run_strong(std::string_view) {
    auto &vitals = player_state.vitals;
    vitals.maximum_hp = vitals.current_hp = strong_max;
    vitals.maximum_mp = vitals.current_mp = strong_max;
    return true;
}

std::vector<verdite::FuzzMacro> macros() {
    return {
        {"warp", 4, choose_warp, run_warp, settle_after_change},
        {"goto", 3, random_open_cell, run_goto, 30},
        {"save", 2, choose_quick_save, [](std::string_view) { return run_quick(QuickRequest::Save); }, 0},
        {"load", 2, choose_quick_load, [](std::string_view) { return run_quick(QuickRequest::Load); }, 30},
        {"kit", 0, nullptr, run_kit, 0},
        {"strong", 0, nullptr, run_strong, 0},
    };
}

// --- invariants

std::optional<std::string> check_vitals() {
    const auto &v = player_state.vitals;
    char text[96];
    if (v.current_hp > v.maximum_hp || v.current_mp > v.maximum_mp || v.maximum_hp > max_hp_cap || v.maximum_mp > max_mp_cap) {
        std::snprintf(text, sizeof text, "hp %u/%u mp %u/%u", v.current_hp, v.maximum_hp, v.current_mp, v.maximum_mp);
        return text;
    }
    return std::nullopt;
}

std::optional<std::string> check_progress() {
    const auto &p = player_state.progress_state;
    const int current = kf_enum_encode<int>(p.current_floor), highest = kf_enum_encode<int>(p.highest_floor);
    if (floor_valid(p.current_floor) && floor_valid(p.highest_floor) && highest >= current && p.level != 0)
        return std::nullopt;
    return "floor " + std::to_string(current) + " highest " + std::to_string(highest) + " level " + std::to_string(p.level);
}

std::optional<std::string> check_position() {
    const auto &at = player_state.camera_position;
    if (at.vx >= 0 && at.vz >= 0 && at.vx < world_size && at.vz < world_size)
        return std::nullopt;
    return "at " + std::to_string(at.vx) + ", " + std::to_string(at.vz);
}

std::optional<std::string> check_cell() {
    const auto &at = player_state.camera_position;
    const auto cell = player_state.motion_state.map_cell;
    if (check_position())
        return std::nullopt;
    const int x = at.vx / KF_MAP_TILE_SIZE, z = at.vz / KF_MAP_TILE_SIZE;
    if (cell.x == x && cell.z == z)
        return std::nullopt;
    return "cell " + std::to_string(cell.x) + "," + std::to_string(cell.z) + " but at " + std::to_string(x) + "," + std::to_string(z);
}

std::optional<std::string> check_in_rock() {
    if (check_position())
        return std::nullopt;
    const int x = player_state.camera_position.vx / KF_MAP_TILE_SIZE, z = player_state.camera_position.vz / KF_MAP_TILE_SIZE;
    if (!cell_blocked(x, z))
        return std::nullopt;
    return "floor " + std::to_string(kf_enum_encode<int>(player_state.progress_state.current_floor)) + " cell " +
           std::to_string(x) + "," + std::to_string(z);
}

std::optional<std::string> check_items() {
    const auto &held = item_stock[kf_enum_encode<u8>(KF_ITEM_STOCK_PLAYER)];
    for (int id = 0; id < KF_ITEM_COUNT; ++id) {
        const bool unused = std::find(unused_items.begin(), unused_items.end(), id) != unused_items.end();
        if (held[id] > item_count_cap || (unused && held[id] != 0))
            return "item " + std::to_string(id) + " count " + std::to_string(held[id]);
    }
    return std::nullopt;
}

std::optional<std::string> check_equipment() {
    const auto &p = player_state;
    for (auto wrong : {outside("weapon", p.equipped_weapon_record, weapon_records.entries),
                       outside("head armour", p.equipped_head_armor_record, armor_records.entries),
                       outside("body armour", p.equipped_body_armor_record, armor_records.entries),
                       outside("shield", p.equipped_shield_record, armor_records.entries),
                       outside("arm armour", p.equipped_arm_armor_record, armor_records.entries),
                       outside("leg armour", p.equipped_leg_armor_record, armor_records.entries),
                       outside("magic", p.selected_magic_record, effect_state.magic.entries),
                       outside("weapon animation", p.weapon_animation_cache, game_graphics_runtime.animation_cache_records)})
        if (wrong)
            return wrong;
    return std::nullopt;
}

std::optional<std::string> check_actor(std::size_t i, const KfActor &actor) {
    if (actor.slot_state == KF_ACTOR_SLOT_FREE)
        return std::nullopt;
    if (actor.definition_id >= KF_ACTOR_DEFINITION_COUNT || actor.tile_x >= KF_MAP_COLUMNS || actor.tile_z >= KF_MAP_ROWS)
        return "actor " + std::to_string(i) + " definition " + std::to_string(actor.definition_id) + " tile " +
               std::to_string(actor.tile_x) + "," + std::to_string(actor.tile_z);
    if (auto wrong = outside("animation", actor.animation_cache, game_graphics_runtime.animation_cache_records))
        return "actor " + std::to_string(i) + " " + *wrong;
    return std::nullopt;
}

std::optional<std::string> check_actors() {
    const auto &s = actor_state;
    for (auto wrong : {outside("current actor", s.current, s.actors), outside("target", s.player_target, s.actors),
                       outside("current definition", s.current_definition, s.definitions.entries)})
        if (wrong)
            return wrong;
    for (std::size_t i = 0; i < s.actors.size(); ++i)
        if (auto wrong = check_actor(i, s.actors[i]))
            return wrong;
    return std::nullopt;
}

std::optional<std::string> check_effects_and_events() {
    const auto &cache = game_graphics_runtime.animation_cache_records;
    for (std::size_t i = 0; i < effect_state.records.size(); ++i)
        if (auto wrong = outside("animation", effect_state.records[i].animation_cache, cache))
            return "effect " + std::to_string(i) + " " + *wrong;
    for (std::size_t i = 0; i < map_runtime_state.events.size(); ++i)
        if (auto wrong = outside("animation", map_runtime_state.events[i].animation_cache, cache))
            return "event " + std::to_string(i) + " " + *wrong;
    for (auto wrong : {outside("current effect", effect_state.current_record, effect_state.records),
                       outside("current magic", effect_state.current_magic, effect_state.magic.entries),
                       outside("current event", map_runtime_state.current_event, map_runtime_state.events)})
        if (wrong)
            return wrong;
    return std::nullopt;
}

std::vector<verdite::FuzzInvariant> invariants() {
    std::vector<verdite::FuzzInvariant> all = {
        {"vitals", check_vitals},       {"progress", check_progress}, {"position", check_position},
        {"cell", check_cell},           {"in-rock", check_in_rock},   {"items", check_items},
        {"equipment", check_equipment}, {"actors", check_actors},     {"effects", check_effects_and_events},
    };
    std::erase_if(all, [](const auto &invariant) { return fuzz.options.skip.count(invariant.name) != 0; });
    return all;
}

// --- weights by name: KF_FUZZ_WEIGHTS=walk=0,warp=20

void apply_weights(verdite::FuzzConfig &config) {
    for (const auto &item : split_list(env("KF_FUZZ_WEIGHTS"))) {
        const auto equals = item.find('=');
        if (equals == std::string::npos)
            continue;
        const auto name = item.substr(0, equals);
        const int weight = std::atoi(item.c_str() + equals + 1);
        bool found = false;
        for (auto *presses : {&config.presses, &config.menu_presses})
            for (auto &press : *presses)
                if (press.name == name) {
                    press.weight = weight;
                    found = true;
                }
        for (auto &macro : config.macros_table)
            if (macro.name == name) {
                macro.per_10000 = weight;
                found = true;
            }
        if (!found)
            std::fprintf(stderr, "[KF1] fuzz: no press or macro '%s' in KF_FUZZ_WEIGHTS\n", name.c_str());
    }
}

// --- coverage: the floors, cells and items a run reached

void note_coverage() {
    const int floor = kf_enum_encode<int>(player_state.progress_state.current_floor);
    if (!floor_valid(player_state.progress_state.current_floor) || check_position())
        return;
    fuzz.floors.insert(floor);
    fuzz.cells.insert({floor, player_state.camera_position.vx / KF_MAP_TILE_SIZE,
                       player_state.camera_position.vz / KF_MAP_TILE_SIZE});
    const auto &held = item_stock[kf_enum_encode<u8>(KF_ITEM_STOCK_PLAYER)];
    for (int id = 0; id < KF_ITEM_COUNT; ++id)
        if (held[id] != 0)
            fuzz.items.insert(id);
}

void write_coverage() {
    if (fuzz.options.coverage.empty())
        return;
    std::FILE *file = std::fopen(fuzz.options.coverage.c_str(), "w");
    if (!file)
        return;
    for (const int floor : fuzz.floors)
        std::fprintf(file, "floor %d\n", floor);
    for (const auto &[floor, x, z] : fuzz.cells)
        std::fprintf(file, "cell %d %d %d\n", floor, x, z);
    for (const int id : fuzz.items)
        std::fprintf(file, "item %d\n", id);
    std::fclose(file);
}

std::string status_fields() {
    const auto &v = player_state.vitals;
    const auto &at = player_state.camera_position;
    char text[256];
    std::snprintf(text, sizeof text, ",\"context\":\"%s\",\"hp\":%u,\"maxHp\":%u,\"floor\":%d,\"level\":%u,\"pos\":[%d,%d,%d],\"cells\":%zu,\"floors\":%zu,\"items\":%zu",
                  input_context_name(host_input_context()), v.current_hp, v.maximum_hp,
                  kf_enum_encode<int>(player_state.progress_state.current_floor),
                  player_state.progress_state.level, at.vx, at.vy, at.vz, fuzz.cells.size(), fuzz.floors.size(), fuzz.items.size());
    return text;
}

[[noreturn]] void finish(int exit_code) {
    write_coverage();
    host_shutdown();
    std::exit(exit_code);
}

// --- the state trace: the named globals, pointers written as table indices

// A pointer as it compares between runs and builds: 0 null, 1 + its index in the table, or
// all ones when it points elsewhere.
template <typename T, typename Table>
std::uint64_t canonical(const T *pointer, const Table &table) {
    if (!pointer)
        return 0;
    const auto index = index_in(pointer, table);
    return index ? *index + 1 : ~std::uint64_t{0};
}

std::uint64_t canonical_arena(const u8 *pointer) {
    if (!pointer)
        return 0;
    if (memory_arena.start && pointer >= memory_arena.start && pointer < memory_arena.end)
        return static_cast<std::uint64_t>(pointer - memory_arena.start) + 1;
    return ~std::uint64_t{0};
}

template <typename T>
void append_bytes(std::vector<std::byte> &out, const T &value) {
    const auto *bytes = reinterpret_cast<const std::byte *>(&value);
    out.insert(out.end(), bytes, bytes + sizeof value);
}

// Overwrites the pointer stored at byte `at` of the object starting at `base` in out.
void put_pointer(std::vector<std::byte> &out, std::size_t base, std::size_t at, std::uint64_t value) {
    static_assert(sizeof(void *) == sizeof value);
    std::memcpy(out.data() + base + at, &value, sizeof value);
}

void write_player(std::vector<std::byte> &out) {
    const auto &p = player_state;
    const auto &cache = game_graphics_runtime.animation_cache_records;
    append_bytes(out, p);
    put_pointer(out, 0, offsetof(KfPlayerState, selected_magic_record), canonical(p.selected_magic_record, effect_state.magic.entries));
    put_pointer(out, 0, offsetof(KfPlayerState, equipped_weapon_record), canonical(p.equipped_weapon_record, weapon_records.entries));
    put_pointer(out, 0, offsetof(KfPlayerState, weapon_asset_buffer), canonical_arena(p.weapon_asset_buffer));
    put_pointer(out, 0, offsetof(KfPlayerState, weapon_animation_cache), canonical(p.weapon_animation_cache, cache));
    put_pointer(out, 0, offsetof(KfPlayerState, equipped_head_armor_record), canonical(p.equipped_head_armor_record, armor_records.entries));
    put_pointer(out, 0, offsetof(KfPlayerState, equipped_body_armor_record), canonical(p.equipped_body_armor_record, armor_records.entries));
    put_pointer(out, 0, offsetof(KfPlayerState, equipped_shield_record), canonical(p.equipped_shield_record, armor_records.entries));
    put_pointer(out, 0, offsetof(KfPlayerState, equipped_arm_armor_record), canonical(p.equipped_arm_armor_record, armor_records.entries));
    put_pointer(out, 0, offsetof(KfPlayerState, equipped_leg_armor_record), canonical(p.equipped_leg_armor_record, armor_records.entries));
}

void write_actors(std::vector<std::byte> &out) {
    const auto &s = actor_state;
    append_bytes(out, s);
    for (std::size_t i = 0; i < s.actors.size(); ++i)
        put_pointer(out, offsetof(KfActorState, actors) + i * sizeof(KfActor), offsetof(KfActor, animation_cache),
                    canonical(s.actors[i].animation_cache, game_graphics_runtime.animation_cache_records));
    put_pointer(out, 0, offsetof(KfActorState, current_definition), canonical(s.current_definition, s.definitions.entries));
    put_pointer(out, 0, offsetof(KfActorState, current), canonical(s.current, s.actors));
    put_pointer(out, 0, offsetof(KfActorState, player_target), canonical(s.player_target, s.actors));
}

void write_effects(std::vector<std::byte> &out) {
    const auto &s = effect_state;
    append_bytes(out, s);
    for (std::size_t i = 0; i < s.records.size(); ++i)
        put_pointer(out, offsetof(KfEffectState, records) + i * sizeof(KfEffectRecord), offsetof(KfEffectRecord, animation_cache),
                    canonical(s.records[i].animation_cache, game_graphics_runtime.animation_cache_records));
    put_pointer(out, 0, offsetof(KfEffectState, current_magic), canonical(s.current_magic, s.magic.entries));
    put_pointer(out, 0, offsetof(KfEffectState, current_record), canonical(s.current_record, s.records));
}

void write_map_events(std::vector<std::byte> &out) {
    const auto &s = map_runtime_state;
    append_bytes(out, s);
    for (std::size_t i = 0; i < s.events.size(); ++i)
        put_pointer(out, offsetof(KfMapRuntimeState, events) + i * sizeof(KfMapEvent), offsetof(KfMapEvent, animation_cache),
                    canonical(s.events[i].animation_cache, game_graphics_runtime.animation_cache_records));
    put_pointer(out, 0, offsetof(KfMapRuntimeState, current_event), canonical(s.current_event, s.events));
    put_pointer(out, 0, offsetof(KfMapRuntimeState, variant_asset_buffer), canonical_arena(s.variant_asset_buffer));
}

std::vector<verdite::TraceRegion> trace_regions() {
    return {
        {"random", [](std::vector<std::byte> &out) { append_bytes(out, random_peek()); }},
        {"player", write_player},
        {"actors", write_actors},
        {"effects", write_effects},
        {"map_events", write_map_events},
        {"map_objects", [](std::vector<std::byte> &out) { append_bytes(out, map_object_state); }},
        {"items", [](std::vector<std::byte> &out) { append_bytes(out, item_stock); }},
    };
}

// --- the run

Options read_options(std::uint64_t seed) {
    Options options;
    options.seed = seed;
    options.kit = env_on("KF_FUZZ_KIT");
    options.strong = env_on("KF_FUZZ_STRONG");
    options.godmode = env_on("KF_FUZZ_GODMODE");
    options.load = env_on("KF_FUZZ_LOAD");
    if (const char *area = env("KF_FUZZ_AREA"))
        options.area = std::strcmp(area, "random") == 0 ? -1 : std::clamp(std::atoi(area), 0, floor_count);
    if (const char *coverage = std::getenv("KF_FUZZ_COVERAGE"))
        options.coverage = std::strcmp(coverage, "0") == 0 ? "" : coverage;
    for (const auto &name : split_list(env("KF_FUZZ_SKIP")))
        options.skip.insert(name);
    return options;
}

// "KEY=value" for each KF_FUZZ_* switch set, for the recording's header.
std::vector<std::string> notes() {
    std::vector<std::string> out;
    for (const char *name : {"KF_FUZZ_TICKS", "KF_FUZZ_MACROS", "KF_FUZZ_MACRORATE", "KF_FUZZ_WEIGHTS", "KF_FUZZ_AREA",
                             "KF_FUZZ_KIT", "KF_FUZZ_STRONG", "KF_FUZZ_GODMODE", "KF_FUZZ_LOAD", "KF_FUZZ_GAME_SEED"})
        if (const char *value = env(name))
            out.push_back(std::string(name) + "=" + value);
    return out;
}

verdite::FuzzConfig fuzz_config(std::uint64_t seed) {
    verdite::FuzzConfig config;
    config.tag = "KF1";
    config.seed = seed;
    config.ticks = static_cast<std::uint64_t>(std::max(env_number("KF_FUZZ_TICKS", 72000), 1L));
    if (const char *replay = env("KF_FUZZ_REPLAY"))
        config.replay = replay;
    config.recording = env("KF_FUZZ_OUT") ? env("KF_FUZZ_OUT") : "fuzz-input.log";
    config.notes = notes();
    config.stop_on_violation = env_on("KF_FUZZ_STOP");
    const char *macros_switch = env("KF_FUZZ_MACROS");
    config.macros = !macros_switch || std::strcmp(macros_switch, "0") != 0;
    config.macro_rate_percent = static_cast<int>(env_number("KF_FUZZ_MACRORATE", 100));
    config.status_every_ticks = static_cast<int>(env_number("KF_FUZZ_STATUS", 3600));
    config.presses = play_presses();
    config.menu_presses = menu_presses();
    config.escape = {button(Button::Back), button(Button::Back), button(Button::Back), button(Button::Confirm)};
    config.macros_table = macros();
    config.invariants = invariants();
    config.in_menu = [] { return host_input_context() == InputContext::Menu || host_input_context() == InputContext::Scripted; };
    config.status = status_fields;
    config.finish = finish;
    apply_weights(config);
    return config;
}

// The start, once: a quick load, the floor, the kit and the strength, as triggered macros so
// a replay does them from its recording.
void set_up(verdite::FuzzRandom &random) {
    fuzz.set_up = true;
    if (fuzz.options.load)
        verdite::fuzz_trigger("load", "");
    if (fuzz.options.area != 0) {
        const int floor = fuzz.options.area < 0 ? random.between(1, floor_count) : fuzz.options.area;
        verdite::fuzz_trigger("warp", std::to_string(floor));
    }
    if (fuzz.options.kit)
        verdite::fuzz_trigger("kit", "");
    if (fuzz.options.strong)
        verdite::fuzz_trigger("strong", "");
}

// Deaths, floor changes and the player standing still, every update in a run or a replay.
void watch_player() {
    const auto &p = player_state;
    if (p.update_state == KF_PLAYER_UPDATE_DYING || p.update_state == KF_PLAYER_UPDATE_RECOVERY_FADE)
        verdite::fuzz_settle(settle_after_change);
    if (p.progress_state.current_floor != fuzz.last_floor || p.map_variant != fuzz.last_variant)
        verdite::fuzz_settle(settle_after_change);
    fuzz.last_floor = p.progress_state.current_floor;
    fuzz.last_variant = p.map_variant;
    const bool still = p.camera_position.vx == fuzz.last_position.vx && p.camera_position.vz == fuzz.last_position.vz;
    fuzz.still_updates = still ? fuzz.still_updates + 1 : 0;
    fuzz.last_position = p.camera_position;
    if (fuzz.options.godmode)
        player_state.vitals.current_hp = player_state.vitals.maximum_hp;
}

// Moves to an open cell after a floor warp or when stuck; recorded, so not while replaying.
void unstick(verdite::FuzzRandom &random) {
    const bool after_warp = fuzz.open_at_update != 0 && fuzz.updates >= fuzz.open_at_update;
    if (!after_warp && fuzz.still_updates < stuck_updates)
        return;
    if (const auto cell = random_open_cell(random)) {
        fuzz.open_at_update = 0;
        fuzz.still_updates = 0;
        verdite::fuzz_trigger("goto", *cell);
    }
}

} // namespace

void fuzz_start() {
    if (const char *path = env("KF_TRACE")) {
        fuzz.trace = verdite::StateTrace::create(path, trace_regions());
        if (!fuzz.trace)
            std::fprintf(stderr, "[KF1] trace: cannot write %s\n", path);
    }
    const char *seed_text = env("KF_FUZZ");
    if (!seed_text && !env("KF_FUZZ_REPLAY"))
        return;
    const auto seed = seed_text ? std::strtoull(seed_text, nullptr, 10) : 0;
    fuzz.options = read_options(seed);
    fuzz.active = verdite::fuzz_start(fuzz_config(seed));
    if (!fuzz.active)
        return;
    fuzz.options.seed = verdite::fuzz_seed();
    random_seed(static_cast<u32>(env_number("KF_FUZZ_GAME_SEED", static_cast<long>(verdite::fuzz_seed() & 0xffffffffu))));
}

void fuzz_poll() {
    if (!fuzz.active)
        return;
    const auto tick = host_clock_tick();
    verdite::fuzz_poll(tick);
    if (tick >= fuzz.next_coverage_tick) {
        fuzz.next_coverage_tick = tick + 3600;
        write_coverage();
    }
}

u32 fuzz_buttons() { return fuzz.active ? verdite::fuzz_buttons(host_clock_tick()) : 0; }

QuickRequest fuzz_quick_request() {
    const auto request = fuzz.quick;
    fuzz.quick = QuickRequest::None;
    return request;
}

void fuzz_between_updates() {
    ++fuzz.updates;
    if (fuzz.trace)
        fuzz.trace->record(fuzz.updates, host_clock_tick());
    if (!fuzz.active)
        return;
    watch_player();
    note_coverage();
    verdite::fuzz_between_updates(host_clock_tick());
    if (verdite::fuzz_replaying())
        return;
    // The fuzzer's own random numbers for the start and unsticking, apart from the policy's.
    static verdite::FuzzRandom random(fuzz.options.seed ^ 0x5EEDull);
    if (!fuzz.set_up)
        set_up(random);
    unstick(random);
}

bool fuzz_allows_power_off() {
    if (!fuzz.active)
        return true;
    verdite::fuzz_note("quit-declined");
    return false;
}

int fuzz_compare_traces(const char *path_a, const char *path_b) {
    const auto comparison = verdite::trace_compare(path_a, path_b);
    if (!comparison) {
        std::printf("{\"ok\":false,\"error\":\"cannot read both traces, or their regions differ\"}\n");
        return 2;
    }
    if (!comparison->divergence) {
        std::printf("{\"ok\":true,\"equal\":true,\"updates\":%llu}\n", static_cast<unsigned long long>(comparison->updates));
        return 0;
    }
    const auto &d = *comparison->divergence;
    std::printf("{\"ok\":true,\"equal\":false,\"updates\":%llu,\"update\":%llu,\"tickA\":%llu,\"tickB\":%llu,\"region\":%s}\n",
                static_cast<unsigned long long>(comparison->updates), static_cast<unsigned long long>(d.update),
                static_cast<unsigned long long>(d.tick_a), static_cast<unsigned long long>(d.tick_b),
                verdite::json_quote(d.region).c_str());
    return 1;
}
}
