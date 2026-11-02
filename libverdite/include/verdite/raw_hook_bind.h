#ifndef VERDITE_RAW_HOOK_BIND_H
#define VERDITE_RAW_HOOK_BIND_H
// Routing a game's functions through the hook layer (verdite/raw_hook.h). Only a game's generated
// source and the tests include this. The build renames the game's definition of f to kf_game_f, and
// the generated source defines f itself, so every caller reaches the hook point:
//   s32 f(s32 a) {
//       if (!active[i].load(std::memory_order_relaxed)) return kf_game_f(a);
//       return raw_hook_fire<&kf_game_f>(layer, i, a);
//   }
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "verdite/raw_call_bind.h"
#include "verdite/raw_hook.h"

namespace verdite {

// Reads a call's flattened results back into the game's types. Only the templates below use it.
class RawHookResults {
public:
    explicit RawHookResults(std::span<const RawValue> values) : values_(values) {}
    // Fills place, which holds type, from the next values.
    void take(const RawType &type, void *place);

private:
    std::span<const RawValue> values_;
    std::size_t next_ = 0;
};

namespace raw_detail {

// What a call needs of an argument beyond how it crosses (CrossingArgument): whether this one can be
// described at all, its value as the layer holds it, and handing a changed one back.
template <typename A> struct HookArgument {
    using Stored = typename CrossingArgument<A>::Stored;
    static bool crosses(const A & /*unused*/) { return true; }
    static Stored absorb(const A &argument) { return argument; }
    static void deliver(const A & /*unused*/, const Stored & /*unused*/) {}
};

template <typename T> struct HookArgument<T *> {
    using Crossing = CrossingArgument<T *>;
    using Stored = typename Crossing::Stored;
    static bool crosses(T *argument) {
        if constexpr (Crossing::pooled)
            return !argument || slot_of<std::remove_cv_t<T>>(argument) >= 0;
        else
            return argument != nullptr;
    }
    static Stored absorb(T *argument) {
        if constexpr (Crossing::pooled)
            return slot_of<std::remove_cv_t<T>>(argument);
        else
            return *argument;
    }
    static void deliver(T *argument, const Stored &stored) {
        if constexpr (Crossing::fills)
            *argument = stored;
    }
};

template <typename T> struct HookArgument<T &> {
    using Crossing = CrossingArgument<T &>;
    using Stored = typename Crossing::Stored;
    static bool crosses(T &argument) {
        if constexpr (Crossing::pooled)
            return slot_of<std::remove_cv_t<T>>(&argument) >= 0;
        else
            return true;
    }
    static Stored absorb(T &argument) {
        if constexpr (Crossing::pooled)
            return slot_of<std::remove_cv_t<T>>(&argument);
        else
            return argument;
    }
    static void deliver(T &argument, const Stored &stored) {
        if constexpr (Crossing::fills)
            argument = stored;
    }
};

// A return value as the flattened results hold it, and as the caller gets it. A reference to anything
// but a pool's element has no hook point: a copy could not outlive the call.
template <typename R> struct HookReturn {
    static_assert(!std::is_reference_v<R>, "only a pool's element may be returned by reference");
    using Stored = std::remove_cv_t<R>;
    static R give(const Stored &stored) { return stored; }
};
template <typename T>
    requires PooledStruct<std::remove_cv_t<T>>
struct HookReturn<T *> {
    using Stored = std::int64_t;
    static T *give(Stored slot) {
        return slot < 0 ? nullptr : &RawPoolOf<std::remove_cv_t<T>>::pool()[static_cast<std::size_t>(slot)];
    }
};
template <typename T>
    requires PooledStruct<std::remove_cv_t<T>>
struct HookReturn<T &> {
    using Stored = std::int64_t;
    static T &give(Stored slot) { return RawPoolOf<std::remove_cv_t<T>>::pool()[static_cast<std::size_t>(slot)]; }
};

template <typename R> std::size_t return_pool_length() {
    using Pointee = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<R>>>;
    if constexpr ((std::is_pointer_v<R> || std::is_reference_v<R>) && PooledStruct<Pointee>)
        return RawPoolOf<Pointee>::pool().size();
    else
        return 0;
}

template <typename A>
void give_given(RawCallSite &flatten, const RawParam &param, std::add_lvalue_reference_t<A> argument) {
    if (param.role == RawParamRole::Out)
        return;   // not filled yet: not read
    const typename HookArgument<A>::Stored stored = HookArgument<A>::absorb(argument);
    flatten.give(*param.type, &stored);
}

template <typename A>
void give_filled(RawCallSite &site, std::size_t param, std::add_lvalue_reference_t<A> argument) {
    if constexpr (CrossingArgument<A>::fills) {
        const typename HookArgument<A>::Stored stored = HookArgument<A>::absorb(argument);
        site.give_param(param, &stored);
    }
}

template <typename A>
void deliver_filled(RawHookResults &results, const RawParam &param, std::add_lvalue_reference_t<A> argument) {
    if constexpr (CrossingArgument<A>::fills) {
        typename HookArgument<A>::Stored stored{};
        results.take(*param.type, &stored);
        HookArgument<A>::deliver(argument, stored);
    }
}

// The function run on what its caller gave, its results flattened.
template <auto function, typename R, typename... A, std::size_t... I>
void run_on_given(std::tuple<A...> &arguments, const RawFunction &described, std::vector<RawValue> &results,
                  std::index_sequence<I...> /*unused*/) {
    RawCallSite site(described, {}, results);
    if constexpr (std::is_void_v<R>) {
        function(std::get<I>(arguments)...);
    } else if constexpr (std::is_reference_v<R>) {
        R result = function(std::get<I>(arguments)...);
        CrossingResult<R>::give(site, result);
    } else {
        const R result = function(std::get<I>(arguments)...);
        CrossingResult<R>::give(site, result);
    }
    (give_filled<A>(site, I, std::get<I>(arguments)), ...);
}

template <auto function, typename R, typename... A, std::size_t... I>
R fire_described(const RawHookLayer &layer, std::size_t index, std::tuple<A...> arguments,
                 std::index_sequence<I...> sequence) {
    const RawHookPoint &point = layer.points[index];
    const RawFunction &described = point.function;
    const auto run_game = [&]() -> R { return function(std::get<I>(arguments)...); };
    std::vector<RawValue> given;
    RawCallSite flatten(described, {}, given);
    (give_given<A>(flatten, described.params[I], std::get<I>(arguments)), ...);

    struct AsGiven {
        std::tuple<A...> &arguments;
        const RawFunction &described;
    } as_given{arguments, described};
    const RawHookCall::RunAsGiven run = [](void *context, std::vector<RawValue> &results) {
        auto &call = *static_cast<AsGiven *>(context);
        run_on_given<function, R, A...>(call.arguments, call.described, results, std::index_sequence<I...>{});
    };
    (void)sequence;
    RawHookCall call(point, index, given, run, &as_given);
    const std::optional<RawCallResult> answered = layer.dispatch->fire(layer.dispatch->context, call);
    if (!answered || !answered->error.empty())
        return run_game();

    RawHookResults results(answered->values);
    if constexpr (std::is_void_v<R>) {
        (deliver_filled<A>(results, described.params[I], std::get<I>(arguments)), ...);
    } else {
        typename HookReturn<R>::Stored returned{};
        results.take(*described.returns, &returned);
        (deliver_filled<A>(results, described.params[I], std::get<I>(arguments)), ...);
        return HookReturn<R>::give(returned);
    }
}

template <auto function, typename Signature> struct HookFire;
template <auto function, typename R, typename... A> struct HookFire<function, std::tuple<R, std::tuple<A...>>> {
    static R run(const RawHookLayer &layer, std::size_t index, A... arguments) {
        if (!layer.dispatch || !layer.dispatch->fire || !(HookArgument<A>::crosses(arguments) && ...))
            return function(arguments...);
        return fire_described<function, R, A...>(layer, index, std::tuple<A...>(arguments...),
                                                 std::index_sequence_for<A...>{});
    }
};

} // namespace raw_detail

// The hook point's way in: raw_hook_fire<&kf_game_f>(layer, index, a, b). The game's function runs
// directly when the call cannot be described.
template <auto function, typename... Given>
decltype(auto) raw_hook_fire(const RawHookLayer &layer, std::size_t index, Given &&...given) {
    using Signature = raw_detail::Signature<decltype(function)>;
    return raw_detail::HookFire<function, std::tuple<typename Signature::Result, typename Signature::Arguments>>::run(
        layer, index, std::forward<Given>(given)...);
}

// A hook point's description, from function's own: raw_hook_point<&kf_game_f>(raw_function<&kf_game_f>(...), reach).
// returned says what a replacement must return, beside its type's own limits; a pool's element also
// has its slot range, and may be null only if returned says so, as callers may not expect it, even
// where the game's own function can return null. Called once per function.
template <auto function>
RawHookPoint raw_hook_point(const RawFunction &described, RawHookReach reach, const RawParamListing &returned = {}) {
    using Result = typename raw_detail::Signature<decltype(function)>::Result;
    RawLimits limits = returned.limits;
    if (const std::size_t length = raw_detail::return_pool_length<Result>(); length > 0) {
        const RawRange slots{0, static_cast<std::int64_t>(length) - 1};
        limits.range = limits.range ? RawRange{std::max(limits.range->min, slots.min), std::min(limits.range->max, slots.max)} : slots;
    }
    return {described, reach,
            RawParam{"result", described.returns, limits, RawParamRole::In, returned.check, returned.nil_passes_null}};
}

} // namespace verdite

#endif // VERDITE_RAW_HOOK_BIND_H
