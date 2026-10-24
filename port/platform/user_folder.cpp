#include <kf/platform/user_folder.h>

#include <SDL3/SDL.h>

#include <string>
#include <system_error>

namespace kf {
namespace {
std::string note;

std::filesystem::path pref_path() {
    char *pref = SDL_GetPrefPath("Verdite", "Verdite1");
    if (!pref)
        return {};
    std::filesystem::path path(reinterpret_cast<const char8_t *>(pref));
    SDL_free(pref);
    return path;
}

std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

// SDL's pref paths are <base>/<org>/<app>/ on every desktop platform, so the earlier
// builds' folder is two levels up. Copied, not moved: those builds still read it.
void copy_earlier_folder(const std::filesystem::path &folder) {
    std::error_code error;
    if (!std::filesystem::is_empty(folder, error) || error)
        return;
    const auto app = folder.has_filename() ? folder : folder.parent_path();   // SDL ends it with a separator
    const auto earlier = app.parent_path().parent_path() / "KingsField" / "SLPS00017";
    if (!std::filesystem::is_directory(earlier, error))
        return;
    std::filesystem::copy(earlier, folder, std::filesystem::copy_options::recursive, error);
    if (error)
        note = "Cannot copy " + utf8(earlier) + " into " + utf8(folder) + ": " + error.message();
    else
        note = "Copied the settings, saves and mods of King's Field Enhanced from " + utf8(earlier);
}

} // namespace

const std::filesystem::path &user_folder() {
    // The first call is on the main thread at startup, before any other thread runs.
    static const std::filesystem::path folder = [] {
        auto path = pref_path();
        if (!path.empty())
            copy_earlier_folder(path);
        return path;
    }();
    return folder;
}

const std::string &user_folder_note() {
    user_folder();
    return note;
}
}
