#include <kf/platform/patch_archive.h>

namespace kf {
std::optional<std::vector<u8>> patch_archive_download(const char *, std::string *error) {
    *error = "This iOS build does not download translation patches. Import your English-patched disc image instead.";
    return std::nullopt;
}
std::optional<std::vector<u8>> patch_from_archive(std::span<const u8>, std::string *error) {
    *error = "Extract the .ppf file from the archive before importing it on iOS.";
    return std::nullopt;
}
}
