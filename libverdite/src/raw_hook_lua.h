#ifndef VERDITE_RAW_HOOK_LUA_H
#define VERDITE_RAW_HOOK_LUA_H
// The hook layer's Lua binding (verdite/raw_hook.h), private to libverdite: mods.cpp calls it once per
// mod state, after the game's tables are bound.
#include "verdite/raw_hook.h"

#include <cstddef>
#include <cstdint>

struct lua_State;

namespace verdite {

enum class RawHookKind : std::uint8_t { Before, After, Replace };
inline constexpr std::size_t raw_hook_kinds = 3;

// Binds kf.hook.before, .after and .replace, each with a function per point its reach allows.
void raw_hook_open(lua_State *L, const RawHookLayer &layer);

// Defined by the mod host: the handler at Lua index 1, or nil to remove the mod's own, for this point.
void mod_set_hook(lua_State *L, std::size_t point, RawHookKind kind);

} // namespace verdite

#endif // VERDITE_RAW_HOOK_LUA_H
