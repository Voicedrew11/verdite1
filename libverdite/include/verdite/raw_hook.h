#ifndef VERDITE_RAW_HOOK_H
#define VERDITE_RAW_HOOK_H
// The hook layer (docs/HOOK_LAYER_PLAN.md): a mod's code before, after or in place of a game
// function, whoever calls it. Game-agnostic: the build routes each of the game's functions through a
// hook point (verdite/raw_hook_bind.h), and the mod host hangs the handlers at RawHookLayer::path
// (ModHostConfig::hooks). Types, limits and crossings are the call layer's (verdite/raw_call.h).
//
// In Lua (KF1 hangs it at kf.hook), each a function taking a handler, or nil to remove the mod's own:
//   kf.hook.before.<function>(function(a, b) ... end)
//       gets what kf.call.<function> takes. May return all of them to change them, or nothing.
//   kf.hook.after.<function>(function(result, a, b) ... end)
//       gets what kf.call.<function> returns, then what the function was given. Returns are ignored.
//   kf.hook.replace.<function>(function(original, a, b) return original(a, b) end)
//       runs in place of the function: gets what kf.call.<function> takes, returns what it returns.
//       original(...) runs the next replacement or the game's own function, as often as it likes.
//
// How far a function may be hooked is its RawHookReach. A value the handler hands on unchanged is
// never refused, even where the game's own is outside the call layer's range; any other is checked as
// a kf.call argument is, before any game code runs.
//
// Mods run in the listed order: before handlers first to last, then the replacements nested with the
// first outermost, then after handlers last to first. A handler runs inline, inside whatever loop
// called the function, so like an event handler it may not make a kf.call that changes state. A mod
// that errs stops; the game goes on with the next mod, or with its own function if nothing has run it.
//
// A call the layer cannot describe skips the hooks and runs the game's function: a null pointer to a
// struct, an object outside its pool, or a call off the host's thread.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "verdite/raw_call.h"

namespace verdite {

enum class RawHookReach : std::uint8_t {
    Watch,            // before and after, which see and change nothing: signature is all it needs
    ChangeArguments,  // a before handler may also change the arguments: their ranges have been read
    Replace,          // and a replacement may run instead: what the callers need back has been read
};

// A game function the build routes through the layer. Made by raw_hook_bind.h, never by hand.
struct RawHookPoint {
    RawFunction function;       // invoke runs the game's own function, past the hook
    RawHookReach reach = RawHookReach::Watch;
    RawParam returned;          // what a replacement must return; its type is function.returns
};

class RawHookCall;

// Runs one call of a hooked function, returning what the game's caller gets: every value valid, as
// call_game and check_results give them. Nothing: as if it had no hook, the game's function runs.
using RawHookFire = std::optional<RawCallResult> (*)(void *context, RawHookCall &call);

// Who answers a hook point: the mod host, or the fuzzer in its place.
struct RawHookDispatch {
    RawHookFire fire = nullptr;
    void *context = nullptr;
};

// The game's whole layer. The tables it views are static and outlive the host.
struct RawHookLayer {
    std::string_view path;                     // "kf.hook"; empty: no hook layer
    std::string_view doc;                      // for the definitions file: what it does not promise
    std::span<const RawHookPoint> points;
    // Parallel to points, read by each hook point on each call: whether to ask dispatch at all.
    std::span<std::atomic<bool>> active;
    RawHookDispatch *dispatch = nullptr;
    std::int64_t instructions_per_fire = 0;    // charged to the handler's budget, for each handler
};

// One call of a hooked function, as a RawHookFire sees it. Arguments and results are flattened as in
// raw_call: In and InOut arguments; the return value, then Out and InOut ones.
class RawHookCall {
public:
    // Runs the game's function as its caller gave it, pointers and all.
    using RunAsGiven = void (*)(void *context, std::vector<RawValue> &results);

    RawHookCall(const RawHookPoint &point, std::size_t index, std::span<const RawValue> given,
                RunAsGiven run_as_given, void *run_context)
        : point_(point), index_(index), given_(given), run_as_given_(run_as_given), run_context_(run_context) {}

    [[nodiscard]] const RawHookPoint &point() const { return point_; }
    [[nodiscard]] std::size_t index() const { return index_; }
    [[nodiscard]] std::span<const RawValue> given() const { return given_; }

    // The arguments converted and checked, for a handler's change: a value the caller gave at that
    // place passes as it is. On an error no game code has run. Watch refuses any change.
    [[nodiscard]] RawCallResult check_arguments(std::span<const RawValue> arguments) const;
    // Runs the game's function past the hook, checking the arguments first, as check_arguments does.
    // Arguments all equal to the caller's run with the caller's own pointers. On an error it did not run.
    [[nodiscard]] RawCallResult call_game(std::span<const RawValue> arguments);
    // A replacement's results converted and checked; values call_game last gave pass as they are.
    [[nodiscard]] RawCallResult check_results(std::span<const RawValue> results) const;

private:
    const RawHookPoint &point_;
    std::size_t index_;
    std::span<const RawValue> given_;
    RunAsGiven run_as_given_;
    void *run_context_;
    std::vector<RawValue> last_results_;
};

// The layer's part of the ---@meta definitions file.
[[nodiscard]] std::string raw_hook_definitions(const RawHookLayer &layer);

} // namespace verdite

#endif // VERDITE_RAW_HOOK_H
