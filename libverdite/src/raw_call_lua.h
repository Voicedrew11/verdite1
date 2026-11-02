#ifndef VERDITE_RAW_CALL_LUA_H
#define VERDITE_RAW_CALL_LUA_H
// The call layer's Lua binding (verdite/raw_call.h), private to libverdite: mods.cpp calls it once
// per mod state, after the game's tables are bound.
#include "verdite/raw_call.h"

#include <cstdint>

struct lua_State;

namespace verdite {

// Binds a function per entry of the layer at layer.path ("kf.call"), creating the tables on the way.
void raw_call_open(lua_State *L, const RawCallLayer &layer);

// Defined by the mod host: whether a call that changes the game may run now, which is directly in
// the mod's on_between_updates, on its main thread.
[[nodiscard]] bool mod_may_change_game(lua_State *L);

// Defined by the mod host: while one lives with runs_frames set, the game may present frames with
// the mod's state in use below them. Nothing here raises a Lua error.
class ModFrameCall {
public:
    ModFrameCall(lua_State *L, bool runs_frames);
    ~ModFrameCall();
    ModFrameCall(const ModFrameCall &) = delete;
    ModFrameCall &operator=(const ModFrameCall &) = delete;

private:
    lua_State *L_;
    bool runs_frames_;
    std::int64_t budget_left_ = 0;
};

} // namespace verdite

#endif // VERDITE_RAW_CALL_LUA_H
