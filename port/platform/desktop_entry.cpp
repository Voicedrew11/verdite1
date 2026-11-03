#include <kf/platform/user_folder.h>

#include <verdite/desktop_entry.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace kf {
#if defined(__linux__) && !defined(__ANDROID__)
namespace {
constexpr const char *app_id = "verdite1";

std::string file_text(const std::filesystem::path &file) {
    std::ifstream stream(file, std::ios::binary);
    std::stringstream text;
    text << stream.rdbuf();
    return text.str();
}

// $XDG_DATA_HOME, or ~/.local/share; empty when neither names a place.
std::filesystem::path data_home() {
    const char *xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] == '/')
        return xdg;
    const char *home = std::getenv("HOME");
    return home && home[0] == '/' ? std::filesystem::path(home) / ".local/share" : std::filesystem::path();
}

bool write_if_changed(const std::filesystem::path &file, const std::string &text) {
    if (file_text(file) == text)
        return false;
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::binary);
    out << text;
    return out.flush().good();
}
} // namespace

void desktop_entry_install() {
    const char *appimage = std::getenv("APPIMAGE");
    const char *appdir = std::getenv("APPDIR");
    const char *off = std::getenv("KF_ICON_INSTALL");
    if (!appimage || !appdir || (off && std::string(off) == "0"))
        return;
    const auto home = data_home();
    if (home.empty())
        return;
    const auto entry = home / "applications" / (std::string(app_id) + ".desktop");
    const std::string old = file_text(entry);
    if (!old.empty() && old.find(verdite::desktop_entry_marker) == std::string::npos)
        return;
    const bool wrote = write_if_changed(
        entry, verdite::desktop_entry_text({product_name, "King's Field",
                                            "A PC port of King's Field (SLPS-00017). Requires your own disc image.",
                                            app_id, appimage}));
    write_if_changed(home / "icons/hicolor/scalable/apps" / (std::string(app_id) + ".svg"),
                     file_text(std::filesystem::path(appdir) / (std::string(app_id) + ".svg")));
    if (wrote)
        std::printf("[desktop] %s is in the menu, running %s\n", product_name, appimage);
}
#else
void desktop_entry_install() {}
#endif
} // namespace kf
