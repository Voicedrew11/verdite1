#ifndef VERDITE_DESKTOP_ENTRY_H
#define VERDITE_DESKTOP_ENTRY_H
// A freedesktop .desktop file for a program that runs from one movable file, an AppImage: what puts
// the game in a desktop's menu, and what a Wayland compositor reads to find its icon.
#include <string>
#include <string_view>

namespace verdite {

struct DesktopEntryText {
    std::string_view name;           // Verdite1
    std::string_view generic_name;   // the game's: King's Field
    std::string_view comment;
    std::string_view icon;           // an icon theme's name: verdite1
    std::string_view executable;     // the absolute path to run, spelled as given
};

// The file's text. It carries desktop_entry_marker, so the program can tell its own entry from one a
// packager wrote and rewrite only its own when the file has moved.
std::string desktop_entry_text(const DesktopEntryText &entry);
inline constexpr std::string_view desktop_entry_marker = "X-Verdite-Written=true";

} // namespace verdite

#endif // VERDITE_DESKTOP_ENTRY_H
