#ifndef VERDITE_RAW_LUA_H
#define VERDITE_RAW_LUA_H
// The raw layer's Lua binding (verdite/raw.h), private to libverdite: mods.cpp calls it once per
// mod state, after the game's tables are bound.
#include "verdite/raw.h"

struct lua_State;

namespace verdite {

// Binds the layer at layer.path ("kf.raw") in L, creating the tables on the way. Raises a Lua
// error if a global's name clashes with the layer's own functions.
void raw_open(lua_State *L, const RawLayer &layer);

} // namespace verdite

#endif // VERDITE_RAW_LUA_H
