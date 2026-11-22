#pragma once
// A mod installed from a .zip the player picks (Settings > Mods > Install mod...): the way in on a
// phone, whose mods folder no file manager reaches. The mod host finds the new folder on its next scan.
#include <string>

namespace kf::ext {

// Reads the archive at `picked` (a path, or Android's content:// URI), checks it
// (verdite::mod_install_plan) and writes the mod's folder into the mods folder. An earlier copy of
// that folder moves to mods_removed beside it rather than being deleted. What happened, for the player.
std::string mod_install_from_archive(const std::string &picked);

} // namespace kf::ext
