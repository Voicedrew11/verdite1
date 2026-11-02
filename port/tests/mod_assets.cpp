#include "../ext/mod_assets.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace kf::ext;

namespace {
using Bytes = std::vector<std::uint8_t>;

const verdite::ModAssetKind &kind(std::size_t index) {
    static const auto kinds = mod_asset_kinds();
    return kinds.at(index);
}

// A direct-colour TIM of one 2x1 rectangle: magic, mode 2, then the pixel block (its size, the
// rectangle, four bytes of texels).
Bytes minimal_tim() {
    return {0x10, 0, 0, 0,  2, 0, 0, 0,  16, 0, 0, 0,  0, 0, 0, 0,  2, 0, 1, 0,
            0x12, 0x34, 0x56, 0x78};
}

// A SEQ with a header (magic, version 1, resolution 480, tempo 500000, two pad bytes) and one
// event: a 96-tick delay, then the end of track.
Bytes minimal_seq() {
    return {0x70, 0x51, 0x45, 0x53,  0, 0, 0, 1,  0x01, 0xe0,  0x07, 0xa1, 0x20,  0, 0,
            0x60, 0xff, 0x2f, 0x00};
}

std::span<const std::uint8_t> view(const Bytes &bytes) { return bytes; }
} // namespace

TEST_CASE("the kinds replaced are .TIM, .SEQ and .TMD, in that order, and nothing else") {
    const auto kinds = mod_asset_kinds();
    REQUIRE(kinds.size() == 3);
    CHECK(kinds[0].suffix == ".TIM");
    CHECK(kinds[1].suffix == ".SEQ");
    CHECK(kinds[2].suffix == ".TMD");
    for (const auto &row : kinds)
        CHECK(row.check != nullptr);
}

TEST_CASE("a .MIM disc path has no kind row, so a MIM replacement is not taken") {
    for (const std::string_view path : {"KF/WEPON/WEP00.MIM", "KF/CHR1.MIM", "KF/CHR3.MIM"})
        for (const auto &row : mod_asset_kinds())
            CHECK_FALSE(path.ends_with(row.suffix));
}

TEST_CASE("a minimal TIM is accepted") {
    CHECK_FALSE(kind(0).check(view(minimal_tim())).has_value());
}

TEST_CASE("a TIM that is not one is refused with a reason, whatever its bytes") {
    const Bytes garbage(64, 0xff);
    const auto reason = kind(0).check(view(garbage));
    REQUIRE(reason.has_value());
    CHECK(reason.value_or("").find("TIM") != std::string::npos);
    CHECK(kind(0).check(std::span<const std::uint8_t>{}).has_value());
}

TEST_CASE("every truncation of a minimal TIM is refused, without a crash") {
    const Bytes tim = minimal_tim();
    for (std::size_t size = 0; size < tim.size(); ++size) {
        const Bytes cut(tim.begin(), tim.begin() + static_cast<std::ptrdiff_t>(size));
        CHECK(kind(0).check(view(cut)).has_value());
    }
}

TEST_CASE("a minimal SEQ is accepted") {
    CHECK_FALSE(kind(1).check(view(minimal_seq())).has_value());
}

TEST_CASE("a SEQ that is not one is refused with a reason, and every truncation of one too") {
    const Bytes garbage(40, 0x5a);
    const auto reason = kind(1).check(view(garbage));
    REQUIRE(reason.has_value());
    CHECK(reason.value_or("").find("SEQ") != std::string::npos);
    const Bytes seq = minimal_seq();
    for (std::size_t size = 0; size < seq.size(); ++size) {
        const Bytes cut(seq.begin(), seq.begin() + static_cast<std::ptrdiff_t>(size));
        CHECK(kind(1).check(view(cut)).has_value());
    }
}

TEST_CASE("a disc size is nothing for a path the disc does not have") {
    CHECK_FALSE(mod_disc_file_size("KF/NO_SUCH_FILE.SEQ").has_value());
    CHECK_FALSE(mod_disc_file_size("KF/../GAME.EXE").has_value());
    CHECK_FALSE(mod_disc_file_size(std::string_view("KF/B1/SND0.SEQ\0x", 16)).has_value());
}

namespace {
// A TMD of one object: three vertices, one normal, one flat triangle (packet mode 0x20). The object's
// arrays sit at payload offsets 28 (vertices), 52 (normal) and 60 (primitives); the file is 88 bytes.
Bytes minimal_tmd() {
    return {
        0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 0,
        28, 0, 0, 0,  3, 0, 0, 0,  52, 0, 0, 0,  1, 0, 0, 0,  60, 0, 0, 0,  1, 0, 0, 0,  0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,  1, 0, 0, 0, 0, 0, 0, 0,  0, 0, 1, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 3, 0, 0x20,  0x10, 0x20, 0x30, 0,  0, 0,  0, 0,  1, 0,  2, 0,
    };
}

Bytes prefix_of(const Bytes &bytes, std::size_t size) {
    return Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
}
} // namespace

TEST_CASE("a minimal TMD is accepted, and a TMD with no object is refused") {
    CHECK_FALSE(kind(2).check(view(minimal_tmd())).has_value());
    Bytes none = minimal_tmd();
    none[8] = 0;
    CHECK(kind(2).check(view(none)).has_value());
}

TEST_CASE("every truncation of a minimal TMD is refused, without a crash") {
    const Bytes tmd = minimal_tmd();
    for (std::size_t size = 0; size < tmd.size(); ++size)
        CHECK(kind(2).check(view(prefix_of(tmd, size))).has_value());
}

TEST_CASE("a TMD face that names a vertex or normal its object lacks is refused") {
    Bytes vertex = minimal_tmd();
    vertex[86] = 3;   // the third corner's vertex index: 2 becomes 3, past the object's three
    CHECK(kind(2).check(view(vertex)).has_value());
    Bytes normal = minimal_tmd();
    normal[80] = 1;   // the face's normal: the object has one, index 0
    CHECK(kind(2).check(view(normal)).has_value());
}

TEST_CASE("a TMD primitive whose body runs past the file, or whose vertices are unaligned, is refused") {
    Bytes body = minimal_tmd();
    body[73] = 4;     // the packet's length in words: 16 bytes of body, 12 in the file
    CHECK(kind(2).check(view(body)).has_value());
    Bytes unaligned = minimal_tmd();
    unaligned[12] = 27;   // the vertex array's offset: odd
    CHECK(kind(2).check(view(unaligned)).has_value());
}

TEST_CASE("garbage is refused as a TMD with a reason, and so is random damage") {
    const Bytes garbage(64, 0xff);
    const auto reason = kind(2).check(view(garbage));
    REQUIRE(reason.has_value());
    CHECK(reason.value_or("").find("is not") != std::string::npos);
    std::mt19937 random(7);
    const Bytes tmd = minimal_tmd();
    for (int trial = 0; trial < 4000; ++trial) {
        Bytes damaged = tmd;
        for (int flips = 0; flips < 1 + static_cast<int>(random() % 3); ++flips)
            damaged[random() % damaged.size()] = static_cast<std::uint8_t>(random());
        (void)kind(2).check(view(damaged));
    }
}
