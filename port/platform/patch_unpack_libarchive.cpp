#include <kf/platform/patch_archive.h>
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
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
} // namespace

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
