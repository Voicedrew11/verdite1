#include "../ext/mod_assets.h"

#include <doctest/doctest.h>

#include <kf/platform/files.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
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

TEST_CASE("the kinds replaced are .TIM, .SEQ and .TMD first, then pictures the disc names otherwise") {
    const auto kinds = mod_asset_kinds();
    REQUIRE(kinds.size() == 37);
    CHECK(kinds[0].suffix == ".TIM");
    CHECK(kinds[1].suffix == ".SEQ");
    CHECK(kinds[2].suffix == ".TMD");
    std::size_t bare = 0, inside = 0;
    for (std::size_t i = 3; i < kinds.size(); ++i) {
        const std::string_view suffix = kinds[i].suffix;
        if (suffix.find('#') != std::string_view::npos)
            ++inside;
        else if (suffix.ends_with('.'))
            ++bare;
    }
    CHECK(bare == 29);     // KF/TIM/M001.-M009., M101.-M115., M999., E0.-E3.
    CHECK(inside == 5);    // the opening's and ending's scroll pages, the banners
    for (const auto &row : kinds)
        CHECK(row.check != nullptr);
}

TEST_CASE("a picture the disc names without .TIM takes the TIM check") {
    const auto kinds = mod_asset_kinds();
    for (const std::string_view path : {"KF/TIM/M001.", "KF/TIM/M999.", "E2."}) {
        const auto row = std::find_if(kinds.begin(), kinds.end(),
                                      [&](const verdite::ModAssetKind &kind) { return path.ends_with(kind.suffix); });
        REQUIRE(row != kinds.end());
        CHECK_FALSE(row->check(path, view(minimal_tim())).has_value());
    }
}

TEST_CASE("a container's other files and pictures have no kind row") {
    for (const std::string_view path : {"KF/B0/MIX3.", "KF/B0/MIXA3.", "KF/B0/MIX3.#0", "KF/COM/MIX.TIM#4", "KF/COM/STAT.DAT"})
        for (const auto &row : mod_asset_kinds())
            if (row.suffix != ".TIM")
                CHECK_FALSE(path.ends_with(row.suffix));
}

TEST_CASE("a .MIM disc path has no kind row, so a MIM replacement is not taken") {
    for (const std::string_view path : {"KF/WEPON/WEP00.MIM", "KF/CHR1.MIM", "KF/CHR3.MIM"})
        for (const auto &row : mod_asset_kinds())
            CHECK_FALSE(path.ends_with(row.suffix));
}

TEST_CASE("a minimal TIM is accepted") {
    CHECK_FALSE(kind(0).check(kind(0).suffix, view(minimal_tim())).has_value());
}

TEST_CASE("a TIM that is not one is refused with a reason, whatever its bytes") {
    const Bytes garbage(64, 0xff);
    const auto reason = kind(0).check(kind(0).suffix, view(garbage));
    REQUIRE(reason.has_value());
    CHECK(reason.value_or("").find("TIM") != std::string::npos);
    CHECK(kind(0).check(kind(0).suffix, std::span<const std::uint8_t>{}).has_value());
}

TEST_CASE("every truncation of a minimal TIM is refused, without a crash") {
    const Bytes tim = minimal_tim();
    for (std::size_t size = 0; size < tim.size(); ++size) {
        const Bytes cut(tim.begin(), tim.begin() + static_cast<std::ptrdiff_t>(size));
        CHECK(kind(0).check(kind(0).suffix, view(cut)).has_value());
    }
}

TEST_CASE("a minimal SEQ is accepted") {
    CHECK_FALSE(kind(1).check(kind(1).suffix, view(minimal_seq())).has_value());
}

TEST_CASE("a SEQ that is not one is refused with a reason, and every truncation of one too") {
    const Bytes garbage(40, 0x5a);
    const auto reason = kind(1).check(kind(1).suffix, view(garbage));
    REQUIRE(reason.has_value());
    CHECK(reason.value_or("").find("SEQ") != std::string::npos);
    const Bytes seq = minimal_seq();
    for (std::size_t size = 0; size < seq.size(); ++size) {
        const Bytes cut(seq.begin(), seq.begin() + static_cast<std::ptrdiff_t>(size));
        CHECK(kind(1).check(kind(1).suffix, view(cut)).has_value());
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
    CHECK_FALSE(kind(2).check(kind(2).suffix, view(minimal_tmd())).has_value());
    Bytes none = minimal_tmd();
    none[8] = 0;
    CHECK(kind(2).check(kind(2).suffix, view(none)).has_value());
}

TEST_CASE("every truncation of a minimal TMD is refused, without a crash") {
    const Bytes tmd = minimal_tmd();
    for (std::size_t size = 0; size < tmd.size(); ++size)
        CHECK(kind(2).check(kind(2).suffix, view(prefix_of(tmd, size))).has_value());
}

TEST_CASE("a TMD face that names a vertex or normal its object lacks is refused") {
    Bytes vertex = minimal_tmd();
    vertex[86] = 3;   // the third corner's vertex index: 2 becomes 3, past the object's three
    CHECK(kind(2).check(kind(2).suffix, view(vertex)).has_value());
    Bytes normal = minimal_tmd();
    normal[80] = 1;   // the face's normal: the object has one, index 0
    CHECK(kind(2).check(kind(2).suffix, view(normal)).has_value());
}

TEST_CASE("a TMD primitive whose body runs past the file, or whose vertices are unaligned, is refused") {
    Bytes body = minimal_tmd();
    body[73] = 4;     // the packet's length in words: 16 bytes of body, 12 in the file
    CHECK(kind(2).check(kind(2).suffix, view(body)).has_value());
    Bytes unaligned = minimal_tmd();
    unaligned[12] = 27;   // the vertex array's offset: odd
    CHECK(kind(2).check(kind(2).suffix, view(unaligned)).has_value());
}

TEST_CASE("garbage is refused as a TMD with a reason, and so is random damage") {
    const Bytes garbage(64, 0xff);
    const auto reason = kind(2).check(kind(2).suffix, view(garbage));
    REQUIRE(reason.has_value());
    CHECK(reason.value_or("").find("is not") != std::string::npos);
    std::mt19937 random(7);
    const Bytes tmd = minimal_tmd();
    for (int trial = 0; trial < 4000; ++trial) {
        Bytes damaged = tmd;
        for (int flips = 0; flips < 1 + static_cast<int>(random() % 3); ++flips)
            damaged[random() % damaged.size()] = static_cast<std::uint8_t>(random());
        (void)kind(2).check(kind(2).suffix, view(damaged));
    }
}

namespace {
// A 4-bit TIM of 4x2 words (16x2 texels) with a 16-colour palette, its palette at (0, palette_y) and
// its picture at (picture_x, 256); every texel byte is `ink`.
Bytes small_picture(u16 picture_x, u16 palette_y, std::uint8_t ink) {
    Bytes tim = {0x10, 0, 0, 0,  8, 0, 0, 0,  44, 0, 0, 0,  0, 0,
                 static_cast<std::uint8_t>(palette_y & 0xff), static_cast<std::uint8_t>(palette_y >> 8),  16, 0,  1, 0};
    for (int i = 0; i < 16; ++i) {
        tim.push_back(static_cast<std::uint8_t>(i));
        tim.push_back(0);
    }
    const Bytes image = {28, 0, 0, 0,  static_cast<std::uint8_t>(picture_x & 0xff), static_cast<std::uint8_t>(picture_x >> 8),
                         0, 1,  4, 0,  2, 0};
    tim.insert(tim.end(), image.begin(), image.end());
    tim.insert(tim.end(), 16, ink);
    return tim;
}

// A file of nine such pictures, as B0/MIX3. holds nine.
Bytes picture_file() {
    Bytes file;
    for (u16 i = 0; i < 9; ++i) {
        const Bytes picture = small_picture(static_cast<u16>(320 + 64 * i), static_cast<u16>(480 + i), static_cast<std::uint8_t>(i));
        file.insert(file.end(), picture.begin(), picture.end());
    }
    return file;
}

constexpr std::size_t small_picture_bytes = 8 + 44 + 28;

Bytes picture_at(const Bytes &file, std::size_t index) {
    const auto from = file.begin() + static_cast<std::ptrdiff_t>(index * small_picture_bytes);
    return Bytes(from, from + static_cast<std::ptrdiff_t>(small_picture_bytes));
}

PictureLookup only(std::string_view path, Bytes bytes) {
    auto shared = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
    return [path = std::string(path), shared](std::string_view asked) {
        return asked == path ? shared : nullptr;
    };
}
} // namespace

TEST_CASE("the pictures a mod may replace inside a file are named by file and number") {
    const auto opening = mod_pictures_in("KF/B0/MIX3.");
    REQUIRE(opening.size() == 2);
    CHECK(opening[0] == "KF/B0/MIX3.#6");
    CHECK(opening[1] == "KF/B0/MIX3.#7");
    CHECK(mod_pictures_in("KF/B0/MIX9.").size() == 2);
    CHECK(mod_pictures_in("KF/COM/MIX.TIM").size() == 1);
    CHECK(mod_pictures_in("KF/B1/MIX.TIM").empty());
    CHECK(mod_pictures_in("KF/B0/MIXA3.").empty());
}

TEST_CASE("a replaced picture is put in where the disc's was, and the file's other pictures are left as they are") {
    const Bytes disc = picture_file();
    // Drawn for another place in the picture store: it is moved to the disc picture's.
    const Bytes replacement = small_picture(960, 501, 0xab);
    const auto out = mod_pictures_put_in("KF/B0/MIX3.", disc, only("KF/B0/MIX3.#6", replacement));
    REQUIRE(out.has_value());
    const Bytes file = out.value_or(Bytes{});
    REQUIRE(file.size() == disc.size());
    for (std::size_t i = 0; i < 9; ++i) {
        const Bytes got = picture_at(file, i);
        if (i != 6) {
            CHECK(got == picture_at(disc, i));
            continue;
        }
        Bytes expected = replacement;
        const Bytes place = picture_at(disc, 6);
        std::copy_n(place.begin() + 12, 4, expected.begin() + 12);   // the palette's x and y
        std::copy_n(place.begin() + 56, 4, expected.begin() + 56);   // the picture's x and y
        CHECK(got == expected);
    }
}

TEST_CASE("a file with none of its pictures replaced, or only with ones that do not fit, is left to the disc") {
    const Bytes disc = picture_file();
    CHECK_FALSE(mod_pictures_put_in("KF/B0/MIX3.", disc, [](std::string_view) {
        return std::shared_ptr<const std::vector<std::uint8_t>>();
    }).has_value());
    CHECK_FALSE(mod_pictures_put_in("KF/B0/MIX3.", disc, only("KF/B0/MIX3.#0", small_picture(0, 0, 1))).has_value());
    Bytes wider = small_picture(320, 480, 1);
    wider[60] = 5;    // the picture's width in words: 4 becomes 5
    wider[52] = 32;   // and its block holds the texels for it
    wider.insert(wider.end(), 4, 1);
    CHECK_FALSE(mod_pictures_put_in("KF/B0/MIX3.", disc, only("KF/B0/MIX3.#6", wider)).has_value());
    CHECK_FALSE(mod_pictures_put_in("KF/B0/MIX3.", Bytes(10, 0), only("KF/B0/MIX3.#6", small_picture(0, 0, 1))).has_value());
}

TEST_CASE("a palette wider or taller than the disc's is not put in, whatever its count of colours") {
    const Bytes disc = picture_file();
    Bytes two_rows = small_picture(320, 480, 1);
    two_rows[16] = 8;    // the palette: 8x2 in place of 16x1, as many colours
    two_rows[18] = 2;
    CHECK_FALSE(mod_pictures_put_in("KF/B0/MIX3.", disc, only("KF/B0/MIX3.#6", two_rows)).has_value());
    Bytes narrower = small_picture(320, 480, 1);
    narrower[16] = 8;    // 8x1: fewer colours, in the disc's row
    narrower[8] = 28;    // its block's length: 12 + 8 colours
    narrower.erase(narrower.begin() + 20 + 16, narrower.begin() + 20 + 32);
    CHECK(mod_pictures_put_in("KF/B0/MIX3.", disc, only("KF/B0/MIX3.#6", narrower)).has_value());
}

TEST_CASE("one picture for an upload of its own is the replacement in the disc's place, or the disc's") {
    const Bytes disc = picture_file();
    const auto own = mod_picture_for_upload("KF/B0/MIX3.#7", disc, nullptr);
    REQUIRE(own.has_value());
    CHECK(own.value_or(Bytes{}) == picture_at(disc, 7));
    const Bytes replacement = small_picture(0, 0, 0xcd);
    const auto moved = mod_picture_for_upload("KF/B0/MIX3.#7", disc, &replacement);
    REQUIRE(moved.has_value());
    const Bytes picture = moved.value_or(Bytes{});
    CHECK(picture.back() == 0xcd);
    CHECK(std::equal(picture.begin() + 12, picture.begin() + 16, disc.begin() + 7 * small_picture_bytes + 12));
    CHECK_FALSE(mod_picture_for_upload("KF/B0/MIX3.#40", disc, nullptr).has_value());
}

TEST_CASE("a picture inside a file is checked against the disc's picture, and sized by it") {
    const auto root = std::filesystem::temp_directory_path() / "kf_mod_pictures";
    std::filesystem::create_directories(root / "KF" / "B0");
    const Bytes disc = picture_file();
    std::ofstream(root / "KF" / "B0" / "MIX3.", std::ios::binary)
        .write(reinterpret_cast<const char *>(disc.data()), static_cast<std::streamsize>(disc.size()));
    REQUIRE(kf::data_files_set_root(root.string().c_str()));
    // Its file's size, for the host's first test; the check measures it against the picture itself.
    CHECK(mod_disc_file_size("KF/B0/MIX3.#6") == disc.size());
    CHECK_FALSE(mod_disc_file_size("KF/B0/MIX3.#9").has_value());
    const auto kinds = mod_asset_kinds();
    const auto row = std::find_if(kinds.begin(), kinds.end(),
                                  [](const verdite::ModAssetKind &kind) { return kind.suffix == "KF/B0/MIX3.#6"; });
    REQUIRE(row != kinds.end());
    CHECK_FALSE(row->check(row->suffix, view(small_picture(960, 501, 3))).has_value());
    Bytes reshaped = small_picture(960, 501, 3);
    reshaped[60] = 2;   // 2x4 words in place of 4x2: as many bytes, another shape
    reshaped[62] = 4;
    const auto reason = row->check(row->suffix, view(reshaped));
    REQUIRE(reason.has_value());
    CHECK(reason.value_or("").find("size") != std::string::npos);
    Bytes larger = small_picture(960, 501, 3);
    larger.insert(larger.end(), 4, 0);   // a few bytes past the TIM: no longer one picture of its size
    CHECK(row->check(row->suffix, view(larger)).has_value());
    REQUIRE(mod_disc_file_at(root.string().c_str(), "KF/B0/MIX3.").has_value());
    CHECK(mod_disc_file_at(root.string().c_str(), "KF/B0/MIX3.").value_or(Bytes{}) == disc);
    CHECK_FALSE(mod_disc_file_at(nullptr, "KF/B0/MIX3.").has_value());
    const auto other = std::find_if(kinds.begin(), kinds.end(),
                                    [](const verdite::ModAssetKind &kind) { return kind.suffix == "KF/COM/MIX.TIM#5"; });
    REQUIRE(other != kinds.end());
    const auto missing = other->check(other->suffix, view(small_picture(960, 501, 3)));
    REQUIRE(missing.has_value());
    CHECK(missing.value_or("").find("not a picture on the disc") != std::string::npos);
    std::filesystem::remove_all(root);
}
