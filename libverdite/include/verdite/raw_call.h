#ifndef VERDITE_RAW_CALL_H
#define VERDITE_RAW_CALL_H
// The call layer (docs/CALL_LAYER_PLAN.md): a game's functions, described as plain data and called
// by name, every argument checked before any game code runs. A Lua value never becomes an address.
// Game-agnostic: the game's generated source describes its functions (verdite/raw_call_bind.h); the
// mod host hangs them at RawCallLayer::path (ModHostConfig::calls) and writes their part of the
// definitions file. Types and limits are the raw layer's (verdite/raw.h).
//
// In Lua (KF1 hangs it at kf.call):
//   kf.call.<function>(a, b, ...)      arguments in the game's order, In and InOut parameters only
//   a, b = kf.call.<function>(...)     the return value, then Out and InOut parameters in order
// As they cross:
//   an integer, enum or flags          an integer, in the parameter's range
//   a bool, float or fixed string      a boolean, number or string, as a raw write takes them
//   a struct or array                  a table with every field, or arrays keyed from 0: what
//                                      kf.raw.copy gives. It is copied; the game never sees the table
//   an element of a game pool          its slot, an integer; a null returned is nil
// A wrong count, type or range, a failed check, or a call at the wrong time is a Lua error, which
// stops the mod as a bad raw write does.
//
// When: a BetweenUpdates function may be called only from on_between_updates, on the mod's main
// thread, and not from an event handler raised inside it or inside a coroutine. Elsewhere game code
// below us may be walking what the function changes. An AnyTime function may be called anywhere.
// Each call charges RawCallLayer::instructions_per_call to the callback's budget.
//
// A RunsFrames function is called as a BetweenUpdates one is, and presents frames until it is done.
// Each frame runs the mods' on_update and on_draw, the caller's own included, and they cannot call
// anything that changes the game. Each frame also gives the caller a whole budget for them
// (ModHost::frame_passed), and what was left of the callback's own comes back when the call returns.
// Until the outermost such call ends, no mod is unloaded by the host's own hand: ModHost::scan does
// nothing, and enable, disable and reload_all wait.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "verdite/raw.h"

namespace verdite {

enum class RawCallWhen : std::uint8_t {
    BetweenUpdates,   // changes state or raises events
    AnyTime,          // changes nothing and raises nothing: the listed file says so
    RunsFrames,       // as BetweenUpdates, and presents frames: the listed file says it may
};

enum class RawParamRole : std::uint8_t {
    In,      // from the caller
    Out,     // a non-const pointer or reference the function fills: returned, not passed
    InOut,   // passed and returned
};

// Why value will not do now, as a static string; empty if it will. For what a range cannot say: a
// slot in use, a cell on the loaded floor. Runs after the type's and limits' checks.
using RawArgumentCheck = std::string_view (*)(std::int64_t value);

// The same for arguments that depend on each other, as a static string; empty if they will do. Gets
// them flattened and converted, a pool's null as slot -1. Runs after every parameter's checks.
using RawCallCheck = std::string_view (*)(std::span<const RawValue> arguments);

struct RawParam {
    std::string_view name;                // the decomp's
    const RawType *type = nullptr;        // a pool's element crosses as an Integer: its slot
    RawLimits limits;                     // as a raw field's; read_only has no meaning here
    RawParamRole role = RawParamRole::In;
    RawArgumentCheck check = nullptr;     // Integer and Enum only
    bool nil_passes_null = false;         // a pool element: nil is accepted and passes null
};

class RawCallSite;   // verdite/raw_call_bind.h: only its templates use it

// A function of the game. Made by raw_call_bind.h's templates from the function itself, never by
// hand, so its parameters always match what invoke calls.
struct RawFunction {
    std::string_view name;                // the decomp's: "player_adjust_hp"
    std::span<const RawParam> params;     // in the game's order
    const RawType *returns = nullptr;     // nullptr: void
    bool may_return_nil = false;          // returns a pool element and may return null
    RawCallWhen when = RawCallWhen::BetweenUpdates;
    std::string_view doc;
    void (*invoke)(RawCallSite &site) = nullptr;   // the arguments are already checked
    RawCallCheck check = nullptr;
};

// The game's whole layer. The tables it views are static and outlive the host.
struct RawCallLayer {
    std::string_view path;                    // "kf.call"; empty: no call layer
    std::string_view doc;                     // for the definitions file: what it does not promise
    std::span<const RawFunction> functions;
    std::int64_t instructions_per_call = 0;   // charged to the callback's budget for each call
    // Around each game function but a RunsFrames one, so a game can see a call that must not reach
    // a frame. Either may be null.
    void (*call_began)() = nullptr;
    void (*call_ended)() = nullptr;
    // Charged instead for a RunsFrames call, whose frames have their own budget: this bounds a loop.
    std::int64_t instructions_per_frame_call = 0;
};

// What a call gave back, or why it never ran. Struct and array values are flattened: every leaf in
// declaration order, array elements in index order.
struct RawCallResult {
    std::string error;                // empty: it ran
    std::vector<RawValue> values;     // the return value's leaves, then each Out and InOut's
};

// Calls the function by name with its In and InOut arguments flattened as above, after every check;
// on an error no game code has run. This does not check when: it is the caller's to know it is
// between updates, as Lua's binding and the fuzzer do. Here a pool element's null is slot -1; Lua's
// binding turns it to and from nil.
[[nodiscard]] RawCallResult raw_call(const RawCallLayer &layer, std::string_view name,
                                     std::span<const RawValue> arguments);

// The layer's part of the ---@meta definitions file: a signature per function, with each
// parameter's range or enum names and its when, and doc at the top.
[[nodiscard]] std::string raw_call_definitions(const RawCallLayer &layer);

} // namespace verdite

#endif // VERDITE_RAW_CALL_H
