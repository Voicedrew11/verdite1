#include <kf/platform/archive_entries.h>
#include <kf/platform/patch_archive.h>
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <optional>
#include <string_view>

namespace kf {
namespace {
struct ArchiveClose {
    void operator()(archive *reader) const {
        archive_read_free(reader);
    }
};

bool names_a_patch(std::string_view path) {
    constexpr std::string_view extension = ".ppf";
    if (path.size() < extension.size())
        return false;
    return std::equal(
        extension.begin(), extension.end(), path.end() - extension.size(),
        [](char wanted, char c) { return wanted == std::tolower(static_cast<unsigned char>(c)); });
}

// The current entry's bytes, decoded in memory so nothing in the archive can name a place on
// disk. Empty when it cannot be read or is over the limit, whatever its header claims.
std::vector<u8> read_entry(archive *reader) {
    std::vector<u8> bytes;
    std::array<u8, std::size_t{64} * 1024> chunk;
    for (;;) {
        const la_ssize_t got = archive_read_data(reader, chunk.data(), chunk.size());
        if (got == 0)
            return bytes;
        if (got < 0 || bytes.size() + static_cast<std::size_t>(got) > patch_file_limit)
            return {};
        bytes.insert(bytes.end(), chunk.data(), chunk.data() + got);
    }
}

// The current entry's bytes, up to room_bytes; nothing past it or on a read failure.
std::optional<std::vector<u8>> read_mod_entry(archive *reader, std::size_t room_bytes) {
    std::vector<u8> bytes;
    std::array<u8, std::size_t{64} * 1024> chunk;
    for (;;) {
        const la_ssize_t got = archive_read_data(reader, chunk.data(), chunk.size());
        if (got == 0)
            return bytes;
        if (got < 0 || static_cast<std::size_t>(got) > room_bytes - bytes.size())
            return std::nullopt;
        bytes.insert(bytes.end(), chunk.data(), chunk.data() + got);
    }
}

ArchiveRead refused(const char *error) {
    return {{}, error};
}
} // namespace

ArchiveRead archive_entries(std::span<const u8> archive_bytes) {
    const std::unique_ptr<archive, ArchiveClose> reader(archive_read_new());
    if (!reader || archive_read_support_format_zip(reader.get()) != ARCHIVE_OK ||
        archive_read_open_memory(reader.get(), archive_bytes.data(), archive_bytes.size()) != ARCHIVE_OK)
        return refused("Cannot open that file as a .zip archive.");
    ArchiveRead read;
    std::size_t room_bytes = verdite::mod_archive_bytes_most;
    for (;;) {
        archive_entry *entry = nullptr;
        const int status = archive_read_next_header(reader.get(), &entry);
        if (status == ARCHIVE_EOF)
            return read;
        const char *path = status == ARCHIVE_OK || status == ARCHIVE_WARN ? archive_entry_pathname(entry) : nullptr;
        if (!path)
            return refused("That archive is damaged.");
        if (archive_entry_filetype(entry) != AE_IFREG)
            continue;
        if (read.entries.size() >= verdite::mod_archive_files_most)
            return refused("That archive holds too many files to be a mod.");
        auto bytes = read_mod_entry(reader.get(), room_bytes);
        if (!bytes)
            return refused("That archive is damaged, or too large to be a mod.");
        room_bytes -= bytes->size();
        read.entries.push_back({path, std::move(*bytes)});
    }
}

std::optional<std::vector<u8>> patch_from_archive(std::span<const u8> archive_bytes,
                                                  std::string *error) {
    const std::unique_ptr<archive, ArchiveClose> reader(archive_read_new());
    if (!reader || archive_read_support_format_rar(reader.get()) != ARCHIVE_OK ||
        archive_read_support_format_zip(reader.get()) != ARCHIVE_OK ||
        archive_read_open_memory(reader.get(), archive_bytes.data(), archive_bytes.size()) !=
            ARCHIVE_OK) {
        *error =
            "Cannot open that archive. Extract the .ppf patch from it and choose that instead.";
        return std::nullopt;
    }
    std::vector<u8> best;
    for (;;) {
        archive_entry *entry = nullptr;
        const int status = archive_read_next_header(reader.get(), &entry);
        if (status == ARCHIVE_EOF)
            break;
        if (status != ARCHIVE_OK && status != ARCHIVE_WARN) {
            if (!best.empty())
                break; // damage after the patch does not matter: its result is checked in full
            *error =
                "Cannot open that archive. Extract the .ppf patch from it and choose that instead.";
            return std::nullopt;
        }
        const char *path = archive_entry_pathname(entry);
        if (!path || archive_entry_filetype(entry) != AE_IFREG || !names_a_patch(path))
            continue;
        auto bytes = read_entry(reader.get());
        if (bytes.size() > best.size())
            best = std::move(bytes);
    }
    if (best.empty()) {
        *error = "That archive holds no .ppf patch.";
        return std::nullopt;
    }
    return best;
}
} // namespace kf
