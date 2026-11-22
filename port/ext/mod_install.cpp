#include "mod_install.h"

#include "internal.hpp"

#include <kf/platform/archive_entries.h>
#include <verdite/mod_archive.h>

#include <SDL3/SDL_iostream.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <system_error>
#include <vector>

namespace kf::ext {
namespace {
namespace fs = std::filesystem;

struct IoClose {
    void operator()(SDL_IOStream *stream) const { SDL_CloseIO(stream); }
};

// The picked file's bytes; nothing when it cannot be read or is larger than any mod.
std::optional<std::vector<u8>> read_picked(const std::string &picked) {
    const std::unique_ptr<SDL_IOStream, IoClose> in(SDL_IOFromFile(picked.c_str(), "rb"));
    if (!in)
        return std::nullopt;
    const Sint64 size = SDL_GetIOSize(in.get());
    if (size < 0 || static_cast<std::uint64_t>(size) > verdite::mod_archive_bytes_most)
        return std::nullopt;
    std::vector<u8> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && SDL_ReadIO(in.get(), bytes.data(), bytes.size()) != bytes.size())
        return std::nullopt;
    return bytes;
}

bool write_files(const fs::path &folder, const std::vector<verdite::ModInstallFile> &files) {
    std::error_code error;
    for (const verdite::ModInstallFile &file : files) {
        const fs::path path = folder / fs::path(file.path);
        fs::create_directories(path.parent_path(), error);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(file.bytes.data()), static_cast<std::streamsize>(file.bytes.size()));
        if (error || !out.flush())
            return false;
    }
    return true;
}

// Puts the written folder in place of the mod's, the old one moved to `kept` first.
bool swap_in(const fs::path &written, const fs::path &target, const fs::path &kept) {
    std::error_code error;
    const bool had_one = fs::exists(target, error);
    if (had_one) {
        fs::remove_all(kept, error);
        fs::create_directories(kept.parent_path(), error);
        fs::rename(target, kept, error);
        if (error)
            return false;
    }
    fs::rename(written, target, error);
    if (error && had_one) {
        std::error_code ignored;
        fs::rename(kept, target, ignored);
    }
    return !error;
}

std::string path_text(const fs::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
} // namespace

std::string mod_install_from_archive(const std::string &picked) {
    const auto bytes = read_picked(picked);
    if (!bytes)
        return "Cannot read that file, or it is too large to be a mod.";
    ArchiveRead read = archive_entries(*bytes);
    if (!read.error.empty())
        return read.error;
    const verdite::ModInstallPlan plan = verdite::mod_install_plan(std::move(read.entries), picked);
    if (!plan.refusal.empty())
        return plan.refusal;

    // Written outside the mods folder, so a scan never sees half a mod, even after a crash.
    const fs::path staging = mods_dir().parent_path() / "mods_installing";
    const fs::path written = staging / plan.folder;
    std::error_code error;
    fs::remove_all(staging, error);
    fs::create_directories(mods_dir(), error);
    if (!write_files(written, plan.files)) {
        fs::remove_all(staging, error);
        return "Cannot write the mod into " + path_text(staging) + ".";
    }
    const fs::path target = mods_dir() / plan.folder;
    const fs::path kept = mods_dir().parent_path() / "mods_removed" / plan.folder;
    const bool replacing = fs::exists(target, error);
    const bool placed = swap_in(written, target, kept);
    fs::remove_all(staging, error);
    if (!placed)
        return "Cannot put the mod into " + path_text(target) + ".";
    std::printf("[mods] installed %s into %s from %s\n", plan.name.c_str(), path_text(target).c_str(), picked.c_str());
    std::string message = "Installed " + plan.name + ". Turn it on under MODS on the title screen.";
    if (replacing)
        message += " The copy it replaced is in mods_removed.";
    return message;
}

} // namespace kf::ext
