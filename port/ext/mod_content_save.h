#pragma once
// What the card cannot hold, kept beside a save slot: the stock of a mod's items and the saved state
// of the floors mods add. A pure codec, for a file that comes from outside the game.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace kf::ext {

struct ContentStockRow {
    std::uint8_t bank = 0, item = 0, count = 0;
    friend bool operator==(const ContentStockRow &, const ContentStockRow &) = default;
};

inline constexpr std::size_t content_floor_record_bytes = 1690;   // KF_MAP_SAVED_RECORD_BYTES

struct ContentFloorRow {
    std::uint8_t floor = 0;
    std::uint32_t files_hash = 0;   // the floor's files these records were written against
    std::array<std::uint8_t, content_floor_record_bytes> records{};
    friend bool operator==(const ContentFloorRow &, const ContentFloorRow &) = default;
};

struct ContentSave {
    std::vector<ContentStockRow> stock;
    std::vector<ContentFloorRow> floors;
    friend bool operator==(const ContentSave &, const ContentSave &) = default;
};

std::vector<std::uint8_t> content_save_encode(const ContentSave &save);
// Nothing unless every row is in range: a bank, a mod's item id, a stack, a mod's floor, each once.
std::optional<ContentSave> content_save_decode(std::span<const std::uint8_t> bytes);

} // namespace kf::ext
