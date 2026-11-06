#ifndef KF_PLATFORM_DEATH_RELOAD_H
#define KF_PLATFORM_DEATH_RELOAD_H
// PC extension: a death reloads the last save made or loaded this session, in place of the
// game's New Game (docs/verdite-port.md, "Reload on death"). The Dragon Tree Fruit's revival at
// the fountain comes first and stays the game's. Kept by port/ext/death_reload.cpp.
#include <kf/platform/saves.h>

namespace kf {

// A save was written or read: a death reloads it from now on.
void death_reload_note_save(SaveSlot slot);
// A new game started: there is nothing to reload until a save is made or loaded.
void death_reload_new_game();
// At the end of the death fade, when no fruit revives the player: loads the last save in place
// of the game's restart if the settings allow it. False leaves the restart to the game.
bool death_reload_last_save();

} // namespace kf

#endif // KF_PLATFORM_DEATH_RELOAD_H
