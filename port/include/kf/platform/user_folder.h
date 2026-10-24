#ifndef KF_PLATFORM_USER_FOLDER_H
#define KF_PLATFORM_USER_FOLDER_H

#include <filesystem>
#include <string>

namespace kf {
// The name players see: window title, dialogs, the release's files.
inline constexpr const char *product_name = "Verdite1";

// The player's folder (SDL's pref path for Verdite/Verdite1): settings, saves, mods, the log,
// crash reports, and the game files when the game's own folder cannot be written. Created if
// missing. The first time it is empty, the folder of the builds named King's Field Enhanced
// (KingsField/SLPS00017 beside it) is copied in, and left as it was. Empty when the platform
// has no such folder.
const std::filesystem::path &user_folder();

// What the first user_folder() call copied, or why it could not, for the log once it is open
// (the copy runs before kf.log exists). Empty when nothing was copied.
const std::string &user_folder_note();
}

#endif // KF_PLATFORM_USER_FOLDER_H
