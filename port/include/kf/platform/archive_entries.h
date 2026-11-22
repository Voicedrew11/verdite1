#ifndef KF_PLATFORM_ARCHIVE_ENTRIES_H
#define KF_PLATFORM_ARCHIVE_ENTRIES_H
#include <kf/lib/types.h>
#include <verdite/mod_archive.h>

#include <span>
#include <string>
#include <vector>

namespace kf {
// The regular files in an archive, a .zip above all, each with its path as the archive spells it.
// Folders and links are left out. Android and Windows read it in-process with libarchive
// (patch_unpack_libarchive.cpp), Linux and the Mac with the system's bsdtar (patch_archive_tools.cpp);
// iOS reads none. The paths are not trusted: verdite::mod_install_plan checks them.
struct ArchiveRead {
    std::vector<verdite::ModArchiveEntry> entries;
    std::string error;   // for the player; empty when the archive was read
};

// Refuses an archive whose files together pass verdite::mod_archive_bytes_most, whatever its
// headers claim. Slow for a large archive, so not on the game's update path.
ArchiveRead archive_entries(std::span<const u8> archive);
} // namespace kf

#endif // KF_PLATFORM_ARCHIVE_ENTRIES_H
