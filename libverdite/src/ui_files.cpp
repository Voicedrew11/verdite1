// The files RmlUi reads (ui_files.h)
#include "ui_files.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

namespace verdite::ui {

namespace {

constexpr std::uintmax_t folder_file_max_bytes = 16u << 20;   // a document, a sheet or a font

// Whether path is root or under it (both already made canonical).
bool is_under(const std::filesystem::path &path, const std::filesystem::path &root) {
    auto [root_end, path_at] = std::mismatch(root.begin(), root.end(), path.begin(), path.end());
    return root_end == root.end();
}

} // namespace

std::string_view asset_name(std::string_view path) {
    while (!path.empty() && (path.front() == '/' || path.front() == '.')) {
        path.remove_prefix(1);
    }
    return path;
}

std::optional<std::filesystem::path> folder_file(const std::filesystem::path &root,
                                                 std::string_view inside) {
    std::filesystem::path relative(std::string(inside), std::filesystem::path::generic_format);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name()) {
        return std::nullopt;
    }
    for (const std::filesystem::path &part : relative) {
        if (part == "..") {
            return std::nullopt;
        }
    }
    std::error_code error;
    std::filesystem::path base = std::filesystem::weakly_canonical(root, error);
    if (error) {
        return std::nullopt;
    }
    std::filesystem::path file = std::filesystem::weakly_canonical(base / relative, error);
    if (error || !is_under(file, base) || file == base) {
        return std::nullopt;   // (a link that leads out of the folder)
    }
    return file;
}

Files::Files(std::span<const Asset> assets, Log log) : assets_(assets), log_(std::move(log)) {}

void Files::add_folder(std::string_view prefix, const std::filesystem::path &root) {
    remove_folder(prefix);
    folders_.push_back({std::string(prefix), root});
}

void Files::remove_folder(std::string_view prefix) {
    std::erase_if(folders_, [&](const Folder &folder) { return folder.prefix == prefix; });
}

std::optional<std::vector<std::byte>> Files::folder_bytes(std::string_view path) const {
    for (const Folder &folder : folders_) {
        if (path.size() <= folder.prefix.size() + 1 || !path.starts_with(folder.prefix) ||
            path[folder.prefix.size()] != '/') {
            continue;
        }
        std::optional<std::filesystem::path> file =
            folder_file(folder.root, path.substr(folder.prefix.size() + 1));
        std::error_code error;
        if (!file || !std::filesystem::is_regular_file(*file, error)) {
            return std::nullopt;
        }
        std::uintmax_t size = std::filesystem::file_size(*file, error);
        if (error || size > folder_file_max_bytes) {
            return std::nullopt;
        }
        std::ifstream in(*file, std::ios::binary);
        std::vector<std::byte> bytes(static_cast<size_t>(size));
        if (!in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size))) {
            return std::nullopt;
        }
        return bytes;
    }
    return std::nullopt;
}

std::optional<std::span<const std::byte>> Files::asset(std::string_view path) const {
    std::string_view name = asset_name(path);
    for (const Asset &file : assets_) {
        if (file.path == name) {
            return file.bytes;
        }
    }
    return std::nullopt;
}

std::span<const std::byte> Files::Reading::bytes() const {
    return disk.empty() ? asset : std::span<const std::byte>(disk);
}

Rml::FileHandle Files::Open(const Rml::String &path) {
    Reading opened;
    opened.open = true;
    if (std::optional<std::span<const std::byte>> carried = asset(path)) {
        opened.asset = *carried;
    } else if (std::optional<std::vector<std::byte>> disk = folder_bytes(asset_name(path))) {
        opened.disk = std::move(*disk);
    } else {
        if (log_) {
            log_("no file " + path);
        }
        return 0;
    }
    // (a closed place is used again: documents open and close their sheets)
    for (size_t i = 0; i < open_.size(); i++) {
        if (!open_[i].open) {
            open_[i] = std::move(opened);
            return i + 1;
        }
    }
    open_.push_back(std::move(opened));
    return open_.size();
}

Files::Reading *Files::reading(Rml::FileHandle file) {
    if (file == 0 || file > open_.size() || !open_[file - 1].open) {
        return nullptr;
    }
    return &open_[file - 1];
}

void Files::Close(Rml::FileHandle file) {
    if (Reading *r = reading(file)) {
        *r = Reading{};
    }
}

size_t Files::Read(void *buffer, size_t size, Rml::FileHandle file) {
    Reading *r = reading(file);
    if (r == nullptr) {
        return 0;
    }
    std::span<const std::byte> bytes = r->bytes();
    size_t count = std::min(size, bytes.size() - r->at);
    std::memcpy(buffer, bytes.data() + r->at, count);
    r->at += count;
    return count;
}

bool Files::Seek(Rml::FileHandle file, long offset, int origin) {
    Reading *r = reading(file);
    if (r == nullptr) {
        return false;
    }
    auto size = static_cast<long>(r->bytes().size());
    long base = 0;
    if (origin == SEEK_CUR) {
        base = static_cast<long>(r->at);
    } else if (origin == SEEK_END) {
        base = size;
    }
    long at = base + offset;
    if (at < 0 || at > size) {
        return false;
    }
    r->at = static_cast<size_t>(at);
    return true;
}

size_t Files::Tell(Rml::FileHandle file) {
    Reading *r = reading(file);
    return r != nullptr ? r->at : 0;
}

} // namespace verdite::ui
