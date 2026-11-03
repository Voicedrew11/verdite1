#ifndef KF_PLATFORM_PATCH_ARCHIVE_H
#define KF_PLATFORM_PATCH_ARCHIVE_H
#include <kf/lib/types.h>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kf {
// The two things getting the translator's patch asks of the platform. The desktop runs the
// system's curl and bsdtar (patch_archive_tools.cpp); a phone has neither, so Android downloads
// through Java and unpacks with libarchive in-process (patch_download_android.cpp,
// patch_unpack_libarchive.cpp). The caller checks what it gets: nothing here is trusted.

// More than any patch archive or patch would be.
inline constexpr std::size_t patch_file_limit = std::size_t{64} * 1024 * 1024;

// The file at an https URL, up to patch_file_limit bytes. On Android, any thread but the UI's.
std::optional<std::vector<u8>> patch_archive_download(const char *url, std::string *error);

// The largest .ppf inside an archive: the translator's .rar, or a .zip. The desktop also opens
// a .7z. Anything else, or an archive with no .ppf, is an error for the player to read.
std::optional<std::vector<u8>> patch_from_archive(std::span<const u8> archive, std::string *error);
} // namespace kf

#endif // KF_PLATFORM_PATCH_ARCHIVE_H
