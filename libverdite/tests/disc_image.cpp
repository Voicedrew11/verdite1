#include <verdite/disc_image.h>

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace verdite;
namespace fs = std::filesystem;

// The made-up disc and its CHDs, from tools/make_disc_fixtures.py.
namespace {
constexpr std::uint64_t no_limit = std::uint64_t{128} * 1024 * 1024;
constexpr std::size_t raw_sector_bytes = 2352;
constexpr std::size_t frames_per_hunk = 8;

fs::path fixture(const char *name) {
    return fs::path(VERDITE_TEST_DATA) / "disc" / name;
}

std::vector<std::uint8_t> read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

fs::path scratch_file(const char *name) {
    return fs::temp_directory_path() / ("verdite-disc-" + std::to_string(::getpid()) + "-" + name);
}

// The whole image, or nothing when a read fails.
std::vector<std::uint8_t> read_all(DiscImage &image) {
    std::vector<std::uint8_t> bytes(image.size_bytes());
    if (!image.read(0, bytes))
        bytes.clear();
    return bytes;
}
} // namespace

TEST_CASE("a CHD of a raw track reads back as the .bin it was made from, with every CD codec") {
    const auto bin = read_file(fixture("made_up.bin"));
    REQUIRE(bin.size() == 33 * raw_sector_bytes);
    for (const char *name : {"made_up_cdlz.chd", "made_up_cdzl.chd", "made_up_cdfl.chd", "made_up_cdzs.chd"}) {
        CAPTURE(name);
        auto opened = open_disc_image(fixture(name), no_limit);
        REQUIRE_MESSAGE(opened.image, opened.error);
        CHECK(opened.image->size_bytes() == bin.size());
        CHECK(read_all(*opened.image) == bin);
    }
}

TEST_CASE("a CHD read in pieces gives the same bytes, across hunks and backwards") {
    const auto bin = read_file(fixture("made_up.bin"));
    auto opened = open_disc_image(fixture("made_up_cdlz.chd"), no_limit);
    REQUIRE(opened.image);
    // Across the first hunk boundary, and a run spanning a sector's end and the next's start.
    const std::size_t boundary = frames_per_hunk * raw_sector_bytes;
    for (const auto [offset, length] : {std::pair<std::size_t, std::size_t>{boundary - 10, 30},
                                        {raw_sector_bytes - 1, 2}, {bin.size() - 7, 7}, {0, 1}}) {
        CAPTURE(offset);
        std::vector<std::uint8_t> piece(length);
        REQUIRE(opened.image->read(offset, piece));
        CHECK(std::equal(piece.begin(), piece.end(), bin.begin() + static_cast<std::ptrdiff_t>(offset)));
    }
    // The last hunk then the first: the cached hunk is not reused for another.
    std::vector<std::uint8_t> sector(raw_sector_bytes);
    for (std::size_t at = bin.size(); at > 0;) {
        at -= raw_sector_bytes;
        REQUIRE(opened.image->read(at, sector));
        CHECK(std::equal(sector.begin(), sector.end(), bin.begin() + static_cast<std::ptrdiff_t>(at)));
    }
}

TEST_CASE("a CHD of a 2048-byte track reads back as the .iso") {
    const auto iso = read_file(fixture("made_up.iso"));
    auto opened = open_disc_image(fixture("made_up_iso.chd"), no_limit);
    REQUIRE_MESSAGE(opened.image, opened.error);
    CHECK(opened.image->size_bytes() == iso.size());
    CHECK(read_all(*opened.image) == iso);
}

TEST_CASE("a plain image reads as itself, whatever its name") {
    const auto bin = read_file(fixture("made_up.bin"));
    const auto renamed = scratch_file("plain.chd");
    write_file(renamed, bin);
    auto opened = open_disc_image(renamed, no_limit);
    REQUIRE(opened.image);
    CHECK(read_all(*opened.image) == bin);
    fs::remove(renamed);
}

TEST_CASE("a read past the end fails") {
    for (const char *name : {"made_up.bin", "made_up_cdlz.chd"}) {
        CAPTURE(name);
        auto opened = open_disc_image(fixture(name), no_limit);
        REQUIRE(opened.image);
        const auto size = opened.image->size_bytes();
        std::vector<std::uint8_t> two(2);
        CHECK_FALSE(opened.image->read(size - 1, two));
        CHECK_FALSE(opened.image->read(UINT64_MAX, two));
        CHECK(opened.image->read(size - 2, two));
    }
}

TEST_CASE("a CHD made as the differences from a parent is refused") {
    const auto opened = open_disc_image(fixture("made_up_child.chd"), no_limit);
    CHECK_FALSE(opened.image);
    CHECK(opened.error.find("differences") != std::string::npos);
}

TEST_CASE("a CHD whose first track is audio is refused") {
    const auto opened = open_disc_image(fixture("audio_first.chd"), no_limit);
    CHECK_FALSE(opened.image);
    CHECK(opened.error.find("audio") != std::string::npos);
}

TEST_CASE("an image larger than the limit is refused unread") {
    const auto bin_size = read_file(fixture("made_up.bin")).size();
    for (const char *name : {"made_up.bin", "made_up_cdlz.chd"}) {
        CAPTURE(name);
        CHECK_FALSE(open_disc_image(fixture(name), bin_size - 1).image);
        CHECK(open_disc_image(fixture(name), bin_size).image);
    }
}

TEST_CASE("a missing or empty file is refused") {
    CHECK_FALSE(open_disc_image(fixture("no_such_image.bin"), no_limit).image);
    const auto empty = scratch_file("empty.bin");
    write_file(empty, {});
    const auto opened = open_disc_image(empty, no_limit);
    CHECK_FALSE(opened.image);
    CHECK(opened.error.find("empty") != std::string::npos);
    fs::remove(empty);
}

TEST_CASE("a damaged CHD fails to open or to read, and never reads back wrong bytes") {
    const auto bin = read_file(fixture("made_up.bin"));
    const auto chd = read_file(fixture("made_up_cdlz.chd"));
    const auto damaged = scratch_file("damaged.chd");
    // Cut short at every point, then each byte in turn flipped (every 61st, to keep it quick). The
    // flips start past the 8-byte magic: without it the file is a plain image, read as itself.
    for (std::size_t length = 0; length < chd.size(); length += 509) {
        CAPTURE(length);
        write_file(damaged, std::vector<std::uint8_t>(chd.begin(), chd.begin() + static_cast<std::ptrdiff_t>(length)));
        auto opened = open_disc_image(damaged, no_limit);
        if (opened.image) {
            const auto bytes = read_all(*opened.image);
            CHECK((bytes.empty() || bytes == bin));
        }
    }
    for (std::size_t at = 8; at < chd.size(); at += 61) {
        CAPTURE(at);
        auto copy = chd;
        copy[at] ^= 0x5A;
        write_file(damaged, copy);
        auto opened = open_disc_image(damaged, no_limit);
        if (opened.image) {
            const auto bytes = read_all(*opened.image);
            CHECK((bytes.empty() || bytes == bin));
        }
    }
    fs::remove(damaged);
}
