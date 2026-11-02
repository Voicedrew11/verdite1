#pragma once
// Whether the game can load and play a floor a mod brings (docs/CONTENT_LAYER_PLAN.md). The game trusts a
// floor's files and stops with host_fail on much of what is wrong in them, so this reads them first, by the
// same parses, and holds them to what the disc's floors are. Nothing here touches the running game.
#include <kf/lib/map_object_types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace kf::ext {

// A floor's files, as they would lie on the disc.
struct ModFloorFiles {
    std::span<const std::uint8_t> tim;    // MIX.TIM
    std::span<const std::uint8_t> mixa;   // MIXA.DAT
    std::span<const std::uint8_t> mixb;   // MIXB.DAT
    std::span<const std::uint8_t> seq;    // SND0.SEQ
};

inline constexpr std::size_t mod_floor_effect_models = 18;   // KF_ASSET_REGISTRY_KNOWN_ENTRIES - KF_ASSET_EFFECT_FIRST

// What the disc's floors make the game expect of any floor: the buffers made for their files, and the
// effect models and clips the game's code asks for by number.
struct ModFloorNeeds {
    std::size_t tim_bytes = 0, mixa_bytes = 0, mixb_bytes = 0, seq_bytes = 0;
    std::size_t effect_models = 0;
    std::array<std::size_t, mod_floor_effect_models> effect_clips{};
    std::array<KfMapObjectOperation, KF_MAP_OBJECT_DEFINITION_COUNT> object_operations{};
};

// Raises needs to hold a floor of the disc's: its files must parse. Why not, if they do not.
std::optional<std::string> mod_floor_needs_add(ModFloorNeeds &needs, const ModFloorFiles &disc_floor);
// The operation of each object id, from COM/COM.DAT's bytes. False if they are not COM.DAT's shape.
bool mod_floor_needs_read_objects(ModFloorNeeds &needs, std::span<const std::uint8_t> com_dat);

// Nothing if the game can load and play these files with the player entering at (entry_x, entry_z),
// else why not, for the modder.
std::optional<std::string> mod_floor_refusal(const ModFloorFiles &files, const ModFloorNeeds &needs, std::uint8_t entry_x,
                                             std::uint8_t entry_z);

} // namespace kf::ext
