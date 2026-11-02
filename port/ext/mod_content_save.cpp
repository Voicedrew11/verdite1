#include "mod_content_save.h"

#include <kf/game/actor.h>
#include <kf/lib/floor.h>
#include <kf/lib/item_types.h>
#include <kf/lib/map.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>

namespace kf::ext {
namespace {

static_assert(content_floor_record_bytes == KF_MAP_SAVED_RECORD_BYTES);

constexpr std::array<std::uint8_t, 4> magic = {'K', 'F', 'C', '1'};
constexpr std::size_t stock_row_bytes = 3;

void put_u16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

// Reads forward; every read past the end fails the whole decode.
struct ByteCursor {
    std::span<const std::uint8_t> rest;
    bool ok = true;

    std::span<const std::uint8_t> take(std::size_t count) {
        if (!ok || rest.size() < count) {
            ok = false;
            return {};
        }
        const auto taken = rest.first(count);
        rest = rest.subspan(count);
        return taken;
    }
    std::uint8_t byte() {
        const auto got = take(1);
        return got.empty() ? 0 : got[0];
    }
    std::uint16_t u16() {
        const auto got = take(2);
        return got.empty() ? 0 : static_cast<std::uint16_t>(got[0] | (got[1] << 8));
    }
    std::uint32_t u32() {
        const auto got = take(4);
        if (got.empty())
            return 0;
        return static_cast<std::uint32_t>(got[0]) | (static_cast<std::uint32_t>(got[1]) << 8) |
               (static_cast<std::uint32_t>(got[2]) << 16) | (static_cast<std::uint32_t>(got[3]) << 24);
    }
};

bool stock_row_valid(const ContentStockRow &row) {
    return row.bank < KF_ITEM_STOCK_BANK_COUNT && row.item >= KF_ITEM_MOD_FIRST && kf_item_id_valid(row.item) &&
           row.count != 0 && row.count <= KF_ITEM_STACK_CAPACITY;
}

} // namespace

std::vector<std::uint8_t> content_save_encode(const ContentSave &save) {
    std::vector<std::uint8_t> out(magic.begin(), magic.end());
    put_u16(out, static_cast<std::uint16_t>(save.stock.size()));
    for (const ContentStockRow &row : save.stock) {
        out.push_back(row.bank);
        out.push_back(row.item);
        out.push_back(row.count);
    }
    out.push_back(static_cast<std::uint8_t>(save.floors.size()));
    for (const ContentFloorRow &row : save.floors) {
        out.push_back(row.floor);
        put_u32(out, row.files_hash);
        out.insert(out.end(), row.records.begin(), row.records.end());
    }
    return out;
}

std::optional<ContentSave> content_save_decode(std::span<const std::uint8_t> bytes) {
    ByteCursor in{bytes};
    const auto head = in.take(magic.size());
    if (!in.ok || !std::ranges::equal(head, magic))
        return std::nullopt;
    ContentSave save;
    const std::size_t stock_rows = in.u16();
    if (stock_rows > in.rest.size() / stock_row_bytes)
        return std::nullopt;
    std::set<std::pair<int, int>> seen_stock;
    for (std::size_t i = 0; i < stock_rows; ++i) {
        ContentStockRow row;
        row.bank = in.byte();
        row.item = in.byte();
        row.count = in.byte();
        if (!stock_row_valid(row) || !seen_stock.insert({row.bank, row.item}).second)
            return std::nullopt;
        save.stock.push_back(row);
    }
    const std::size_t floor_rows = in.byte();
    std::set<int> seen_floors;
    for (std::size_t i = 0; i < floor_rows; ++i) {
        ContentFloorRow row;
        row.floor = in.byte();
        row.files_hash = in.u32();
        const auto records = in.take(row.records.size());
        if (!in.ok || !kf_floor_is_mod(row.floor) || !seen_floors.insert(row.floor).second)
            return std::nullopt;
        std::ranges::copy(records, row.records.begin());
        save.floors.push_back(row);
    }
    if (!in.ok || !in.rest.empty())
        return std::nullopt;
    return save;
}

} // namespace kf::ext
