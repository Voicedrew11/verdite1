#ifndef VERDITE_MOD_SAVED_H
#define VERDITE_MOD_SAVED_H
// The mods a save was made with, kept beside the save, and how they differ from the mods running
// now, so a game can warn before it loads the save. Game-agnostic and pure: the game reads and writes
// the text wherever it keeps its saves.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace verdite {

struct SavedMod {
    std::string id, name, version;   // version may be empty
    friend bool operator==(const SavedMod &, const SavedMod &) = default;
};

inline constexpr std::size_t saved_mods_most = 256;

// JSON text: {"mods":[{"id","name","version"}]}. The list may be empty: a save made with no mods.
std::string saved_mods_encode(std::span<const SavedMod> mods);
// The text comes from a file the player can edit. Nothing unless it is that object with at most
// saved_mods_most entries, each with a non-empty string id; a missing or wrong name is the id, and a
// missing or wrong version empty.
std::optional<std::vector<SavedMod>> saved_mods_decode(std::string_view json);

enum class SavedModChange : std::uint8_t {
    Missing,        // in the save, not running now
    Added,          // running now, not in the save
    OtherVersion,   // in both, at different versions
};

struct SavedModDifference {
    SavedMod mod;   // as it is now, for Added and OtherVersion; as the save has it, for Missing
    SavedModChange change = SavedModChange::Missing;
    friend bool operator==(const SavedModDifference &, const SavedModDifference &) = default;
};

// Empty when they agree. Mods match by id. Sorted by change (in the enum's order), then by name.
std::vector<SavedModDifference> saved_mods_compare(std::span<const SavedMod> in_save, std::span<const SavedMod> running);

} // namespace verdite

#endif // VERDITE_MOD_SAVED_H
