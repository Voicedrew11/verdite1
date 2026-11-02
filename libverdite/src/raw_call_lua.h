#ifndef VERDITE_RAW_CALL_LUA_H
#define VERDITE_RAW_CALL_LUA_H
// The call layer's Lua binding (verdite/raw_call.h), private to libverdite: mods.cpp calls it once
// per mod state, after the game's tables are bound.
#include "verdite/raw_call.h"

struct lua_State;

namespace verdite {

// Binds a function per entry of the layer at layer.path ("kf.call"), creating the tables on the way.
void raw_call_open(lua_State *L, const RawCallLayer &layer);

// Defined by the mod host: whether a call that changes the game may run now, which is directly in
// the mod's on_between_updates, on its main thread.
[[nodiscard]] bool mod_may_change_game(lua_State *L);

} // namespace verdite

#endif // VERDITE_RAW_CALL_LUA_H
