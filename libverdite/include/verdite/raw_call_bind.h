#ifndef VERDITE_RAW_CALL_BIND_H
#define VERDITE_RAW_CALL_BIND_H
// Describing a game's functions for the call layer (verdite/raw_call.h) from the functions
// themselves, so the compiler checks every name and signature. Only a game's generated source and
// the tests include this. A parameter or return with no description is a compile error.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "verdite/raw_bind.h"
#include "verdite/raw_call.h"

namespace verdite {

// Makes T, a struct the game keeps in a pool, cross as its slot wherever a function takes or returns
// T * or T &. A game's generated source specializes it; a pointer to any other described struct
// crosses as the struct. The primary template is undefined.
//   static std::span<T> pool();     the pool, whose length bounds the slot
template <typename T> struct RawPoolOf;

// What the listed file says of one parameter. The template adds its type.
struct RawParamListing {
    std::string_view name;
    RawLimits limits;
    RawParamRole role = RawParamRole::In;
    RawArgumentCheck check = nullptr;
    bool nil_passes_null = false;
};

// What the listed file says of a function.
struct RawFunctionListing {
    RawCallWhen when = RawCallWhen::BetweenUpdates;
    std::string_view doc;
};

// Holds one call's checked arguments and takes its results. Only the templates below use it.
class RawCallSite {
public:
    RawCallSite(const RawFunction &function, std::span<const RawValue> arguments,
                std::vector<RawValue> &results)
        : function_(function), arguments_(arguments), results_(results) {}

    // Fills place, which holds the parameter's type, from the next arguments; an Out takes none.
    void take(std::size_t param, void *place);
    // Adds a value of this type to the results: every leaf.
    void give(const RawType &type, const void *place);
    // Adds an InOut or Out parameter's value to the results; any other adds nothing.
    void give_param(std::size_t param, const void *place);

private:
    const RawFunction &function_;
    std::span<const RawValue> arguments_;
    std::vector<RawValue> &results_;
    std::size_t next_argument_ = 0;
};

namespace raw_detail {

template <typename T>
concept PooledStruct = requires { RawPoolOf<T>::pool(); };

// How a parameter of type A crosses: what is stored for the call, its description and how it is
// handed to the game. By default the value itself.
template <typename A> struct CrossingArgument {
    using Stored = std::remove_cv_t<A>;
    static constexpr bool pooled = false, fills = false;
    static const RawType &type() { return RawTypeOf<Stored>::type(); }
    static A pass(Stored &stored) { return stored; }
};

// A pointer or reference to a struct: a copy of it, which a non-const one may fill.
template <typename T> struct CrossingArgument<T *> {
    using Stored = std::remove_cv_t<T>;
    static constexpr bool pooled = false, fills = !std::is_const_v<T>;
    static const RawType &type() { return RawTypeOf<Stored>::type(); }
    static T *pass(Stored &stored) { return &stored; }
};
template <typename T> struct CrossingArgument<T &> {
    using Stored = std::remove_cv_t<T>;
    static constexpr bool pooled = false, fills = !std::is_const_v<T>;
    static const RawType &type() { return RawTypeOf<Stored>::type(); }
    static T &pass(Stored &stored) { return stored; }
};

// A pointer or reference to a pool's element: its slot, already checked against the pool. The
// generated source guarantees the range; -1 is null where the listed file allows it.
template <typename T>
    requires PooledStruct<std::remove_cv_t<T>>
struct CrossingArgument<T *> {
    using Stored = std::int64_t;
    static constexpr bool pooled = true, fills = false;
    static const RawType &type() { return RawTypeOf<std::int64_t>::type(); }
    static T *pass(Stored &slot) {
        return slot < 0 ? nullptr : &RawPoolOf<std::remove_cv_t<T>>::pool()[static_cast<std::size_t>(slot)];
    }
};
template <typename T>
    requires PooledStruct<std::remove_cv_t<T>>
struct CrossingArgument<T &> {
    using Stored = std::int64_t;
    static constexpr bool pooled = true, fills = false;
    static const RawType &type() { return RawTypeOf<std::int64_t>::type(); }
    static T &pass(Stored &slot) {
        return RawPoolOf<std::remove_cv_t<T>>::pool()[static_cast<std::size_t>(slot)];
    }
};

// The slot of a pool's element, or -1 for null or one outside the pool.
template <typename T> std::int64_t slot_of(const T *element) {
    const std::span<T> pool = RawPoolOf<T>::pool();
    const std::less<const T *> before;
    if (!element || before(element, pool.data()) || !before(element, pool.data() + pool.size()))
        return -1;
    return element - pool.data();
}

// How a return value of type R crosses.
template <typename R> struct CrossingResult {
    using Bare = std::remove_cvref_t<R>;
    static constexpr bool may_be_nil = false;
    static const RawType *type() { return &RawTypeOf<Bare>::type(); }
    static void give(RawCallSite &site, const Bare &value) { site.give(RawTypeOf<Bare>::type(), &value); }
};
template <> struct CrossingResult<void> {
    static constexpr bool may_be_nil = false;
    static const RawType *type() { return nullptr; }
};
template <typename T>
    requires PooledStruct<std::remove_cv_t<T>>
struct CrossingResult<T *> {
    static constexpr bool may_be_nil = true;
    static const RawType *type() { return &RawTypeOf<std::int64_t>::type(); }
    static void give(RawCallSite &site, const T *element) {
        const std::int64_t slot = slot_of<std::remove_cv_t<T>>(element);
        site.give(RawTypeOf<std::int64_t>::type(), &slot);
    }
};
template <typename T>
    requires PooledStruct<std::remove_cv_t<T>>
struct CrossingResult<T &> {
    static constexpr bool may_be_nil = false;
    static const RawType *type() { return &RawTypeOf<std::int64_t>::type(); }
    static void give(RawCallSite &site, const T &element) {
        const std::int64_t slot = slot_of<std::remove_cv_t<T>>(&element);
        site.give(RawTypeOf<std::int64_t>::type(), &slot);
    }
};

template <typename Function> struct Signature;
template <typename R, typename... A> struct Signature<R (*)(A...)> {
    using Result = R;
    using Arguments = std::tuple<A...>;
    static constexpr std::size_t count = sizeof...(A);
};
template <typename R, typename... A> struct Signature<R (*)(A...) noexcept> : Signature<R (*)(A...)> {};

// Fills the arguments, calls the function, and hands back what it gave.
template <auto function, typename R, typename... A, std::size_t... I>
void invoke_with(RawCallSite &site, std::index_sequence<I...> /*unused*/) {
    std::tuple<typename CrossingArgument<A>::Stored...> stored{};
    (site.take(I, &std::get<I>(stored)), ...);
    if constexpr (std::is_void_v<R>) {
        function(CrossingArgument<A>::pass(std::get<I>(stored))...);
    } else if constexpr (std::is_reference_v<R>) {
        R result = function(CrossingArgument<A>::pass(std::get<I>(stored))...);
        CrossingResult<R>::give(site, result);
    } else {
        const R result = function(CrossingArgument<A>::pass(std::get<I>(stored))...);
        CrossingResult<R>::give(site, result);
    }
    (site.give_param(I, &std::get<I>(stored)), ...);
}

template <auto function, typename Signature> struct Invoker;
template <auto function, typename R, typename... A>
struct Invoker<function, std::tuple<R, std::tuple<A...>>> {
    static void invoke(RawCallSite &site) {
        invoke_with<function, R, A...>(site, std::index_sequence_for<A...>{});
    }
    static std::array<const RawType *, sizeof...(A)> types() {
        return {&CrossingArgument<A>::type()...};
    }
    static std::array<bool, sizeof...(A)> fills() { return {CrossingArgument<A>::fills...}; }
    static std::array<bool, sizeof...(A)> pooled() { return {CrossingArgument<A>::pooled...}; }
    static std::array<std::size_t, sizeof...(A)> pool_lengths() {
        return {pool_length<A>()...};
    }
    template <typename Argument> static std::size_t pool_length() {
        using Pointee = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<Argument>>>;
        if constexpr (CrossingArgument<Argument>::pooled)
            return RawPoolOf<Pointee>::pool().size();
        else
            return 0;
    }
};

} // namespace raw_detail

// function's description:
//   raw_function<&player_adjust_hp>("player_adjust_hp", std::array{RawParamListing{...}})
//   raw_function<&player_clear_motion>("player_clear_motion", std::array<RawParamListing, 0>{})
// One listing per parameter, or a compile error. A const pointer or reference is In only; Out and
// InOut need a non-const one, and a non-const one of a non-pool struct must be listed as one of them:
// each is an assert, since the generator writes the roles from the same signature. Called once for
// each function: its parameters live in the template's static storage.
template <auto function, std::size_t count>
RawFunction raw_function(std::string_view name, const std::array<RawParamListing, count> &params,
                         RawFunctionListing listing = {}) {
    using Signature = raw_detail::Signature<decltype(function)>;
    static_assert(Signature::count == count, "one listing per parameter");
    using Crossing = raw_detail::Invoker<function, std::tuple<typename Signature::Result, typename Signature::Arguments>>;
    static std::array<RawParam, count> described{};
    const auto types = Crossing::types();
    [[maybe_unused]] const auto fills = Crossing::fills();
    const auto pooled = Crossing::pooled();
    const auto lengths = Crossing::pool_lengths();
    for (std::size_t i = 0; i < count; ++i) {
        assert(fills[i] == (params[i].role != RawParamRole::In));
        RawLimits limits = params[i].limits;
        if (pooled[i]) {
            const RawRange slots{0, static_cast<std::int64_t>(lengths[i]) - 1};
            limits.range = limits.range ? RawRange{std::max(limits.range->min, slots.min), std::min(limits.range->max, slots.max)} : slots;
        }
        described[i] = {params[i].name, types[i], limits, params[i].role, params[i].check,
                        params[i].nil_passes_null};
    }
    return {name,
            std::span<const RawParam>(described),
            raw_detail::CrossingResult<typename Signature::Result>::type(),
            raw_detail::CrossingResult<typename Signature::Result>::may_be_nil,
            listing.when,
            listing.doc,
            Crossing::invoke};
}

} // namespace verdite

#endif // VERDITE_RAW_CALL_BIND_H
