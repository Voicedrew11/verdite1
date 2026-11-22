#ifndef VERDITE_MOD_ARCHIVE_H
#define VERDITE_MOD_ARCHIVE_H
// A mod handed over as one archive, a .zip from a download, and the folder it installs as. Nothing
// here touches the disk: the platform unpacks the archive into entries, the plan says which of them
// are the mod and where each goes, and the game writes them. Every path is checked here, once, so
// the writer can trust the plan.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace verdite {

// A regular file from the archive. Folders and links are not entries.
struct ModArchiveEntry {
    std::string path;                  // as the archive spells it
    std::vector<std::uint8_t> bytes;
};

// More than any mod would be: the files together, and how many.
inline constexpr std::size_t mod_archive_bytes_most = std::size_t{64} * 1024 * 1024;
inline constexpr std::size_t mod_archive_files_most = 4096;

struct ModInstallFile {
    std::string path;                  // relative to the mod's folder, '/' between parts
    std::vector<std::uint8_t> bytes;
};

struct ModInstallPlan {
    std::string refusal;               // why the archive cannot be installed, for the player; empty when it can
    std::string folder;                // the folder under the mods folder: mod.json's id, else the archive's folder
    std::string name;                  // mod.json's name, else the folder, for telling the player
    std::vector<ModInstallFile> files;
};

// The one mod in the entries: mod.json or main.lua at the top, or in a single folder at the top,
// with the files beside it. Files outside the mod's folder, and a Mac's __MACOSX and .DS_Store, are
// left out. `archive_name` is the archive's file name, the folder when nothing else gives one.
ModInstallPlan mod_install_plan(std::vector<ModArchiveEntry> entries, std::string_view archive_name);

} // namespace verdite

#endif // VERDITE_MOD_ARCHIVE_H
