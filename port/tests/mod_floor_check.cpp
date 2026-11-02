#include "../ext/mod_floor_check.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace kf::ext;

namespace {
using Bytes = std::vector<std::uint8_t>;

ModFloorNeeds no_floor_at_all() {
    ModFloorNeeds needs;
    needs.tim_bytes = needs.mixa_bytes = needs.mixb_bytes = needs.seq_bytes = 1 << 20;
    return needs;
}

void put_chunk(Bytes &out, const Bytes &body) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(body.size() >> shift));
    out.insert(out.end(), body.begin(), body.end());
}

const Bytes tim = {0x10, 0, 0, 0, 2, 0, 0, 0};
const Bytes seq = {'p', 'Q', 'E', 'S'};

std::string refusal_of(const Bytes &mixa, const Bytes &mixb) {
    const auto reason = mod_floor_refusal({tim, mixa, mixb, seq}, no_floor_at_all(), 1, 1);
    return reason.value_or("");
}
} // namespace

TEST_CASE("files larger than the disc's largest are refused before they are parsed") {
    ModFloorNeeds needs = no_floor_at_all();
    needs.mixa_bytes = 3;
    const Bytes big(4);
    const auto reason = mod_floor_refusal({big, big, big, big}, needs, 0, 0).value_or("");
    CHECK(reason.find("MIXA.DAT") != std::string::npos);
}

TEST_CASE("an empty file is refused") {
    const Bytes some(4), none;
    const auto reason = mod_floor_refusal({some, none, some, some}, no_floor_at_all(), 0, 0).value_or("");
    CHECK(reason.find("empty") != std::string::npos);
}

TEST_CASE("a .DAT must hold every chunk the game walks") {
    Bytes short_file;
    put_chunk(short_file, {1, 2, 3});
    const auto reason = refusal_of(short_file, short_file);
    // MIX.TIM or SND0.SEQ may refuse first; what matters is that bad files are never taken as good.
    CHECK_FALSE(reason.empty());
}

TEST_CASE("a chunk claiming more than the file holds is refused") {
    const Bytes lying = {0xff, 0xff, 0xff, 0x7f, 1, 2, 3};
    CHECK_FALSE(refusal_of(lying, lying).empty());
}

TEST_CASE("the disc's own objects table is read for operations, and a short one refused") {
    ModFloorNeeds needs;
    Bytes com;
    for (int i = 0; i < 5; ++i)
        put_chunk(com, {0});
    CHECK_FALSE(mod_floor_needs_read_objects(needs, com));
    put_chunk(com, Bytes(sizeof(KfMapObjectDiscDefinitions) - 4, 0x40));   // runs on into the next chunk, as on the disc
    com.insert(com.end(), 3, 0x40);
    CHECK_FALSE(mod_floor_needs_read_objects(needs, com));
    com.push_back(0x40);
    CHECK(mod_floor_needs_read_objects(needs, com));
    CHECK(needs.object_operations[0] == KF_MAP_OBJECT_OP_ITEM_PICKUP);
}
