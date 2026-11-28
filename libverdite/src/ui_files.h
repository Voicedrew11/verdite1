#ifndef VERDITE_UI_FILES_H
#define VERDITE_UI_FILES_H
// The files RmlUi reads: the game's carried assets, and folders on disk under a
// prefix (a mod's documents), never read from outside their folder. Game thread.
#include <RmlUi/Core/FileInterface.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <verdite/ui.h>

namespace verdite::ui {

// The file a path under a folder names, or nothing when it would be outside
// root ("..", an absolute path, a link out). For tests too.
[[nodiscard]] std::optional<std::filesystem::path>
folder_file(const std::filesystem::path &root, std::string_view inside);

// "/menu.rml", "./menu.rml" and "menu.rml" as one name.
[[nodiscard]] std::string_view asset_name(std::string_view path);

class Files final : public Rml::FileInterface {
public:
    using Log = std::function<void(std::string_view)>;
    Files(std::span<const Asset> assets, Log log);

    void add_folder(std::string_view prefix, const std::filesystem::path &root);
    void remove_folder(std::string_view prefix);
    // The whole of a file, from the assets or a folder; nothing if missing.
    [[nodiscard]] std::optional<std::span<const std::byte>> asset(std::string_view path) const;

    Rml::FileHandle Open(const Rml::String &path) override;
    void Close(Rml::FileHandle file) override;
    size_t Read(void *buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;
    size_t Tell(Rml::FileHandle file) override;

private:
    struct Folder {
        std::string prefix;
        std::filesystem::path root;
    };
    struct Reading {
        bool open = false;
        std::span<const std::byte> asset;   // a carried file
        std::vector<std::byte> disk;        // or a file read from a folder
        size_t at = 0;
        [[nodiscard]] std::span<const std::byte> bytes() const;
    };
    [[nodiscard]] std::optional<std::vector<std::byte>> folder_bytes(std::string_view path) const;
    Reading *reading(Rml::FileHandle file);

    std::span<const Asset> assets_;
    Log log_;
    std::vector<Folder> folders_;
    std::vector<Reading> open_;   // a handle is its place plus one
};

} // namespace verdite::ui

#endif // VERDITE_UI_FILES_H
