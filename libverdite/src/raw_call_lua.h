#ifndef VERDITE_RAW_CALL_LUA_H
#define VERDITE_RAW_CALL_LUA_H
// The call layer's Lua binding (verdite/raw_call.h), private to libverdite: mods.cpp calls it once
// per mod state, after the game's tables are bound. The hook layer shares its checks and crossings.
#include "verdite/raw_call.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace verdite {

// Binds a function per entry of the layer at layer.path ("kf.call"), creating the tables on the way.
void raw_call_open(lua_State *L, const RawCallLayer &layer);

// Values checked against parameters: converted, or why they will not do. A value equal to trusted's at
// its place passes unchecked, so the game's own values go back through a mod. trusted may be empty.
struct RawCheckedValues {
    std::string error;
    std::vector<RawValue> converted;
    bool all_trusted = true;
};
[[nodiscard]] RawCheckedValues raw_check_values(std::string_view function, std::span<const RawParam> params,
                                                std::span<const RawValue> given, std::span<const RawValue> trusted);
// The same for a function's arguments, with its own check over them all, unless all were trusted.
[[nodiscard]] RawCheckedValues raw_check_arguments(const RawFunction &function, std::span<const RawValue> given,
                                                   std::span<const RawValue> trusted);
// Runs the function on arguments already checked.
void raw_run_checked(const RawFunction &function, std::span<const RawValue> converted, std::vector<RawValue> &results);
// Fills place, which holds type, from values starting at next.
void raw_store_leaves(const RawType &type, void *place, std::span<const RawValue> values, std::size_t &next);

// Adds the Lua value at index, flattened as param; raises a Lua error if it does not fit.
void raw_flatten_from_lua(lua_State *L, int index, const RawFunction &function, const RawParam &param,
                          std::vector<RawValue> &values);
// The count Lua values from index first, flattened as the function's In and InOut parameters.
[[nodiscard]] std::vector<RawValue> raw_arguments_from_lua(lua_State *L, int first, int count, const RawFunction &function);
// Pushes the return value, then each Out and InOut; a null element is nil. The count pushed.
int raw_push_results(lua_State *L, const RawFunction &function, std::span<const RawValue> values);
// Pushes the In and InOut arguments; a null element that may be nil is nil. The count pushed.
int raw_push_arguments(lua_State *L, const RawFunction &function, std::span<const RawValue> values);

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
