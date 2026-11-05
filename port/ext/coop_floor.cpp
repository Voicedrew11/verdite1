#include "coop_floor.h"

#include "coop_bytes.h"

#include <kf/game/actor.h>
#include <kf/game/animation_cache.h>
#include <kf/game/asset.h>
#include <kf/game/audio.h>
#include <kf/game/collision.h>
#include <kf/game/effect.h>
#include <kf/game/graphics.h>
#include <kf/game/player.h>
#include <kf/game/save.h>
#include <kf/game/state.h>
#include <kf/lib/map.h>
#include <kf/lib/map_data.h>
#include <kf/lib/random.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <type_traits>

namespace kf::ext {
namespace {

constexpr std::array<std::uint8_t, 4> magic = {'K', 'F', 'F', 'L'};
constexpr std::uint8_t version = 2;

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
    std::array<KfEffectSource, KF_EFFECT_CAPACITY> effect_sources;
    std::array<u32, KF_EFFECT_CAPACITY> effect_targets;   // a homing effect's player
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

bool cell_valid(u32 x, u32 z) {
    return x < KF_MAP_COLUMNS && z < KF_MAP_ROWS;
}

// Whether this machine can draw an effect's render id: a billboard it has, or a model clip it has.
bool drawable(KfEffectRenderId render_id, KfAnimationClip clip) {
    if (clip == KF_ANIMATION_CLIP_NONE)
        return render_id.billboard() == KF_EFFECT_BILLBOARD_NONE || render_id.value < KF_EFFECT_BILLBOARD_SPRITE_COUNT;
    if (render_id.model() == KF_EFFECT_MODEL_NONE)
        return true;
    const std::size_t asset = std::size_t{render_id.value} + KF_ASSET_EFFECT_FIRST;
    if (asset >= KF_ASSET_REGISTRY_KNOWN_ENTRIES || !game_graphics_runtime.asset_registry_tmds[asset].data)
        return false;
    const auto &clips = game_graphics_runtime.asset_animations[asset].clips;
    return clips.empty() || kf_enum_encode<std::size_t>(clip) < clips.size();
}

bool drawable(const KfEffectRecord &effect) {
    return drawable(effect.render_id, effect.animation_clip);
}

// A clip this machine's model has, for an actor or an event, which the animation cache refuses
// (ending the game) otherwise: past the model's clips (none is past them all), the last. A machine
// in another of floor 5's variants holds other models for the same actors.
KfAnimationClip fit_clip(std::size_t asset, KfAnimationClip clip) {
    const auto &clips = game_graphics_runtime.asset_animations[asset].clips;
    if (clips.empty() || kf_enum_encode<std::size_t>(clip) < clips.size())
        return clip;
    return kf_enum_decode<KfAnimationClip>(static_cast<u8>(clips.size() - 1));
}

std::size_t actor_model(u8 definition_id) {
    return actor_state.definitions.entries[definition_id].model_and_texture & 0xf;
}

std::size_t event_model(const KfMapEvent &event) {
    return std::size_t{event.model_index} + KF_ASSET_MAP_EVENT_FIRST;
}

bool position_valid(const VECTOR &position) {
    return position.vx >= 0 && position.vx < KF_MAP_COLUMNS * KF_MAP_TILE_SIZE && position.vz >= 0
        && position.vz < KF_MAP_ROWS * KF_MAP_TILE_SIZE;
}

// A spawned actor's placement tile is the invalid cell (actor.cpp), not one on the map.
bool tile_valid(const KfActor &actor) {
    return cell_valid(actor.tile_x, actor.tile_z)
        || (actor.tile_x == KF_MAP_CELL_COORD_INVALID && actor.tile_z == KF_MAP_CELL_COORD_INVALID);
}

// The actor's cell is worked out from its position as it moves, and read from the grids unchecked.
// Every slot's definition, free or not: actor_bind_current points at it for each.
bool actor_valid(const KfActor &actor) {
    if (actor.definition_id >= KF_ACTOR_DEFINITION_COUNT)
        return false;
    if (actor.slot_state == KF_ACTOR_SLOT_FREE)   // drawn and hit while active, though not updated
        return actor.lifecycle != KF_ACTOR_LIFECYCLE_ACTIVE;
    return cell_valid(actor.cell_x, actor.cell_z) && tile_valid(actor) && position_valid(actor.position);
}

// A floor deformation walks the lines effect_floor_deform_line keeps, from first_segment on.
bool floor_deformation_valid(const KfEffectRecord &effect) {
    if (effect.kind != KF_EFFECT_KIND_FLOOR_DEFORMATION)
        return true;
    const auto &floor = effect.floor_deformation;
    return floor.first_segment >= 0 && floor.segment_count >= 0
        && floor.first_segment + floor.segment_count <= KF_FLOOR_DEFORM_SEGMENT_COUNT;
}

// Its render id now and the one its frames count from: the sprite table and the models index by them.
bool effect_valid(const KfEffectRecord &effect) {
    if (effect.type == KF_EFFECT_SLOT_FREE)
        return true;
    const bool shown = (!effect_has_visual(effect) || drawable(effect))
        && drawable(effect.base_render_id, effect.animation_clip);
    return effect.parent_effect_index < KF_EFFECT_CAPACITY && shown && floor_deformation_valid(effect);
}

// As save_world_valid checks a saved event: a stage of 0 would read the page limits before their first.
bool event_valid(const KfMapEvent &event) {
    if (event.state == KF_MAP_EVENT_FREE)
        return true;
    return event.dialogue.stage_limit <= KF_DIALOGUE_STAGE_COUNT && event.dialogue.stage <= KF_DIALOGUE_STAGE_COUNT
        && event.dialogue.stage != 0 && cell_valid(event.cell_x, event.cell_z);
}

bool is_door(KfMapObjectOperation action) {
    return action == KF_MAP_OBJECT_OP_HINGED_DOOR || action == KF_MAP_OBJECT_OP_LIFT_DOOR;
}

// map_object_pool_update runs every slot by its action, whatever its id: an emptied slot keeps its
// action (a taken pickup, floor 5's copy regions), and a falling drop reads the grid at its cell. A
// door's link names its partner's slot, which the door's swing writes, and a door's collision edge
// reads its object's definition, which an empty slot has not.
bool object_valid(const KfMapObject &object) {
    if (!map_saved_link_valid(object.action, object.link))
        return false;
    const bool door = is_door(object.action);
    if (object.object_id == KF_OBJECT_NONE)
        return object.action == KF_MAP_OBJECT_OP_NONE || (!door && cell_valid(object.cell_x, object.cell_z));
    return kf_enum_encode<u8>(object.object_id) < KF_MAP_OBJECT_DEFINITION_COUNT
        && cell_valid(object.cell_x, object.cell_z);
}

bool occupants_valid(const Floor &floor) {
    return std::ranges::all_of(floor.occupants, [](const auto &row) {
        return std::ranges::all_of(row, [](u8 count) { return (count & ~KF_CELL_OCCUPANT_COUNT_MASK) == 0; });
    });
}

// A placed object's link also as the action its definition starts when it is used reads it (as
// save_world_valid checks a saved one); drops have no such action.
bool links_fit_definitions(const KfMapObjectState &objects) {
    for (std::size_t i = 0; i < KF_MAP_OBJECT_EFFECT_FIRST; ++i) {
        const KfMapObject &object = objects.objects[i];
        if (object.object_id != KF_OBJECT_NONE
            && !map_saved_link_valid(objects.definitions.entries[kf_enum_encode<u8>(object.object_id)].behavior_type,
                object.link))
            return false;
    }
    return true;
}

bool floor_valid(const Floor &floor) {
    return std::ranges::all_of(floor.actors, actor_valid) && std::ranges::all_of(floor.effects, effect_valid)
        && std::ranges::all_of(floor.events, event_valid) && std::ranges::all_of(floor.objects.objects, object_valid)
        && links_fit_definitions(floor.objects)
        && occupants_valid(floor) && std::ranges::all_of(floor.effect_sources, [](KfEffectSource source) {
               return source == KfEffectSource::Other || source == KfEffectSource::Actor
                   || source == KfEffectSource::Player;
           });
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

// What the disc gave this machine for the same floor, which play never changes, in place of the
// sender's: the definition tables (but the floor 5 boss's animations, which follow the story flag
// the record carries), each actor's placement drop and each event's placement. A shop's character
// is its stock bank, and an event's model an asset. False when an event slot is free on one machine
// and not the other: only loading the floor frees or fills one.
bool keep_own_placements(Floor &floor) {
    floor.definitions = actor_state.definitions;
    floor.objects.definitions = map_object_state.definitions;
    for (std::size_t i = 0; i < KF_ACTOR_CAPACITY; ++i)
        floor.actors[i].death_drop_object_id = actor_state.actors[i].death_drop_object_id;
    for (std::size_t i = 0; i < KF_MAP_EVENT_CAPACITY; ++i) {
        const KfMapEvent &own = map_runtime_state.events[i];
        KfMapEvent &sent = floor.events[i];
        if ((own.state == KF_MAP_EVENT_FREE) != (sent.state == KF_MAP_EVENT_FREE))
            return false;
        sent.character_id = own.character_id;
        sent.model_index = own.model_index;
        sent.behavior = own.behavior;
        sent.home_x = own.home_x;
        sent.home_z = own.home_z;
        sent.radius = own.radius;
        sent.unknown_0c = own.unknown_0c;
        sent.unknown_0d = own.unknown_0d;
    }
    return true;
}

// Each actor's and event's clip, which is drawn while it is active, one its model here has.
void fit_clips(Floor &floor) {
    for (KfActor &actor : floor.actors)
        if (actor.lifecycle == KF_ACTOR_LIFECYCLE_ACTIVE)
            actor.animation_clip = fit_clip(actor_model(actor.definition_id), actor.animation_clip);
    for (KfMapEvent &event : floor.events)
        if (event.state == KF_MAP_EVENT_ACTIVE)
            event.animation_clip = fit_clip(event_model(event), event.animation_clip);
}

// The live floor in a snapshot of the loaded floor, checked; nothing for bytes that are not one.
std::optional<Floor> snapshot_floor(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != sizeof(Header) + sizeof(Floor))
        return std::nullopt;
    Header header{};
    std::memcpy(&header, bytes.data(), sizeof header);
    if (header.magic != magic || header.version != version || header.floor != current_floor() || header.layout != layout)
        return std::nullopt;
    Floor floor{};
    std::memcpy(&floor, bytes.data() + sizeof header, sizeof floor);
    drop_caches(floor);
    if (!keep_own_placements(floor) || !floor_valid(floor))
        return std::nullopt;
    fit_clips(floor);
    return floor;
}

void put_floor(const Floor &floor) {
    // No cache may keep pointing into a slot that is about to be replaced.
    animation_cache_release_all();
    actor_state.actors = floor.actors;
    actor_state.definitions = floor.definitions;
    effect_state.records = floor.effects;
    // The sender's spells fly on here, but are not this player's to send with its pose.
    std::ranges::transform(floor.effect_sources, effect_sources.begin(), [](KfEffectSource source) {
        return source == KfEffectSource::Player ? KfEffectSource::Other : source;
    });
    effect_target_players = floor.effect_targets;
    map_runtime_state.events = floor.events;
    map_runtime_state.dialogue_advance_gate = floor.dialogue_advance_gate;
    map_runtime_state.ambient_script_countdown = floor.ambient_script_countdown;
    map_object_state = floor.objects;
    put_occupants(floor);
}

} // namespace

std::vector<std::uint8_t> coop_floor_capture() {
    Floor floor{};
    floor.actors = actor_state.actors;
    floor.definitions = actor_state.definitions;
    floor.effects = effect_state.records;
    floor.effect_sources = effect_sources;
    floor.effect_targets = effect_target_players;
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
    const auto floor = snapshot_floor(bytes);
    if (!floor)
        return false;
    put_floor(*floor);
    return true;
}

// ---- the floor's record ----

namespace {

constexpr std::array<std::uint8_t, 4> record_magic = {'K', 'F', 'R', 'C'};
constexpr std::size_t record_header_bytes = record_magic.size() + 2;   // magic, version, floor

// The story flags a floor's record carries: floor 1's, 3's or 5's, as the save keeps them.
std::span<std::uint8_t> floor_flags(KfMapSavedWorld &world, int floor) {
    switch (floor) {
    case 1:
        return {reinterpret_cast<std::uint8_t *>(&world.floor1), sizeof world.floor1};
    case 3:
        return {reinterpret_cast<std::uint8_t *>(&world.floor3), sizeof world.floor3};
    case 5:
        return {reinterpret_cast<std::uint8_t *>(&world.floor5), sizeof world.floor5};
    default:
        return {};
    }
}
static_assert(std::is_trivially_copyable_v<KfMapFloor1Script> && sizeof(KfMapFloor1Script) == 4);
static_assert(std::is_trivially_copyable_v<KfMapFloor5Script> && sizeof(KfMapFloor5Script) == 4);

bool stage_valid(KfMapAreaTriggerStage stage) {
    return kf_enum_encode<u8>(stage) <= kf_enum_encode<u8>(KF_MAP_TRIGGER_COMPLETE);
}

bool flag_valid(KfMapScriptFlag flag) {
    return kf_enum_encode<u8>(flag) <= kf_enum_encode<u8>(KF_MAP_SCRIPT_SET);
}

bool flags_valid(const KfMapSavedWorld &world) {
    const auto &f1 = world.floor1;
    const auto &f5 = world.floor5;
    return stage_valid(f1.object_removal_stage) && stage_valid(f1.actor_activation_stage) && flag_valid(f1.passage_opened)
        && flag_valid(f1.revival_enabled) && world.floor3.revealed_piece_count <= KF_MAP_FLOOR3_REQUIRED_REVEALS
        && flag_valid(f5.character_arrived) && flag_valid(f5.weapon_transformed) && flag_valid(f5.boss_encounter_started)
        && flag_valid(f5.boss_defeat);
}

// A record as it came: its floor, its records and its flags, put into a copy of this world.
struct Record {
    int floor = 0;
    KfMapSavedWorld world;   // this machine's world with the record's floor and flags in it
};

std::optional<Record> record_read(std::span<const std::uint8_t> bytes, const KfMapObjectDefinitionTable *definitions) {
    if (bytes.size() < record_header_bytes || !std::ranges::equal(bytes.first(record_magic.size()), record_magic)
        || bytes[record_magic.size()] != version)
        return std::nullopt;
    Record record{bytes[record_magic.size() + 1], map_runtime_state.world_state};
    if (record.floor < 1 || record.floor > KF_MAP_SAVED_FLOOR_COUNT)
        return std::nullopt;
    auto &floor = record.world.floors[static_cast<std::size_t>(record.floor - 1)];
    const auto flags = floor_flags(record.world, record.floor);
    if (bytes.size() != record_header_bytes + floor.records.size() + flags.size())
        return std::nullopt;
    const auto body = bytes.subspan(record_header_bytes);
    std::ranges::copy(body.first(floor.records.size()), floor.records.begin());
    std::ranges::copy(body.subspan(floor.records.size()), flags.begin());
    if (!save_system_floor_record_valid(floor, definitions) || !flags_valid(record.world))
        return std::nullopt;
    // Floor 1's two-step triggers are each player's until complete.
    const auto &own = map_runtime_state.world_state.floor1;
    auto &kept = record.world.floor1;
    if (kept.object_removal_stage != KF_MAP_TRIGGER_COMPLETE)
        kept.object_removal_stage = own.object_removal_stage;
    if (kept.actor_activation_stage != KF_MAP_TRIGGER_COMPLETE)
        kept.actor_activation_stage = own.actor_activation_stage;
    return record;
}

std::vector<std::uint8_t> record_bytes(KfMapSavedWorld &world, int floor) {
    const auto &records = world.floors[static_cast<std::size_t>(floor - 1)].records;
    std::vector<std::uint8_t> out(record_magic.begin(), record_magic.end());
    out.push_back(version);
    out.push_back(static_cast<std::uint8_t>(floor));
    out.insert(out.end(), records.begin(), records.end());
    const auto flags = floor_flags(world, floor);
    out.insert(out.end(), flags.begin(), flags.end());
    return out;
}

} // namespace

std::vector<std::uint8_t> coop_floor_record(int floor) {
    if (floor < 1 || floor > KF_MAP_SAVED_FLOOR_COUNT)
        return {};
    if (floor == current_floor())
        map_world_state_persist();
    return record_bytes(map_runtime_state.world_state, floor);
}

int coop_floor_record_floor(std::span<const std::uint8_t> record) {
    if (record.size() < record_header_bytes || !std::ranges::equal(record.first(record_magic.size()), record_magic))
        return 0;
    const int floor = record[record_magic.size() + 1];
    return floor >= 1 && floor <= KF_MAP_SAVED_FLOOR_COUNT ? floor : 0;
}

bool coop_floor_keep_record(std::span<const std::uint8_t> bytes) {
    const auto record = record_read(bytes, nullptr);
    if (!record || record->floor == current_floor())
        return false;
    map_runtime_state.world_state = record->world;
    return true;
}


// ---- writes ----

namespace {

constexpr std::uint8_t writes_version = 2;
constexpr std::size_t event_write_bytes = 6;   // index, state, stage limit, stage, page, the stage's last page

// The floor's lasting state as this machine last sent or took it.
struct Baseline {
    int floor = 0;   // 0: nothing yet, start afresh
    std::array<KfMapObject, KF_MAP_OBJECT_CAPACITY> objects{};
    std::array<KfMapEvent, KF_MAP_EVENT_CAPACITY> events{};
    std::vector<std::uint8_t> flags;
};
Baseline baseline;
bool puppets_shown = false;   // some actor on the floor was drawn from another machine's

bool is_drop(std::size_t index) {
    return index >= KF_MAP_OBJECT_EFFECT_FIRST;
}

// Objects whose link holds a transient effect index or an emitter's effect: only their link id is
// the world's.
bool link_id_only(KfMapObjectOperation action) {
    switch (action) {
    case KF_MAP_OBJECT_OP_PROJECTILE_EMITTER:
    case KF_MAP_OBJECT_OP_RELEASE_ORBIT_OR_SHORT_SWING:
    case KF_MAP_OBJECT_OP_RELEASE_LONG_SWING:
    case KF_MAP_OBJECT_OP_EFFECT_SWITCH:
        return true;
    default:
        return false;
    }
}

bool object_changed(std::size_t index, const KfMapObject &was, const KfMapObject &now) {
    if (was.object_id != now.object_id)
        return true;
    if (is_drop(index))
        return now.object_id != KF_OBJECT_NONE && (was.cell_x != now.cell_x || was.cell_z != now.cell_z
            || map_object_gold_amount(was.link) != map_object_gold_amount(now.link));
    const bool started = was.action == KF_MAP_OBJECT_OP_NONE && now.action != KF_MAP_OBJECT_OP_NONE;
    if (link_id_only(now.action))
        return started || was.link.link_id != now.link.link_id;
    return started || map_object_link_bytes(was.link) != map_object_link_bytes(now.link);
}

bool event_changed(const KfMapEvent &was, const KfMapEvent &now) {
    return was.state != now.state || was.dialogue.stage_limit != now.dialogue.stage_limit
        || was.dialogue.stage != now.dialogue.stage
        || !std::ranges::equal(was.dialogue_pages.last_page, now.dialogue_pages.last_page);
}

// The floor's story flags as they are the party's: floor 1's two-step triggers only once complete,
// and none of floor 3's, since each machine counts the map pieces its own objects reveal.
std::vector<std::uint8_t> shared_flags() {
    KfMapSavedWorld world = map_runtime_state.world_state;
    auto &f1 = world.floor1;
    if (f1.object_removal_stage != KF_MAP_TRIGGER_COMPLETE)
        f1.object_removal_stage = KF_MAP_TRIGGER_AWAIT_ENTRY;
    if (f1.actor_activation_stage != KF_MAP_TRIGGER_COMPLETE)
        f1.actor_activation_stage = KF_MAP_TRIGGER_AWAIT_ENTRY;
    const int floor = current_floor();
    if (floor == 3)
        return {};
    const auto flags = floor_flags(world, floor);
    return {flags.begin(), flags.end()};
}

void take_baseline() {
    baseline.floor = current_floor();
    baseline.objects = map_object_state.objects;
    baseline.events = map_runtime_state.events;
    baseline.flags = shared_flags();
}

bool event_write_valid(std::span<const std::uint8_t> write) {
    const auto state = kf_enum_decode<KfMapEventState>(write[1]);
    // Only loading the floor frees or fills an event slot, so a write does neither.
    const bool state_known = state == KF_MAP_EVENT_INACTIVE || state == KF_MAP_EVENT_ACTIVE
        || state == KF_MAP_EVENT_DISABLED;
    return write[0] < KF_MAP_EVENT_CAPACITY && map_runtime_state.events[write[0]].state != KF_MAP_EVENT_FREE
        && state_known && write[2] <= KF_DIALOGUE_STAGE_COUNT && write[3] <= KF_DIALOGUE_STAGE_COUNT;
}

bool drop_id_valid(std::size_t index, KfObjectId id) {
    if (id == KF_OBJECT_NONE)
        return true;
    if (index < KF_MAP_OBJECT_DEFINITION_DROP_FIRST)
        return id == KF_ITEM_GOLD_COIN;
    return kf_enum_encode<u8>(id) < KF_ITEM_COUNT;
}

bool object_write_valid(std::size_t index, const KfMapObject &object) {
    if (index >= KF_MAP_OBJECT_CAPACITY || !object_valid(object))
        return false;
    if (is_drop(index))
        return drop_id_valid(index, object.object_id);
    return object.object_id == KF_OBJECT_NONE
        || map_saved_link_valid(map_object_state.definitions.entries[kf_enum_encode<u8>(object.object_id)].behavior_type,
            object.link);
}

void apply_object(std::size_t index, const KfMapObject &sent) {
    KfMapObject &object = map_object_state.objects[index];
    if (is_drop(index)) {
        if (sent.object_id == KF_OBJECT_NONE) {
            object.object_id = KF_OBJECT_NONE;
            object.action = KF_MAP_OBJECT_OP_NONE;
        } else {
            object = sent;   // a new drop, falling as it fell there
        }
        return;
    }
    // What runs on is this machine's action, not the one the sender named, so the link is checked
    // against that too: a door swinging here would write to whatever slot a forged link names.
    KfMapObject after = object;
    after.object_id = sent.object_id;
    if (link_id_only(after.action))
        after.link.link_id = sent.link.link_id;
    else
        after.link = sent.link;
    if (sent.action != KF_MAP_OBJECT_OP_NONE)
        map_object_start_action_if_idle(&after, sent.action);
    if (after.object_id == KF_OBJECT_NONE && is_door(after.action))
        after.action = KF_MAP_OBJECT_OP_NONE;
    if (object_write_valid(index, after))
        object = after;
}

void apply_event(std::span<const std::uint8_t> write) {
    KfMapEvent &event = map_runtime_state.events[write[0]];
    event.state = kf_enum_decode<KfMapEventState>(write[1]);
    event.animation_clip = fit_clip(event_model(event), event.animation_clip);
    event.dialogue.stage_limit = write[2];
    // A stage is never set lower in coop (map_event_refresh_dialogue_stage).
    if (write[3] > event.dialogue.stage) {
        event.dialogue.stage = write[3];
        event.dialogue.page = KF_DIALOGUE_FIRST_PAGE;
    }
    // An exchange made: its stage ends on another page, so it is made once for the party.
    const u8 stage = event.dialogue.stage;
    if (stage != 0 && stage == write[3] && event.dialogue_pages.last_page[stage - 1] != write[5]) {
        event.dialogue_pages.last_page[stage - 1] = write[5];
        event.dialogue.page = write[4];
        event.dialogue.page_delay = 0;
    }
}

void start_boss_encounter() {
    auto &animations = actor_state.definitions.entries[KF_FLOOR5_BOSS_DEFINITION].action_animations;
    animations[KF_ACTOR_ANIM_SLOT_MELEE] = KF_ANIMATION_CLIP_THIRD;
    animations[KF_ACTOR_ANIM_SLOT_EFFECT0] = KF_ANIMATION_CLIP_FOURTH;
    animations[KF_ACTOR_ANIM_SLOT_EFFECT1] = KF_ANIMATION_CLIP_FOURTH;
    animations[KF_ACTOR_ANIM_SLOT_EFFECT2] = KF_ANIMATION_CLIP_FOURTH;
    animations[KF_ACTOR_ANIM_SLOT_MULTI_HIT_ATTACK] = KF_ANIMATION_CLIP_SECOND;
    map_apply_copy_region(KF_MAP_COPY_FLOOR5_BOSS_ENCOUNTER);
}

void wake_floor1_trigger_actor() {
    const s32 index = actor_pool_find_at_tile(KF_FLOOR1_TRIGGER_ACTOR_TILE_X, KF_FLOOR1_TRIGGER_ACTOR_TILE_Z);
    if (index == -1 || actor_state.actors[static_cast<std::size_t>(index)].lifecycle == KF_ACTOR_LIFECYCLE_ACTIVE)
        return;
    actor_state.actors[static_cast<std::size_t>(index)].lifecycle = KF_ACTOR_LIFECYCLE_DORMANT;
    actor_initialize_slot(static_cast<u16>(index));
}

// What raised flags change on the loaded floor, as the scripts that raise them do.
void apply_flags(std::span<const std::uint8_t> bytes) {
    KfMapSavedWorld sent = map_runtime_state.world_state;
    const auto into = floor_flags(sent, current_floor());
    if (bytes.size() != into.size())
        return;
    std::ranges::copy(bytes, into.begin());
    if (!flags_valid(sent))
        return;
    auto &world = map_runtime_state.world_state;
    auto raise_flag = [](KfMapScriptFlag &flag, KfMapScriptFlag to) {
        const bool raised = flag == KF_MAP_SCRIPT_UNSET && to == KF_MAP_SCRIPT_SET;
        if (raised)
            flag = to;
        return raised;
    };
    auto complete = [](KfMapAreaTriggerStage &stage, KfMapAreaTriggerStage to) {
        const bool completed = stage != KF_MAP_TRIGGER_COMPLETE && to == KF_MAP_TRIGGER_COMPLETE;
        if (completed)
            stage = to;
        return completed;
    };
    switch (current_floor()) {
    case 1:
        complete(world.floor1.object_removal_stage, sent.floor1.object_removal_stage);
        if (complete(world.floor1.actor_activation_stage, sent.floor1.actor_activation_stage))
            wake_floor1_trigger_actor();
        if (raise_flag(world.floor1.passage_opened, sent.floor1.passage_opened))
            map_apply_copy_region(KF_MAP_COPY_FLOOR1_PASSAGE);
        raise_flag(world.floor1.revival_enabled, sent.floor1.revival_enabled);
        break;
    case 5:
        raise_flag(world.floor5.character_arrived, sent.floor5.character_arrived);
        raise_flag(world.floor5.weapon_transformed, sent.floor5.weapon_transformed);
        if (raise_flag(world.floor5.boss_encounter_started, sent.floor5.boss_encounter_started))
            start_boss_encounter();
        raise_flag(world.floor5.boss_defeat, sent.floor5.boss_defeat);
        break;
    default:
        break;
    }
}

} // namespace

std::vector<std::uint8_t> coop_floor_writes() {
    if (baseline.floor != current_floor()) {
        take_baseline();
        return {};
    }
    CoopByteWriter out;
    out.u8(writes_version);
    out.u8(static_cast<std::uint8_t>(sizeof(KfMapObject)));
    std::vector<std::size_t> objects;
    for (std::size_t i = 0; i < KF_MAP_OBJECT_CAPACITY; ++i)
        if (object_changed(i, baseline.objects[i], map_object_state.objects[i]))
            objects.push_back(i);
    out.u8(static_cast<std::uint8_t>(objects.size()));
    for (const std::size_t i : objects) {
        out.u8(static_cast<std::uint8_t>(i));
        KfMapObject object = map_object_state.objects[i];
        append(out.bytes(), object);
    }
    std::vector<std::size_t> events;
    for (std::size_t i = 0; i < KF_MAP_EVENT_CAPACITY; ++i)
        if (event_changed(baseline.events[i], map_runtime_state.events[i]))
            events.push_back(i);
    out.u8(static_cast<std::uint8_t>(events.size()));
    for (const std::size_t i : events) {
        const KfMapEvent &event = map_runtime_state.events[i];
        out.u8(static_cast<std::uint8_t>(i));
        out.u8(kf_enum_encode<u8>(event.state));
        out.u8(event.dialogue.stage_limit);
        out.u8(event.dialogue.stage);
        out.u8(event.dialogue.page);
        out.u8(event.dialogue.stage != 0 ? event.dialogue_pages.last_page[event.dialogue.stage - 1] : 0);
    }
    const auto flags = shared_flags();
    const bool flags_changed = flags != baseline.flags;
    out.u8(flags_changed ? static_cast<std::uint8_t>(flags.size()) : 0);
    if (flags_changed)
        out.raw(flags);
    take_baseline();
    if (objects.empty() && events.empty() && !flags_changed)
        return {};
    return std::move(out.bytes());
}

bool coop_floor_apply_writes(std::span<const std::uint8_t> bytes) {
    CoopByteReader in(bytes);
    if (in.u8() != writes_version || in.u8() != sizeof(KfMapObject) || !in.ok())
        return false;
    std::vector<std::pair<std::size_t, KfMapObject>> objects(in.u8());
    for (auto &[index, object] : objects) {
        index = in.u8();
        const auto raw = in.raw(sizeof object);
        if (!in.ok())
            return false;
        std::memcpy(&object, raw.data(), sizeof object);
        if (!object_write_valid(index, object))
            return false;
    }
    const std::size_t event_count = in.u8();
    const auto events = in.raw(event_count * event_write_bytes);
    const auto flags = in.raw(in.u8());
    if (!in.done())
        return false;
    for (std::size_t i = 0; i < event_count; ++i)
        if (!event_write_valid(events.subspan(i * event_write_bytes, event_write_bytes)))
            return false;
    if (!flags.empty() && flags.size() != shared_flags().size())
        return false;

    if (baseline.floor != current_floor())
        take_baseline();
    for (const auto &[index, object] : objects) {
        apply_object(index, object);
        baseline.objects[index] = map_object_state.objects[index];
    }
    for (std::size_t i = 0; i < event_count; ++i) {
        const auto write = events.subspan(i * event_write_bytes, event_write_bytes);
        apply_event(write);
        baseline.events[write[0]] = map_runtime_state.events[write[0]];
    }
    if (!flags.empty()) {
        apply_flags(flags);
        baseline.flags = shared_flags();
    }
    return true;
}

void coop_floor_forget() {
    baseline.floor = 0;
    puppets_shown = false;
    coop_floor_hide_effects();
}

// ---- puppets ----

namespace {

// An awake actor as the others on its floor draw it.
struct Puppet {
    u8 index;
    u8 definition_id;
    KfAnimationClip animation_clip;
    KfActorAction action;   // whether it goes after a player (coop_death_play.cpp)
    u16 animation_phase;
    u16 health;
    u8 cell_x, cell_z;
    VECTOR position;
    SVECTOR rotation;
};
constexpr std::size_t puppet_bytes = 1 + 1 + 1 + 1 + 2 + 2 + 1 + 1 + 3 * 4 + 3 * 2;

void write_puppet(CoopByteWriter &out, std::size_t index, const KfActor &actor) {
    out.u8(static_cast<std::uint8_t>(index));
    out.u8(actor.definition_id);
    out.u8(kf_enum_encode<u8>(actor.animation_clip));
    out.u8(kf_enum_encode<u8>(actor.action));
    out.u16(actor.animation_phase);
    out.u16(actor.health);
    out.u8(static_cast<std::uint8_t>(actor.cell_x));
    out.u8(static_cast<std::uint8_t>(actor.cell_z));
    out.s32(actor.position.vx);
    out.s32(actor.position.vy);
    out.s32(actor.position.vz);
    out.u16(static_cast<std::uint16_t>(actor.rotation.vx));
    out.u16(static_cast<std::uint16_t>(actor.rotation.vy));
    out.u16(static_cast<std::uint16_t>(actor.rotation.vz));
}

std::optional<Puppet> read_puppet(CoopByteReader &in) {
    Puppet puppet{};
    puppet.index = in.u8();
    puppet.definition_id = in.u8();
    puppet.animation_clip = kf_enum_decode<KfAnimationClip>(in.u8());
    puppet.action = kf_enum_decode<KfActorAction>(in.u8());
    puppet.animation_phase = in.u16();
    puppet.health = in.u16();
    puppet.cell_x = in.u8();
    puppet.cell_z = in.u8();
    puppet.position = {in.s32(), in.s32(), in.s32()};
    puppet.rotation = {static_cast<s16>(in.u16()), static_cast<s16>(in.u16()), static_cast<s16>(in.u16())};
    if (!in.ok() || puppet.index >= KF_ACTOR_CAPACITY || puppet.definition_id >= KF_ACTOR_DEFINITION_COUNT
        || !cell_valid(puppet.cell_x, puppet.cell_z) || !position_valid(puppet.position))
        return std::nullopt;
    return puppet;
}

bool awake(const KfActor &actor) {
    return actor.slot_state != KF_ACTOR_SLOT_FREE && actor.lifecycle == KF_ACTOR_LIFECYCLE_ACTIVE;
}

} // namespace

namespace {

// An actor's sound for each action that has one, as actor_play_sound_at_phase plays them.
struct ActionSound {
    std::size_t action;   // KF_ACTOR_ANIM_SLOT_*
    std::size_t sound;    // KF_ACTOR_SOUND_*
};
constexpr std::array<ActionSound, 6> action_sounds = {{
    {KF_ACTOR_ANIM_SLOT_MELEE, KF_ACTOR_SOUND_ATTACK},
    {KF_ACTOR_ANIM_SLOT_JUMP_ATTACK, KF_ACTOR_SOUND_ATTACK},
    {KF_ACTOR_ANIM_SLOT_SPECIAL_ATTACK, KF_ACTOR_SOUND_ATTACK},
    {KF_ACTOR_ANIM_SLOT_MULTI_HIT_ATTACK, KF_ACTOR_SOUND_ATTACK},
    {KF_ACTOR_ANIM_SLOT_HIT_REACTION, KF_ACTOR_SOUND_HIT_REACTION},
    {KF_ACTOR_ANIM_SLOT_DEATH, KF_ACTOR_SOUND_DEATH},
}};

void play_at(const SoundRef &sound, const KfActor &actor) {
    if (current_floor() == 5 && actor.definition_id == KF_FLOOR5_BOSS_DEFINITION)
        audio_play_spatial_range(&sound, &actor.position, KF_AUDIO_MAX_VOLUME, KF_AUDIO_EXTENDED_MAX_DISTANCE,
            KF_AUDIO_EXTENDED_ATTENUATION_DISTANCE);
    else
        audio_play_spatial_default_range(&sound, &actor.position, KF_AUDIO_MAX_VOLUME);
}

// The sound the owner's actor played between two frames: its clip went on past the action's sound.
void play_puppet_sound(const KfActor &was, const KfActor &now) {
    if (was.lifecycle != KF_ACTOR_LIFECYCLE_ACTIVE || was.definition_id != now.definition_id
        || was.animation_clip != now.animation_clip || now.animation_phase <= was.animation_phase)
        return;
    const KfActorDefinition &definition = actor_state.definitions.entries[now.definition_id];
    for (const ActionSound &entry : action_sounds) {
        const u16 phase = definition.action_animation_phases[entry.action];
        if (definition.action_animations[entry.action] == now.animation_clip && phase >= was.animation_phase
            && phase < now.animation_phase) {
            play_at(definition.sounds[entry.sound], now);
            return;
        }
    }
}

// An effect in flight as the others on its floor draw it.
constexpr std::size_t shown_effect_bytes = 1 + 1 + 1 + 1 + 2 + 3 * 4 + 3 * 2 + 3 * 2;

bool sent_to_others(std::size_t index, KfEffectSource source) {
    const KfEffectRecord &effect = effect_state.records[index];
    return effect.type != KF_EFFECT_SLOT_FREE && effect_sources[index] == source && effect_has_visual(effect);
}

void write_effect(CoopByteWriter &out, std::size_t index, const KfEffectRecord &effect) {
    out.u8(static_cast<std::uint8_t>(index));
    out.u8(kf_enum_encode<u8>(effect.kind));
    out.u8(effect.render_id.value);
    out.u8(kf_enum_encode<u8>(effect.animation_clip));
    out.u16(effect.animation_phase);
    out.s32(effect.position.vx);
    out.s32(effect.position.vy);
    out.s32(effect.position.vz);
    out.u16(static_cast<std::uint16_t>(effect.rotation.vx));
    out.u16(static_cast<std::uint16_t>(effect.rotation.vy));
    out.u16(static_cast<std::uint16_t>(effect.rotation.vz));
    out.u16(effect.scale_x);
    out.u16(effect.scale_y);
    out.u16(effect.scale_z);
}

// Into the slot it is drawn from, keeping the slot's animation cache.
bool read_effect(CoopByteReader &in, std::array<KfEffectRecord, KF_EFFECT_CAPACITY> &shown) {
    const std::uint8_t index = in.u8();
    KfEffectRecord effect{};
    effect.type = KF_EFFECT_TYPE_NONE;
    effect.kind = kf_enum_decode<KfEffectKind>(in.u8());
    effect.render_id.value = in.u8();
    effect.base_render_id = effect.render_id;
    effect.animation_clip = kf_enum_decode<KfAnimationClip>(in.u8());
    effect.animation_phase = in.u16();
    effect.position = {in.s32(), in.s32(), in.s32()};
    effect.rotation = {static_cast<s16>(in.u16()), static_cast<s16>(in.u16()), static_cast<s16>(in.u16())};
    effect.scale_x = in.u16();
    effect.scale_y = in.u16();
    effect.scale_z = in.u16();
    if (!in.ok() || index >= KF_EFFECT_CAPACITY || kf_enum_encode<u8>(effect.kind) >= KF_MAGIC_RECORD_COUNT
        || !drawable(effect))
        return false;
    effect.animation_cache = shown[index].animation_cache;
    shown[index] = effect;
    return true;
}

// The launch and impact sounds of the owner's effects, as they come and go between two frames.
void play_effect_sounds(const std::array<KfEffectRecord, KF_EFFECT_CAPACITY> &was,
    const std::array<KfEffectRecord, KF_EFFECT_CAPACITY> &now) {
    for (std::size_t i = 0; i < KF_EFFECT_CAPACITY; ++i) {
        const bool came = now[i].type != KF_EFFECT_SLOT_FREE && (was[i].type == KF_EFFECT_SLOT_FREE || was[i].kind != now[i].kind);
        const bool went = was[i].type != KF_EFFECT_SLOT_FREE && (now[i].type == KF_EFFECT_SLOT_FREE || was[i].kind != now[i].kind);
        if (went)
            audio_play_spatial_default_range(&effect_state.magic.entries[kf_enum_encode<u8>(was[i].kind)].sounds[1],
                &was[i].position, KF_AUDIO_MAX_VOLUME);
        if (came)
            audio_play_spatial_default_range(&effect_state.magic.entries[kf_enum_encode<u8>(now[i].kind)].sounds[0],
                &now[i].position, KF_AUDIO_MAX_VOLUME);
    }
}

bool read_actors(CoopByteReader &in, std::size_t count, std::array<std::optional<Puppet>, KF_ACTOR_CAPACITY> &shown) {
    for (std::size_t i = 0; i < count; ++i) {
        const auto puppet = read_puppet(in);
        if (!puppet)
            return false;
        shown[puppet->index] = puppet;
    }
    return true;
}

void show_actors(const std::array<std::optional<Puppet>, KF_ACTOR_CAPACITY> &shown) {
    for (std::size_t i = 0; i < KF_ACTOR_CAPACITY; ++i) {
        KfActor &actor = actor_state.actors[i];
        const KfActor was = actor;
        const bool was_counted = actor.lifecycle == KF_ACTOR_LIFECYCLE_ACTIVE;
        if (was_counted)
            collision_adjust_cell_occupancy(actor.cell_x, actor.cell_z, -1);
        const auto &puppet = shown[i];
        if (!puppet) {
            if (was_counted)
                actor.lifecycle = KF_ACTOR_LIFECYCLE_DORMANT;
            continue;
        }
        if (actor.slot_state == KF_ACTOR_SLOT_FREE)
            actor.slot_state = KF_ACTOR_SLOT_DYNAMIC;
        actor.lifecycle = KF_ACTOR_LIFECYCLE_ACTIVE;
        actor.definition_id = puppet->definition_id;
        actor.animation_clip = fit_clip(actor_model(puppet->definition_id), puppet->animation_clip);
        actor.action = puppet->action;
        actor.animation_phase = puppet->animation_phase;
        actor.health = puppet->health;
        actor.cell_x = puppet->cell_x;
        actor.cell_z = puppet->cell_z;
        actor.position = puppet->position;
        actor.rotation = puppet->rotation;
        collision_adjust_cell_occupancy(actor.cell_x, actor.cell_z, 1);
        play_puppet_sound(was, actor);
    }
}

} // namespace

std::vector<std::uint8_t> coop_floor_frame() {
    CoopByteWriter actors;
    std::size_t actor_count = 0;
    for (std::size_t i = 0; i < KF_ACTOR_CAPACITY; ++i)
        if (awake(actor_state.actors[i])) {
            write_puppet(actors, i, actor_state.actors[i]);
            ++actor_count;
        }
    CoopByteWriter out;
    out.u8(static_cast<std::uint8_t>(actor_count));
    out.raw(actors.bytes());
    for (std::size_t i = 0; i < KF_EFFECT_CAPACITY; ++i)
        if (sent_to_others(i, KfEffectSource::Actor))
            write_effect(out, i, effect_state.records[i]);
    return std::move(out.bytes());
}

static_assert(coop_floor_shown_players == KF_EFFECT_SHOWN_PLAYERS);

std::vector<std::uint8_t> coop_floor_player_effects(std::size_t most_bytes) {
    CoopByteWriter out;
    for (std::size_t i = 0; i < KF_EFFECT_CAPACITY && out.bytes().size() + shown_effect_bytes <= most_bytes; ++i)
        if (sent_to_others(i, KfEffectSource::Player))
            write_effect(out, i, effect_state.records[i]);
    return std::move(out.bytes());
}

bool coop_floor_show_player_effects(std::size_t player, std::span<const std::uint8_t> effects_bytes) {
    if (player >= KF_EFFECT_SHOWN_PLAYERS)
        return false;
    auto &shown = effect_player_shown_records[player];
    std::array<KfEffectRecord, KF_EFFECT_CAPACITY> effects = shown;
    for (KfEffectRecord &effect : effects)
        effect.type = KF_EFFECT_SLOT_FREE;
    CoopByteReader in(effects_bytes);
    bool read = effects_bytes.size() % shown_effect_bytes == 0;
    for (std::size_t i = 0; read && i < effects_bytes.size() / shown_effect_bytes; ++i)
        read = read_effect(in, effects);
    if (!read) {
        coop_floor_hide_player_effects(player);
        return false;
    }
    play_effect_sounds(shown, effects);
    shown = effects;
    return true;
}

void coop_floor_hide_player_effects(std::size_t player) {
    if (player >= KF_EFFECT_SHOWN_PLAYERS)
        return;
    for (KfEffectRecord &effect : effect_player_shown_records[player])
        effect.type = KF_EFFECT_SLOT_FREE;
}

bool coop_floor_show_frame(std::span<const std::uint8_t> bytes) {
    CoopByteReader in(bytes);
    const std::size_t actor_count = in.u8();
    if (!in.ok() || bytes.size() < 1 + actor_count * puppet_bytes
        || (bytes.size() - 1 - actor_count * puppet_bytes) % shown_effect_bytes != 0)
        return false;
    std::array<std::optional<Puppet>, KF_ACTOR_CAPACITY> actors{};
    if (!read_actors(in, actor_count, actors))
        return false;
    std::array<KfEffectRecord, KF_EFFECT_CAPACITY> effects = effect_shown_records;
    for (KfEffectRecord &effect : effects)
        effect.type = KF_EFFECT_SLOT_FREE;
    const std::size_t effect_count = (bytes.size() - 1 - actor_count * puppet_bytes) / shown_effect_bytes;
    for (std::size_t i = 0; i < effect_count; ++i)
        if (!read_effect(in, effects))
            return false;
    show_actors(actors);
    play_effect_sounds(effect_shown_records, effects);
    effect_shown_records = effects;
    puppets_shown = true;
    return true;
}

void coop_floor_hide_effects() {
    for (KfEffectRecord &effect : effect_shown_records)
        effect.type = KF_EFFECT_SLOT_FREE;
}

// ---- running the floor again ----

bool coop_floor_reload(std::span<const std::uint8_t> bytes, std::span<const std::uint8_t> live) {
    auto record = record_read(bytes, &map_object_state.definitions);
    const auto floor_now = live.empty() ? std::nullopt : snapshot_floor(live);
    if (!record || record->floor != current_floor() || (!live.empty() && !floor_now))
        return false;
    // Actors drawn from another machine's are not the floor's own: they need the floor loaded, or
    // the live floor put over them.
    if ((floor_now || !puppets_shown) && coop_floor_record(record->floor) == record_bytes(record->world, record->floor)) {
        if (floor_now)
            put_floor(*floor_now);
        coop_floor_forget();
        return true;
    }
    // As a machine taking a floor loads it: its player and random stream are its own.
    const u32 random = random_peek();
    KfPlayerState player = player_state;
    player.weapon_animation_cache = nullptr;
    map_unload_floor();
    map_runtime_state.world_state = record->world;
    map_load_floor();
    player_state = player;
    random_seed(random);
    // The live floor was checked against the floor as it was; it is checked again as loaded.
    if (const auto floor = live.empty() ? std::nullopt : snapshot_floor(live))
        put_floor(*floor);
    coop_floor_forget();
    return true;
}

}
