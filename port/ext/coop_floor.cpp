#include "coop_floor.h"

#include <kf/game/actor.h>
#include <kf/game/animation_cache.h>
#include <kf/game/collision.h>
#include <kf/game/effect.h>
#include <kf/game/player.h>
#include <kf/lib/map.h>
#include <kf/lib/map_data.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <type_traits>

namespace kf::ext {
namespace {

constexpr std::array<std::uint8_t, 4> magic = {'K', 'F', 'F', 'L'};
constexpr std::uint8_t version = 1;

// The sizes of what is copied whole: a snapshot from a build of another layout is refused.
constexpr std::array<std::uint32_t, 6> layout = {
    sizeof(KfActor), sizeof(KfActorDefinitionTable), sizeof(KfEffectRecord), sizeof(KfMapEvent),
    sizeof(KfMapObjectState), sizeof(KfMapGrid)};

struct Header {
    std::array<std::uint8_t, 4> magic;
    std::uint8_t version;
    std::uint8_t floor;
    std::array<std::uint32_t, 6> layout;
};

// The floor's live state, as it is copied in and out.
struct Floor {
    std::array<KfActor, KF_ACTOR_CAPACITY> actors;
    KfActorDefinitionTable definitions;
    std::array<KfEffectRecord, KF_EFFECT_CAPACITY> effects;
    std::array<KfMapEvent, KF_MAP_EVENT_CAPACITY> events;
    u16 dialogue_advance_gate;
    u16 ambient_script_countdown;
    KfMapObjectState objects;
    std::array<std::array<u8, KF_MAP_COLUMNS>, KF_MAP_ROWS> occupants;   // this player not counted
};
static_assert(std::is_trivially_copyable_v<Floor>);

u8 current_floor() {
    return kf_enum_encode<u8>(player_state.progress_state.current_floor);
}

template <typename T>
void append(std::vector<std::uint8_t> &out, const T &value) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
    out.insert(out.end(), bytes, bytes + sizeof value);
}

// The occupant counts of the collision grid without this player, who the taker counts itself.
std::array<std::array<u8, KF_MAP_COLUMNS>, KF_MAP_ROWS> occupants_but_this_player() {
    const auto cell = player_state.motion_state.map_cell;
    collision_adjust_cell_occupancy(cell.x, cell.z, -1);
    std::array<std::array<u8, KF_MAP_COLUMNS>, KF_MAP_ROWS> occupants{};
    for (std::size_t z = 0; z < KF_MAP_ROWS; ++z)
        for (std::size_t x = 0; x < KF_MAP_COLUMNS; ++x)
            occupants[z][x] = map_collision_flag_grid.cells[z][x] & KF_CELL_OCCUPANT_COUNT_MASK;
    collision_adjust_cell_occupancy(cell.x, cell.z, 1);
    return occupants;
}

// Animation clips are not checked here: the animation cache checks each against its model's clips.
bool cell_valid(u32 x, u32 z) {
    return x < KF_MAP_COLUMNS && z < KF_MAP_ROWS;
}

bool actor_valid(const KfActor &actor) {
    if (actor.slot_state == KF_ACTOR_SLOT_FREE)
        return true;
    return actor.definition_id < KF_ACTOR_DEFINITION_COUNT && cell_valid(actor.cell_x, actor.cell_z)
        && cell_valid(actor.tile_x, actor.tile_z);
}

bool effect_valid(const KfEffectRecord &effect) {
    if (effect.type == KF_EFFECT_SLOT_FREE)
        return true;
    return effect.parent_effect_index < KF_EFFECT_CAPACITY;
}

bool event_valid(const KfMapEvent &event) {
    if (event.state == KF_MAP_EVENT_FREE)
        return true;
    return event.dialogue.stage <= KF_DIALOGUE_STAGE_COUNT && cell_valid(event.cell_x, event.cell_z);
}

bool object_valid(const KfMapObject &object) {
    if (object.object_id == KF_OBJECT_NONE)
        return true;
    return kf_enum_encode<u8>(object.object_id) < KF_MAP_OBJECT_DEFINITION_COUNT
        && cell_valid(object.cell_x, object.cell_z) && map_saved_link_valid(object.action, object.link);
}

bool occupants_valid(const Floor &floor) {
    return std::ranges::all_of(floor.occupants, [](const auto &row) {
        return std::ranges::all_of(row, [](u8 count) { return (count & ~KF_CELL_OCCUPANT_COUNT_MASK) == 0; });
    });
}

bool floor_valid(const Floor &floor) {
    return std::ranges::all_of(floor.actors, actor_valid) && std::ranges::all_of(floor.effects, effect_valid)
        && std::ranges::all_of(floor.events, event_valid) && std::ranges::all_of(floor.objects.objects, object_valid)
        && occupants_valid(floor);
}

// The animation caches are drawing's, rebuilt when the slot is next drawn.
void drop_caches(Floor &floor) {
    for (auto &actor : floor.actors)
        actor.animation_cache = nullptr;
    for (auto &effect : floor.effects)
        effect.animation_cache = nullptr;
    for (auto &event : floor.events)
        event.animation_cache = nullptr;
}

void put_occupants(const Floor &floor) {
    for (std::size_t z = 0; z < KF_MAP_ROWS; ++z)
        for (std::size_t x = 0; x < KF_MAP_COLUMNS; ++x) {
            u8 &cell = map_collision_flag_grid.cells[z][x];
            cell = static_cast<u8>((cell & ~KF_CELL_OCCUPANT_COUNT_MASK) | floor.occupants[z][x]);
        }
    const auto cell = player_state.motion_state.map_cell;
    collision_adjust_cell_occupancy(cell.x, cell.z, 1);
}

} // namespace

std::vector<std::uint8_t> coop_floor_capture() {
    Floor floor{};
    floor.actors = actor_state.actors;
    floor.definitions = actor_state.definitions;
    floor.effects = effect_state.records;
    floor.events = map_runtime_state.events;
    floor.dialogue_advance_gate = map_runtime_state.dialogue_advance_gate;
    floor.ambient_script_countdown = map_runtime_state.ambient_script_countdown;
    floor.objects = map_object_state;
    floor.occupants = occupants_but_this_player();
    drop_caches(floor);

    std::vector<std::uint8_t> out;
    out.reserve(sizeof(Header) + sizeof(Floor));
    append(out, Header{magic, version, current_floor(), layout});
    append(out, floor);
    return out;
}

bool coop_floor_restore(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != sizeof(Header) + sizeof(Floor))
        return false;
    Header header{};
    std::memcpy(&header, bytes.data(), sizeof header);
    if (header.magic != magic || header.version != version || header.floor != current_floor() || header.layout != layout)
        return false;
    Floor floor{};
    std::memcpy(&floor, bytes.data() + sizeof header, sizeof floor);
    drop_caches(floor);
    if (!floor_valid(floor))
        return false;

    // No cache may keep pointing into a slot that is about to be replaced.
    animation_cache_release_all();
    actor_state.actors = floor.actors;
    actor_state.definitions = floor.definitions;
    effect_state.records = floor.effects;
    map_runtime_state.events = floor.events;
    map_runtime_state.dialogue_advance_gate = floor.dialogue_advance_gate;
    map_runtime_state.ambient_script_countdown = floor.ambient_script_countdown;
    map_object_state = floor.objects;
    put_occupants(floor);
    return true;
}

}
