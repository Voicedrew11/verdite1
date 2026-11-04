#include "../src/rated_file.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdio>
#include <vector>

using namespace vo;

TEST_CASE("a rated file's ids are 8 little-endian bytes each") {
    const std::array<std::uint8_t, 16> bytes = {1, 0, 0, 0, 0, 0, 0, 0, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    CHECK(parse_rated_ids(bytes) == std::set<std::uint64_t>{1, 0x1122334455667788u});
}

TEST_CASE("a last id cut short is left out, and nothing is read from nothing") {
    const std::array<std::uint8_t, 11> bytes = {2, 0, 0, 0, 0, 0, 0, 0, 9, 9, 9};
    CHECK(parse_rated_ids(bytes) == std::set<std::uint64_t>{2});
    CHECK(parse_rated_ids({}).empty());
}

TEST_CASE("ids past the most are not read") {
    std::vector<std::uint8_t> bytes((rated_ids_most + 1) * 8);
    for (std::size_t i = 0; i <= rated_ids_most; ++i) {
        bytes[i * 8] = static_cast<std::uint8_t>(i & 0xff);
        bytes[i * 8 + 1] = static_cast<std::uint8_t>((i >> 8) & 0xff);
        bytes[i * 8 + 2] = static_cast<std::uint8_t>((i >> 16) & 0xff);
    }
    CHECK(parse_rated_ids(bytes).size() == rated_ids_most);
}

TEST_CASE("a rating appended is read back; a missing file holds none") {
    const std::string path = "rated_file_test.rated";
    std::remove(path.c_str());
    CHECK(read_rated_ids(path).empty());
    CHECK(append_rated_id(path, 7));
    CHECK(append_rated_id(path, 0xfedcba9876543210u));
    CHECK(read_rated_ids(path) == std::set<std::uint64_t>{7, 0xfedcba9876543210u});
    std::remove(path.c_str());
}
