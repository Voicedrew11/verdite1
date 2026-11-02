#include "../ext/mod_content_save.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace kf::ext;

namespace {
ContentSave a_save() {
    ContentSave save;
    save.stock = {{0, 160, 3}, {1, 254, 99}};
    ContentFloorRow floor;
    floor.floor = 6;
    floor.files_hash = 0x01020304;
    floor.records[0] = 1;
    floor.records[content_floor_record_bytes - 1] = 7;
    save.floors = {floor};
    return save;
}
} // namespace

TEST_CASE("a mod's stock and floors come back as they went") {
    const auto bytes = content_save_encode(a_save());
    CHECK(content_save_decode(bytes) == a_save());
    CHECK(content_save_decode(content_save_encode({})) == ContentSave{});
}

TEST_CASE("a cut file, or a byte past the end, is refused") {
    auto bytes = content_save_encode(a_save());
    for (std::size_t size = 0; size < bytes.size(); ++size)
        CHECK_FALSE(content_save_decode(std::span(bytes).first(size)));
    bytes.push_back(0);
    CHECK_FALSE(content_save_decode(bytes));
}

TEST_CASE("rows out of range are refused") {
    const auto with_stock = [](ContentStockRow row) {
        ContentSave save;
        save.stock = {row};
        return content_save_decode(content_save_encode(save));
    };
    CHECK(with_stock({2, 160, 1}));
    CHECK_FALSE(with_stock({3, 160, 1}));     // no such bank
    CHECK_FALSE(with_stock({0, 79, 1}));      // the card's
    CHECK_FALSE(with_stock({0, 120, 1}));     // a fitting's id
    CHECK_FALSE(with_stock({0, 255, 1}));     // none
    CHECK_FALSE(with_stock({0, 160, 0}));     // nothing to keep
    CHECK_FALSE(with_stock({0, 160, 100}));   // past a stack
    const auto with_floor = [](std::uint8_t number) {
        ContentSave save;
        save.floors.resize(1);
        save.floors[0].floor = number;
        return content_save_decode(content_save_encode(save));
    };
    CHECK(with_floor(6));
    CHECK(with_floor(21));
    CHECK_FALSE(with_floor(5));
    CHECK_FALSE(with_floor(22));
}

TEST_CASE("a row given twice is refused") {
    ContentSave save;
    save.stock = {{0, 160, 1}, {0, 160, 2}};
    CHECK_FALSE(content_save_decode(content_save_encode(save)));
    save.stock.clear();
    save.floors.resize(2);
    save.floors[0].floor = save.floors[1].floor = 6;
    CHECK_FALSE(content_save_decode(content_save_encode(save)));
}

TEST_CASE("the first bytes must be the magic") {
    auto bytes = content_save_encode({});
    bytes[0] ^= 1;
    CHECK_FALSE(content_save_decode(bytes));
}
