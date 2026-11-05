// libFuzzer: the game state another player in a room sends, put into this game the coop way: a
// floor's live snapshot, its writes, its owner's frame, a player's spells, a floor's record, and
// the host's character and world for a guest. The first byte says which, the second the floor.
//
// A snapshot, a record and a character are made by this machine first, then changed by the input
// (each change: a little-endian u32 offset, taken modulo the size, a length byte, then the bytes),
// so the fuzzer gets past their sizes and headers to their fields. A snapshot the game takes is run
// for a few world updates, as the floor's owner would run it: what it holds must not reach past an
// array there either.
#include "../../ext/coop_floor.h"

#include <kf/game/actor.h>
#include <kf/game/effect.h>
#include <kf/game/player.h>
#include <kf/game/save.h>
#include <kf/lib/map.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

void actor_reset_module_state();
void actor_pool_reset_module_state();
void map_object_pool_reset_module_state();
void map_object_reset_module_state();
void map_events_reset_module_state();
void effect_pool_reset_module_state();
void effect_update_reset_module_state();
void effect_dispatch_reset_module_state();
void player_core_reset_module_state();
void collision_reset_module_state();

namespace {

using namespace kf::ext;

constexpr int world_updates = 8;

void reset_game() {
    actor_reset_module_state();
    actor_pool_reset_module_state();
    map_object_pool_reset_module_state();
    map_object_reset_module_state();
    map_events_reset_module_state();
    effect_pool_reset_module_state();
    effect_update_reset_module_state();
    effect_dispatch_reset_module_state();
    player_core_reset_module_state();
    collision_reset_module_state();
}

// The input's changes applied to bytes.
std::vector<std::uint8_t> changed(std::vector<std::uint8_t> bytes, std::span<const std::uint8_t> changes) {
    while (!bytes.empty() && changes.size() >= 5) {
        const std::uint32_t offset = static_cast<std::uint32_t>(changes[0] | changes[1] << 8 | changes[2] << 16)
            | static_cast<std::uint32_t>(changes[3]) << 24;
        const std::size_t length = std::min<std::size_t>(changes[4], changes.size() - 5);
        const std::size_t at = offset % bytes.size();
        const std::size_t count = std::min(length, bytes.size() - at);
        std::copy_n(changes.begin() + 5, count, bytes.begin() + static_cast<std::ptrdiff_t>(at));
        changes = changes.subspan(5 + length);
    }
    return bytes;
}

void run_world() {
    for (int i = 0; i < world_updates; ++i) {
        actor_pool_update();
        map_object_pool_update();
        effect_pool_update();
        map_event_pool_update();
    }
}

void snapshot(std::span<const std::uint8_t> changes) {
    if (coop_floor_restore(changed(coop_floor_capture(), changes)))
        run_world();
}

void record(int floor, std::span<const std::uint8_t> changes) {
    // Kept for a floor this machine has not loaded, as the host keeps an owner's.
    const int other = floor % KF_MAP_SAVED_FLOOR_COUNT + 1;
    (void)coop_floor_keep_record(changed(coop_floor_record(other), changes));
}

void character_and_world(std::span<const std::uint8_t> changes) {
    const std::size_t half = changes.size() / 2;
    const auto character = changed(save_system_coop_character(), changes.first(half));
    const auto world = changed(save_system_coop_world(), changes.subspan(half));
    std::array<u8, KF_ITEM_COUNT> pack{};
    std::copy_n(item_stock[kf_enum_encode<u8>(KF_ITEM_STOCK_PLAYER)].begin(), pack.size(), pack.begin());
    (void)save_system_coop_apply(character, world, pack, player_state.gold);
}

constexpr std::size_t seeded_actors = 8;
constexpr std::size_t seeded_events = 4;

// A floor as one loads: a few actors and events in their slots, every other slot free.
void seed_floor() {
    for (std::size_t i = 0; i < KF_ACTOR_CAPACITY; ++i) {
        KfActor &actor = actor_state.actors[i];
        actor = KfActor{};
        actor.slot_state = i < seeded_actors ? KF_ACTOR_SLOT_PERSISTENT : KF_ACTOR_SLOT_FREE;
        actor.lifecycle = i < seeded_actors ? KF_ACTOR_LIFECYCLE_ACTIVE : KF_ACTOR_LIFECYCLE_DORMANT;
        actor.tile_x = actor.tile_z = static_cast<u8>(10 + i);
        actor.cell_x = actor.cell_z = static_cast<u16>(10 + i);
        actor.position = {actor.cell_x * KF_MAP_TILE_SIZE + KF_MAP_TILE_CENTER, 0,
                          actor.cell_z * KF_MAP_TILE_SIZE + KF_MAP_TILE_CENTER};
    }
    for (std::size_t i = 0; i < KF_MAP_EVENT_CAPACITY; ++i) {
        KfMapEvent &event = map_runtime_state.events[i];
        event = KfMapEvent{};
        event.state = i < seeded_events ? KF_MAP_EVENT_ACTIVE : KF_MAP_EVENT_FREE;
        event.dialogue.stage_limit = event.dialogue.stage = 1;
        event.animation_clip = KF_ANIMATION_CLIP_NONE;
        event.cell_x = event.cell_z = static_cast<u16>(20 + i);
    }
    for (KfEffectRecord &effect : effect_state.records)
        effect.type = KF_EFFECT_SLOT_FREE;
    map_object_pool_clear();
    // The kinds of object a floor's table holds; left zero, every definition would be a hinged door.
    static constexpr std::array behaviours = {KF_MAP_OBJECT_OP_HINGED_DOOR, KF_MAP_OBJECT_OP_LIFT_DOOR,
        KF_MAP_OBJECT_OP_HINGED_CONTAINER, KF_MAP_OBJECT_OP_COPY_REGION, KF_MAP_OBJECT_OP_RESTORE_POINT,
        KF_MAP_OBJECT_OP_ITEM_PICKUP, KF_MAP_OBJECT_OP_PROJECTILE_EMITTER, KF_MAP_OBJECT_OP_EFFECT_SWITCH};
    for (std::size_t i = 0; i < KF_MAP_OBJECT_DEFINITION_COUNT; ++i)
        map_object_state.definitions.entries[i].behavior_type = behaviours[i % behaviours.size()];
}

void start_on(int floor) {
    reset_game();
    seed_floor();
    player_state.progress_state.current_floor = kf_enum_decode<KfFloorId>(static_cast<u8>(floor));
    player_state.progress_state.highest_floor = player_state.progress_state.current_floor;
    player_state.vitals.maximum_hp = player_state.vitals.maximum_mp = 1;
}

// A write starting a hinged door with a fair link, then one in the same packet naming a slot past the
// pool under the swing it started: valid each alone, as the sender's own action is none. The door's
// update writes to the slot its link names, so that link may never reach it.
void check_forged_door_refused() {
    start_on(1);
    constexpr std::size_t lift_door_definition = 1;   // seed_floor's: its link is not limited
    KfMapObject fair{};
    fair.object_id = kf_enum_decode<KfObjectId>(static_cast<u8>(lift_door_definition));
    fair.cell_x = fair.cell_z = 10;
    fair.link.action_parameter = KF_MAP_OBJECT_PARAMETER_NONE;
    fair.action = KF_MAP_OBJECT_OP_HINGED_DOOR;
    KfMapObject forged = fair;
    forged.link.action_parameter = KF_MAP_OBJECT_CAPACITY + 10;
    forged.action = KF_MAP_OBJECT_OP_NONE;
    std::vector<std::uint8_t> writes = {2, static_cast<std::uint8_t>(sizeof(KfMapObject)), 2};
    for (const KfMapObject &object : {fair, forged}) {
        writes.push_back(0);   // the slot
        const auto *bytes = reinterpret_cast<const std::uint8_t *>(&object);
        writes.insert(writes.end(), bytes, bytes + sizeof object);
    }
    writes.insert(writes.end(), {0, 0});   // no events, no flags
    if (!coop_floor_apply_writes(writes) || map_object_state.objects[0].link.action_parameter != KF_MAP_OBJECT_PARAMETER_NONE) {
        std::fprintf(stderr, "a forged link reached a door's swing\n");
        std::abort();
    }
}

} // namespace

// The harness tests nothing if the game refuses its own snapshot unchanged.
extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    for (int floor = 1; floor <= KF_MAP_SAVED_FLOOR_COUNT; ++floor) {
        start_on(floor);
        if (!coop_floor_restore(coop_floor_capture())) {
            std::fprintf(stderr, "floor %d refuses its own unchanged snapshot\n", floor);
            std::abort();
        }
    }
    check_forged_door_refused();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size < 2)
        return 0;
    const int floor = 1 + data[1] % KF_MAP_SAVED_FLOOR_COUNT;
    start_on(floor);
    const std::span<const std::uint8_t> rest(data + 2, size - 2);
    switch (data[0] % 6) {
    case 0:
        snapshot(rest);
        break;
    case 1:
        // Run too: a write can leave an object's action and link at odds, which only its update reads.
        if (coop_floor_apply_writes(rest))
            run_world();
        break;
    case 2:
        (void)coop_floor_show_frame(rest);
        break;
    case 3:
        if (!rest.empty())
            (void)coop_floor_show_player_effects(rest[0] % coop_floor_shown_players, rest.subspan(1));
        break;
    case 4:
        record(floor, rest);
        break;
    default:
        character_and_world(rest);
        break;
    }
    return 0;
}
