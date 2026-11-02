#pragma once
// Small numbers for the things mods add, kept from run to run. A mod names its item by a key; a save
// and the game's tables hold a byte. The number a key gets here never changes and is never given to
// another key, so a save keeps meaning the same items whatever mods are on or off today.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace verdite {

// Where a kind of thing's numbers come from.
struct ContentIdRange {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

class ContentIds {
public:
    // Reads what path holds: a missing file is empty, and a line it cannot use is ignored, so a
    // damaged file costs numbers and never hands one out twice.
    explicit ContentIds(std::filesystem::path path);

    // key's number in kind, given and stored on its first use. Nothing if the range is used up, the
    // kind or key is not a plain name (letters, digits, '_', '-', '.', '/', at most 64) or the file
    // cannot be written, since a number that is not stored would change on the next run.
    std::optional<std::uint32_t> assign(std::string_view kind, std::string_view key, ContentIdRange range);

    // The number key already has in kind, if any.
    [[nodiscard]] std::optional<std::uint32_t> find(std::string_view kind, std::string_view key) const;

private:
    struct Entry {
        std::string kind, key;
        std::uint32_t id = 0;
    };
    std::filesystem::path path_;
    std::vector<Entry> entries_;
};

} // namespace verdite
