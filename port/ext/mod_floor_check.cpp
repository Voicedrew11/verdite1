#include "mod_floor_check.h"

#include "mod_assets.h"

#include <kf/audio/codec.h>
#include <kf/game/actor.h>
#include <kf/game/asset.h>
#include <kf/game/effect.h>
#include <kf/lib/codec.h>
#include <kf/lib/graphics.h>
#include <kf/lib/item_types.h>
#include <kf/lib/map.h>
#include <kf/lib/map_types.h>
#include <kf/lib/render_types.h>
#include <kf/lib/tmd.h>
#include <kf/renderer/constants.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace kf::ext {
namespace {

using Bytes = std::span<const std::uint8_t>;
using Refusal = std::optional<std::string>;

constexpr std::size_t chunk_header_bytes = 4;
constexpr std::size_t saved_link_room =
    (KF_MAP_SAVED_RECORD_BYTES - (1 + KF_MAP_EVENT_CAPACITY * 7) - (1 + KF_ACTOR_CAPACITY * 2) - KF_MAP_OBJECT_CAPACITY - 1 -
     KF_MAP_OBJECT_EFFECT_GROUP_CAPACITY * 4 - 2 * KF_MAP_OBJECT_EFFECT_GROUP_CAPACITY * 3) / 9;
constexpr std::size_t effect_spawners_allowed = KF_EFFECT_CAPACITY / 2;
constexpr std::size_t grids_bytes = sizeof(KfMapAttributeGrid) + sizeof(KfMapGrid) + sizeof(KfMapOrientationGrid) +
                                    sizeof(KfMapGrid) + sizeof(KfMapCollisionGrid);
constexpr std::size_t com_object_chunk = 5;

// The chunks a .DAT is, read in order as map_resources_load walks them.
std::optional<std::vector<Bytes>> chunks_of(Bytes file, std::size_t count) {
    std::vector<Bytes> out;
    for (std::size_t i = 0; i < count; ++i) {
        if (file.size() < chunk_header_bytes)
            return std::nullopt;
        const std::size_t size = file[0] | (static_cast<std::size_t>(file[1]) << 8) |
                                 (static_cast<std::size_t>(file[2]) << 16) | (static_cast<std::size_t>(file[3]) << 24);
        if (size > file.size() - chunk_header_bytes)
            return std::nullopt;
        out.push_back(file.subspan(chunk_header_bytes, size));
        file = file.subspan(chunk_header_bytes + size);
    }
    return out;
}

// The offset of bytes within file, for the game's 4-byte buffer: its TMDs must lie on a word there.
bool word_aligned(Bytes file, Bytes bytes) {
    return (bytes.data() - file.data()) % 4 == 0;
}

template <typename Decode>
Refusal decoded(std::string_view what, Decode decode) {
    try {
        decode();
        return std::nullopt;
    } catch (const kf::codec::Error &error) {
        return std::string(what) + ": " + error.message;
    } catch (const std::runtime_error &error) {
        return std::string(what) + ": " + error.what();
    } catch (const std::bad_alloc &) {
        return std::string(what) + ": out of memory";
    } catch (const std::length_error &) {
        return std::string(what) + ": too large";
    }
}

Refusal tmd_refusal_of(std::string_view what, Bytes file, Bytes tmd) {
    if (!word_aligned(file, tmd))
        return std::string(what) + " does not start on a word";
    if (auto reason = mod_tmd_refusal(tmd))
        return std::string(what) + " " + *reason;
    return std::nullopt;
}

u32 tmd_objects(Bytes tmd) {
    return tmd_read_word(tmd.data() + offsetof(KfTmdHeader, object_count));
}

u32 tmd_first_object_vertices(Bytes tmd) {
    return tmd_read_word(tmd.data() + KF_TMD_HEADER_BYTES + offsetof(KfTmdObject, vertex_count));
}

// An archive of models, as asset_registry_load_tmd_archive reads it. The clip count of each.
struct ArchiveRead {
    Refusal refusal;
    std::vector<std::size_t> clips;
};

ArchiveRead read_archive(std::string_view what, Bytes file, Bytes archive, std::size_t bank_size) {
    ArchiveRead out;
    const auto fail = [&](std::string reason) {
        out.refusal = std::string(what) + ": " + std::move(reason);
        return out;
    };
    if (archive.size() < KF_ASSET_ARCHIVE_HEADER_BYTES)
        return fail("is shorter than its header");
    const std::size_t count = archive[0] | (static_cast<std::size_t>(archive[1]) << 8);
    if (count > bank_size)
        return fail("has more models than the game keeps");
    Bytes rest = archive.subspan(KF_ASSET_ARCHIVE_HEADER_BYTES);
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t used = 0;
        std::size_t clips = 0;
        Refusal inner;
        auto reason = decoded(what, [&] {
            const KfAsset asset(rest);
            const Bytes tmd = asset.tmd();
            inner = tmd_refusal_of(what, file, tmd);
            if (inner)
                return;
            u32 vertices = 0;
            if (asset.animated()) {
                vertices = tmd_first_object_vertices(tmd);
                if (vertices > KF_PROJECTED_VERTEX_CAPACITY) {
                    inner = std::string(what) + ": an animated model has more vertices than the game projects";
                    return;
                }
            }
            clips = asset.animation(vertices).clips.size();
            used = asset.encoded_size();
        });
        if (reason)
            out.refusal = std::move(reason);
        else if (inner)
            out.refusal = std::move(inner);
        if (out.refusal)
            return out;
        out.clips.push_back(clips);
        rest = rest.subspan(used);
    }
    return out;
}

Refusal grids_refusal(Bytes grids, u32 map_objects, std::uint8_t entry_x, std::uint8_t entry_z) {
    if (grids.size() < grids_bytes)
        return "its map grids are short";
    const std::size_t heights_at = sizeof(KfMapAttributeGrid);
    const std::size_t orientations_at = heights_at + sizeof(KfMapGrid);
    const std::size_t collision_at = orientations_at + sizeof(KfMapOrientationGrid) + sizeof(KfMapGrid);
    for (std::size_t cell = 0; cell < KF_MAP_CELL_COUNT; ++cell) {
        const std::uint8_t attribute = grids[cell];
        if (attribute >= KF_MAP_ATTRIBUTE_COUNT && attribute != kf_enum_encode<u8>(KF_MAP_ATTRIBUTE_NONE))
            return "a cell's attribute is past the game's table";
        if (attribute != kf_enum_encode<u8>(KF_MAP_ATTRIBUTE_NONE) && attribute >= map_objects)
            return "a cell names a map model the floor does not have";
        const std::uint8_t orientation = grids[orientations_at + cell];
        if (orientation < kf_enum_encode<u8>(KF_MAP_ORIENT_UNROTATED) || orientation > kf_enum_encode<u8>(KF_MAP_ORIENT_THREE_QUARTER_TURN))
            return "a cell's orientation is not one of the four";
        if (grids[collision_at + cell] > kf_enum_encode<u8>(KF_MAP_CELL_STEP))
            return "a cell's collision is not a kind the game has";
    }
    if (entry_x >= KF_MAP_COLUMNS || entry_z >= KF_MAP_ROWS)
        return "its entrance is off the map";
    if (grids[collision_at + std::size_t{entry_z} * KF_MAP_COLUMNS + entry_x] == kf_enum_encode<u8>(KF_MAP_CELL_BLOCKED))
        return "its entrance is a blocked cell";
    return std::nullopt;
}

Refusal floor_items_refusal(Bytes items) {
    constexpr std::size_t placement_bytes = 12, frame_count_offset = 2, cell_z_offset = 4, cell_x_offset = 5;
    constexpr std::uint8_t frame_count_mask = 0x0f;
    std::size_t count = 0;
    for (std::size_t offset = 0; offset + 2 <= items.size(); offset += placement_bytes) {
        const unsigned base = items[offset] | (static_cast<unsigned>(items[offset + 1]) << 8);
        if (base == kf_enum_encode<u16>(KF_FLOOR_ITEM_END))
            return std::nullopt;
        if (items.size() - offset < placement_bytes || count++ == KF_FLOOR_ITEM_CAPACITY)
            return "its floor sprites are cut short or too many";
        const unsigned frames = items[offset + frame_count_offset] & frame_count_mask;
        if (base >= KF_FLOOR_ITEM_SPRITE_COUNT || frames > KF_FLOOR_ITEM_SPRITE_COUNT - base || items[offset + cell_z_offset] >= KF_MAP_ROWS ||
            items[offset + cell_x_offset] >= KF_MAP_COLUMNS)
            return "a floor sprite is out of range";
    }
    return "its floor sprites have no end";
}

bool object_spawns_effect(KfObjectId id) {
    return id == KF_MAP_OBJECT_ORBITING_PROJECTILE || id == KF_MAP_OBJECT_SHORT_SWING || id == KF_MAP_OBJECT_LONG_SWING ||
           id == KF_MAP_OBJECT_EFFECT_SWITCH;
}

// map_saved_link_valid's test (src/game/map_load.cpp): the game stops at a load on a link that fails it.
bool object_link_valid(KfMapObjectOperation operation, const KfMapObjectLink &link) {
    switch (operation) {
    case KF_MAP_OBJECT_OP_HINGED_CONTAINER:
    case KF_MAP_OBJECT_OP_ITEM_CONTAINER:
        for (unsigned slot = 0; slot < KF_MAP_CONTAINER_ITEM_COUNT; ++slot) {
            const auto item = map_container_item(link, operation, slot);
            if (item != KF_OBJECT_NONE && !kf_item_id_valid(kf_enum_encode<u8>(item)))
                return false;
        }
        return true;
    case KF_MAP_OBJECT_OP_HINGED_DOOR:
    case KF_MAP_OBJECT_OP_HINGED_DOOR_PARTNER:
        return link.action_parameter == KF_MAP_OBJECT_PARAMETER_NONE || link.action_parameter < KF_MAP_OBJECT_CAPACITY;
    case KF_MAP_OBJECT_OP_COPY_REGION:
        return kf_enum_decode<KfMapCopyRegionId>(link.action_parameter) == KF_MAP_COPY_REGION_NONE ||
               link.action_parameter < KF_MAP_COPY_REGION_COUNT;
    default:
        return true;
    }
}

// map_world_state_persist (src/game/map_events.cpp) leaves these out of a save while they stand still.
bool object_link_unsaved(KfMapObjectOperation operation) {
    return operation == KF_MAP_OBJECT_OP_NONE || operation == KF_MAP_OBJECT_OP_SCREEN_IMAGE || operation == KF_MAP_OBJECT_OP_ITEM_PICKUP ||
           operation == KF_MAP_OBJECT_OP_SAVE_POINT || operation == KF_MAP_OBJECT_OP_GOLD_PICKUP;
}

Refusal objects_refusal(Bytes placements, const ModFloorNeeds &needs, u32 entity_objects) {
    std::vector<KfObjectPlacementData> decoded_objects(KF_MAP_OBJECT_CAPACITY);
    std::size_t count = 0;
    if (auto reason = decoded("its objects", [&] {
            count = kf_object_placements_decode(placements, {KF_MAP_COLUMNS, KF_MAP_OBJECT_ID_COUNT, KF_MAP_TILE_SIZE}, decoded_objects);
        }))
        return reason;
    std::size_t spawners = 0, saved_links = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto &placement = decoded_objects[i];
        const auto id = kf_enum_decode<KfObjectId>(placement.object_id);
        if (placement.object_id >= KF_MAP_OBJECT_DEFINITION_COUNT && !kf_item_id_valid(placement.object_id))
            return "an object id is not one the game has";
        if (placement.object_id < KF_MAP_OBJECT_DEFINITION_COUNT && placement.object_id >= entity_objects &&
            placement.object_id < kf_enum_encode<u8>(KF_MAP_OBJECT_RENDER_ID_END))
            return "an object has no model in the floor's objects";
        // A sign's picture is a disc file named from its link, which a floor of a mod's cannot be checked against.
        if (id == KF_MAP_OBJECT_SIGNBOARD || id == KF_MAP_OBJECT_INSCRIPTION_PANEL)
            return "signs and inscriptions are not allowed yet";
        if (object_spawns_effect(id) && ++spawners > effect_spawners_allowed)
            return "it has more moving hazards than the game's effects can hold";
        const auto operation = placement.object_id < KF_MAP_OBJECT_DEFINITION_COUNT ? needs.object_operations[placement.object_id]
                                                                                    : KF_MAP_OBJECT_OP_ITEM_PICKUP;
        if (!object_link_valid(operation, map_object_link_from_bytes(placement.link)))
            return "an object's link is not one the game can load";
        if (!object_link_unsaved(operation) && ++saved_links > saved_link_room)
            return "it has more doors and fittings than a save can hold";
    }
    return std::nullopt;
}

Refusal definition_refusal(const KfActorDefinition &row, const ArchiveRead &models) {
    const std::size_t asset = row.model_and_texture & 0xf;
    const std::size_t texture = row.model_and_texture >> 4;
    if (asset >= models.clips.size())
        return "a monster kind names a model the floor does not have";
    // Only floor 5's three retextured materials exist, and that is the disc's floor.
    if (texture != 0)
        return "a monster kind asks for a texture only floor 5 has";
    for (std::size_t slot = 0; slot < std::size(row.action_animations); ++slot) {
        const KfAnimationClip clip = row.action_animations[slot];
        // A model with no clips is drawn still, whatever its kind asks for (render_bind_instance_vertices).
        if (models.clips[asset] != 0 && clip != KF_ANIMATION_CLIP_NONE && kf_enum_encode<u16>(clip) >= models.clips[asset])
            return "a monster kind plays clip " + std::to_string(kf_enum_encode<u16>(clip)) + " in action slot " + std::to_string(slot) +
                   ", and its model " + std::to_string(asset) + " has " + std::to_string(models.clips[asset]);
    }
    for (const u16 step : row.action_animation_steps)
        if (step == 0)
            return "a monster kind has an animation step of 0";
    // An effect with no clip is never chosen (actor_update_awareness); the disc leaves its code unset.
    constexpr std::array<std::size_t, KF_ACTOR_EFFECT_PARAMETER_COUNT> effect_slots = {
        KF_ACTOR_ANIM_SLOT_EFFECT0, KF_ACTOR_ANIM_SLOT_EFFECT1, KF_ACTOR_ANIM_SLOT_EFFECT2};
    for (std::size_t i = 0; i < effect_slots.size(); ++i)
        if (row.action_animations[effect_slots[i]] != KF_ANIMATION_CLIP_NONE &&
            (kf_enum_encode<u8>(row.action_parameters.effect_codes[i]) & 0x1f) >= KF_ACTOR_ACTION_PROFILE_COUNT)
            return "a monster kind names an action the game does not have";
    if (row.initial_health == 0)
        return "a monster kind has no health";
    return std::nullopt;
}

Refusal actors_refusal(Bytes placements, Bytes definitions, const ArchiveRead &models) {
    std::vector<KfActorPlacementData> decoded_actors(KF_ACTOR_CAPACITY);
    std::size_t count = 0;
    if (auto reason = decoded("its monsters", [&] {
            count = kf_actor_placements_decode(placements, {KF_MAP_COLUMNS, KF_ACTOR_DEFINITION_CAPACITY, KF_MAP_TILE_SIZE}, decoded_actors);
        }))
        return reason;
    const std::size_t rows = std::min<std::size_t>(definitions.size() / sizeof(KfActorDefinition), KF_ACTOR_DEFINITION_CAPACITY);
    for (std::size_t i = 0; i < count; ++i) {
        const auto &placement = decoded_actors[i];
        if (placement.definition_id >= rows)
            return "a monster's kind is not one of the floor's";
        const auto drop = placement.death_drop_object_id;
        if (drop != KF_OBJECT_NONE && drop != KF_MAP_OBJECT_DROP_DISABLED && !kf_item_id_valid(kf_enum_encode<u8>(drop)))
            return "a monster drops an id that is not an item";
    }
    // The disc's own rows past its kinds hold leftovers with no health; nothing makes a monster of those. The
    // game spawns kinds 0, 2 and 4 by number, so a row with no health still draws: it needs model 0.
    if (models.clips.empty())
        return "it has no monster models";
    std::array<bool, KF_ACTOR_DEFINITION_CAPACITY> placed{};
    for (std::size_t i = 0; i < count; ++i)
        placed[decoded_actors[i].definition_id] = true;
    for (std::size_t i = 0; i < rows; ++i) {
        KfActorDefinition row;
        std::memcpy(&row, definitions.data() + i * sizeof row, sizeof row);
        if (row.initial_health == 0 && !placed[i])
            continue;
        if (auto reason = definition_refusal(row, models))
            return reason;
    }
    return std::nullopt;
}

// A character's talk pictures and pages are disc files, so a mod's floor has none yet.
Refusal events_refusal(Bytes placements) {
    if (placements.empty() || placements[0] != 255)
        return "it has people or events, which are not allowed yet";
    return std::nullopt;
}

Refusal sound_bank_refusal(Bytes header, Bytes body) {
    return decoded("its sound bank", [&] {
        KfAudioBankData bank;
        kf_audio_bank_decode(header, body, bank);
    });
}

Refusal size_refusal(std::string_view name, std::size_t size, std::size_t limit) {
    if (size == 0)
        return std::string(name) + " is empty";
    if (size > limit)
        return std::string(name) + " is larger than the disc's largest";
    return std::nullopt;
}

Refusal sizes_refusal(const ModFloorFiles &files, const ModFloorNeeds &needs) {
    if (auto reason = size_refusal("MIX.TIM", files.tim.size(), needs.tim_bytes))
        return reason;
    if (auto reason = size_refusal("MIXA.DAT", files.mixa.size(), needs.mixa_bytes))
        return reason;
    if (auto reason = size_refusal("MIXB.DAT", files.mixb.size(), needs.mixb_bytes))
        return reason;
    return size_refusal("SND0.SEQ", files.seq.size(), needs.seq_bytes);
}

constexpr std::size_t mixa_chunks = 8;   // bank header, bank body, grids, sprites, objects, monsters, kinds, events
constexpr std::size_t mixb_chunks = 5;   // objects, map, event models, effect models, monster models

Refusal effects_refusal(const ArchiveRead &effects, const ModFloorNeeds &needs) {
    if (effects.clips.size() < needs.effect_models)
        return "it has fewer effect models than the game asks for";
    for (std::size_t i = 0; i < needs.effect_models; ++i)
        if (effects.clips[i] < needs.effect_clips[i])
            return "an effect model has fewer clips than the game asks for";
    return std::nullopt;
}

Refusal contents_refusal(const ModFloorFiles &files, const ModFloorNeeds &needs, std::uint8_t entry_x, std::uint8_t entry_z) {
    const auto a = chunks_of(files.mixa, mixa_chunks);
    const auto b = chunks_of(files.mixb, mixb_chunks);
    if (!a)
        return "MIXA.DAT is not made of the chunks the game reads";
    if (!b)
        return "MIXB.DAT is not made of the chunks the game reads";
    if (auto reason = sound_bank_refusal((*a)[0], (*a)[1]))
        return reason;
    if (auto reason = tmd_refusal_of("its objects' model", files.mixb, (*b)[0]))
        return reason;
    if (auto reason = tmd_refusal_of("its map model", files.mixb, (*b)[1]))
        return reason;
    if (auto reason = grids_refusal((*a)[2], tmd_objects((*b)[1]), entry_x, entry_z))
        return reason;
    if (auto reason = floor_items_refusal((*a)[3]))
        return reason;
    if (tmd_objects((*b)[0]) < kf_enum_encode<u8>(KF_MAP_OBJECT_RENDER_ID_END))
        return "its objects' model has fewer objects than the game's items and fittings";
    if (auto reason = objects_refusal((*a)[4], needs, tmd_objects((*b)[0])))
        return reason;
    const ArchiveRead monsters = read_archive("its monster models", files.mixb, (*b)[4], KF_ASSET_MAP_EVENT_FIRST - KF_ASSET_ACTOR_FIRST);
    if (monsters.refusal)
        return monsters.refusal;
    if (auto reason = actors_refusal((*a)[5], (*a)[6], monsters))
        return reason;
    if (auto reason = events_refusal((*a)[7]))
        return reason;
    const ArchiveRead effects = read_archive("its effect models", files.mixb, (*b)[3], KF_ASSET_REGISTRY_KNOWN_ENTRIES - KF_ASSET_EFFECT_FIRST);
    if (effects.refusal)
        return effects.refusal;
    return effects_refusal(effects, needs);
}

} // namespace

std::optional<std::string> mod_floor_needs_add(ModFloorNeeds &needs, const ModFloorFiles &disc_floor) {
    const auto b = chunks_of(disc_floor.mixb, mixb_chunks - 1);   // floor 5's monsters are CHR files
    if (!b)
        return "MIXB.DAT is not made of the chunks the game reads";
    const ArchiveRead effects = read_archive("its effect models", disc_floor.mixb, (*b)[3], KF_ASSET_REGISTRY_KNOWN_ENTRIES - KF_ASSET_EFFECT_FIRST);
    if (effects.refusal)
        return effects.refusal;
    needs.effect_models = std::max(needs.effect_models, effects.clips.size());
    for (std::size_t i = 0; i < effects.clips.size(); ++i)
        needs.effect_clips[i] = std::max(needs.effect_clips[i], effects.clips[i]);
    needs.tim_bytes = std::max(needs.tim_bytes, disc_floor.tim.size());
    needs.mixa_bytes = std::max(needs.mixa_bytes, disc_floor.mixa.size());
    needs.mixb_bytes = std::max(needs.mixb_bytes, disc_floor.mixb.size());
    needs.seq_bytes = std::max(needs.seq_bytes, disc_floor.seq.size());
    return std::nullopt;
}

bool mod_floor_needs_read_objects(ModFloorNeeds &needs, std::span<const std::uint8_t> com_dat) {
    const auto chunks = chunks_of(com_dat, com_object_chunk + 1);
    if (!chunks)
        return false;
    // As the game copies it: the table runs on past its chunk's end, into the next chunk's bytes.
    const Bytes table = com_dat.subspan(static_cast<std::size_t>((*chunks)[com_object_chunk].data() - com_dat.data()));
    if (table.size() < sizeof(KfMapObjectDiscDefinitions))
        return false;
    for (std::size_t id = 0; id < needs.object_operations.size(); ++id) {
        KfMapObjectDefinition row;
        std::memcpy(&row, table.data() + id * sizeof row, sizeof row);
        needs.object_operations[id] = row.behavior_type;
    }
    return true;
}

std::optional<std::string> mod_floor_refusal(const ModFloorFiles &files, const ModFloorNeeds &needs, std::uint8_t entry_x,
                                             std::uint8_t entry_z) {
    if (auto reason = sizes_refusal(files, needs))
        return reason;
    if (auto reason = mod_tim_refusal(files.tim))
        return "MIX.TIM " + *reason;
    if (auto reason = mod_seq_refusal(files.seq))
        return "SND0.SEQ " + *reason;
    return contents_refusal(files, needs, entry_x, entry_z);
}

} // namespace kf::ext
