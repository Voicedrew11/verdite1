#include <kf/platform/archive_entries.h>

namespace kf {
ArchiveRead archive_entries(std::span<const u8>) {
    return {{}, "Unzip the mod first, then copy its folder into the mods folder with the Files app."};
}
} // namespace kf
