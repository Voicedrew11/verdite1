#include <kf/platform/patch_archive.h>

#include <doctest/doctest.h>

#include <string_view>

using kf::patch_from_archive;

namespace {
using Bytes = std::vector<u8>;

struct Entry {
    std::string_view path;
    std::string_view contents;
};

std::uint32_t crc32_of(const u8 *bytes, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFF;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
    }
    return ~crc;
}

std::uint32_t crc32_of(std::string_view text) {
    return crc32_of(reinterpret_cast<const u8 *>(text.data()), text.size());
}

void put(Bytes &out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i)
        out.push_back(static_cast<u8>(value >> (8 * i)));
}

void put(Bytes &out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}

// A RAR 2.9 block: its header's CRC is the low half of the CRC of all that follows it.
void put_rar_block(Bytes &out, const Bytes &header) {
    put(out, crc32_of(header.data(), header.size()) & 0xFFFF, 2);
    out.insert(out.end(), header.begin(), header.end());
}

// Stored rather than compressed: making compressed RAR takes the proprietary tool. That the
// real decoder opens the translator's -m5 archive is checked by hand (docs/ANDROID_PLAN.md).
Bytes rar_of(std::initializer_list<Entry> entries) {
    constexpr int block_prefix_bytes = 7; // CRC, type, flags, size
    Bytes out{0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00};
    Bytes main{0x73};
    put(main, 0, 2);
    put(main, block_prefix_bytes + 6, 2);
    put(main, 0, 4 + 2);
    put_rar_block(out, main);
    for (const Entry &entry : entries) {
        constexpr int file_fields_bytes = 25;
        Bytes file{0x74};
        put(file, 0x8000, 2); // data follows, its size in the header
        put(file,
            static_cast<std::uint32_t>(block_prefix_bytes + file_fields_bytes + entry.path.size()),
            2);
        put(file, static_cast<std::uint32_t>(entry.contents.size()), 4);
        put(file, static_cast<std::uint32_t>(entry.contents.size()), 4);
        file.push_back(2); // Windows
        put(file, crc32_of(entry.contents), 4);
        put(file, 0x21000000, 4); // a date
        file.push_back(29);       // the version to unpack
        file.push_back(0x30);     // stored
        put(file, static_cast<std::uint32_t>(entry.path.size()), 2);
        put(file, 0x20, 4); // an ordinary file
        put(file, entry.path);
        put_rar_block(out, file);
        put(out, entry.contents);
    }
    Bytes end{0x7B};
    put(end, 0x4000, 2);
    put(end, block_prefix_bytes, 2);
    put_rar_block(out, end);
    return out;
}

Bytes zip_of(std::initializer_list<Entry> entries) {
    Bytes out, directory;
    for (const Entry &entry : entries) {
        const auto at = static_cast<std::uint32_t>(out.size());
        auto common = [&](Bytes &to) {
            put(to, 20, 2);    // the version to unpack
            put(to, 0, 2 + 2); // flags; stored
            put(to, 0x21000000, 4);
            put(to, crc32_of(entry.contents), 4);
            put(to, static_cast<std::uint32_t>(entry.contents.size()), 4);
            put(to, static_cast<std::uint32_t>(entry.contents.size()), 4);
            put(to, static_cast<std::uint32_t>(entry.path.size()), 2);
            put(to, 0, 2); // no extra field
        };
        put(out, "PK\x03\x04");
        common(out);
        put(out, entry.path);
        put(out, entry.contents);
        put(directory, "PK\x01\x02");
        put(directory, 20, 2);
        common(directory);
        put(directory, 0, 2 + 2 + 2); // no comment; disk; internal attributes
        put(directory, 0, 4);         // external attributes
        put(directory, at, 4);
        put(directory, entry.path);
    }
    const auto directory_at = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), directory.begin(), directory.end());
    put(out, "PK\x05\x06");
    put(out, 0, 2 + 2);
    put(out, static_cast<std::uint32_t>(entries.size()), 2);
    put(out, static_cast<std::uint32_t>(entries.size()), 2);
    put(out, static_cast<std::uint32_t>(directory.size()), 4);
    put(out, directory_at, 4);
    put(out, 0, 2);
    return out;
}

std::string text_of(const std::optional<Bytes> &bytes) {
    return bytes ? std::string(bytes->begin(), bytes->end()) : std::string("(none)");
}

constexpr std::initializer_list<Entry> the_release = {
    {"Translated Manual/Cover.jpg", "a much longer picture of the manual's cover"},
    {"small.PPF", "PPF small"},
    {"KF Jap to Eng v1.0.ppf", "PPF30 the patch"},
    {"Read Me.rtf", "words"},
};
} // namespace

TEST_CASE("the largest .ppf comes out of a .rar, whatever else is in it") {
    std::string error;
    CHECK(text_of(patch_from_archive(rar_of(the_release), &error)) == "PPF30 the patch");
}

TEST_CASE("the largest .ppf comes out of a .zip") {
    std::string error;
    CHECK(text_of(patch_from_archive(zip_of(the_release), &error)) == "PPF30 the patch");
}

TEST_CASE("an archive with no .ppf says so") {
    std::string error;
    CHECK_FALSE(patch_from_archive(rar_of({{"Read Me.rtf", "words"}}), &error));
    CHECK(error == "That archive holds no .ppf patch.");
}

TEST_CASE("what is not an archive is refused") {
    std::string error;
    const Bytes garbage(4096, 0x5A);
    CHECK_FALSE(patch_from_archive(garbage, &error));
    CHECK_FALSE(error.empty());
    error.clear();
    CHECK_FALSE(patch_from_archive(std::span<const u8>{}, &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("an archive cut short anywhere is refused or gives the whole patch, never a part") {
    const Bytes whole = rar_of({{"patch.ppf", "PPF30 the patch"}, {"after.txt", "words"}});
    for (std::size_t size = 0; size < whole.size(); ++size) {
        std::string error;
        const auto got = patch_from_archive(std::span(whole).first(size), &error);
        if (got)
            CHECK(text_of(got) == "PPF30 the patch");
        else
            CHECK_FALSE(error.empty());
    }
}
