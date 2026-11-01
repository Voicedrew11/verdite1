#ifndef VERDITE_RAW_BIND_H
#define VERDITE_RAW_BIND_H
// Describing a game's types for the raw layer (verdite/raw.h) from the C++ types themselves, so
// the compiler checks every name and that each description is its member's type. Only a game's
// generated source and the tests include this.
//
// RawTypeOf<T>::type() describes T. This header gives the integers, bool, float, double, std::array
// and C arrays of described types, and std::array<char, N> and char[N] as a String; a game's
// generated source specializes it for its structs and enums. A type with no description is a
// compile error: the primary template is undefined.
#include "verdite/raw.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace verdite {

template <typename T> struct RawTypeOf;

namespace raw_detail {

template <typename T> RawValue load_integer(const void *place) {
    return static_cast<std::int64_t>(*static_cast<const T *>(place));
}
template <typename T> void store_integer(void *place, const RawValue &value) {
    *static_cast<T *>(place) = static_cast<T>(std::get<std::int64_t>(value));
}
// A u64 above the int64 range reads as negative and cannot be written back above it.
template <typename T> constexpr RawRange integer_range() {
    if constexpr (std::numeric_limits<T>::max() > std::numeric_limits<std::int64_t>::max())
        return {0, std::numeric_limits<std::int64_t>::max()};
    else
        return {static_cast<std::int64_t>(std::numeric_limits<T>::min()),
                static_cast<std::int64_t>(std::numeric_limits<T>::max())};
}

template <typename T> RawValue load_number(const void *place) {
    return static_cast<double>(*static_cast<const T *>(place));
}
template <typename T> void store_number(void *place, const RawValue &value) {
    *static_cast<T *>(place) = static_cast<T>(std::get<double>(value));
}

template <std::size_t N> RawValue load_string(const void *place) {
    const char *text = static_cast<const char *>(place);
    std::size_t size = 0;
    while (size < N && text[size] != '\0')
        ++size;
    return std::string(text, size);
}
template <std::size_t N> void store_string(void *place, const RawValue &value) {
    const std::string &text = std::get<std::string>(value);
    const std::span<char, N> target(static_cast<char *>(place), N);
    std::ranges::fill(target, '\0');
    std::ranges::copy(text, target.begin());
}

template <typename Array> void *element_at(void *array, std::size_t index) {
    return std::addressof((*static_cast<Array *>(array))[index]);
}

// Enum values cross through the enum itself, so a wrapper (KfEnumStorage) that converts to and from
// it works as the enum does.
template <typename Enum, typename Stored> RawValue load_enum(const void *place) {
    return static_cast<std::int64_t>(static_cast<Enum>(*static_cast<const Stored *>(place)));
}
template <typename Enum, typename Stored> void store_enum(void *place, const RawValue &value) {
    *static_cast<Stored *>(place) = static_cast<Enum>(std::get<std::int64_t>(value));
}

// Array holds N of Element and indexes them with [].
template <typename Array, typename Element, std::size_t N> RawType array_type() {
    RawType described;
    described.kind = RawKind::Array;
    described.name = "array";
    described.element = &RawTypeOf<std::remove_cv_t<Element>>::type();
    described.length = N;
    described.element_at = element_at<Array>;
    return described;
}
template <std::size_t N> RawType string_type() {
    RawType described;
    described.kind = RawKind::String;
    described.name = "string";
    described.length = N;
    described.load = load_string<N>;
    described.store = store_string<N>;
    return described;
}
template <typename T> RawType integer_type() {
    RawType described;
    described.kind = RawKind::Integer;
    described.name = std::is_signed_v<T> ? "signed" : "unsigned";
    described.range = integer_range<T>();
    described.load = load_integer<T>;
    described.store = store_integer<T>;
    return described;
}
template <typename T> RawType number_type() {
    RawType described;
    described.kind = RawKind::Number;
    described.name = "number";
    described.load = load_number<T>;
    described.store = store_number<T>;
    return described;
}
inline RawType boolean_type() {
    RawType described;
    described.kind = RawKind::Boolean;
    described.name = "boolean";
    described.load = [](const void *place) -> RawValue { return *static_cast<const bool *>(place); };
    described.store = [](void *place, const RawValue &value) {
        *static_cast<bool *>(place) = std::get<bool>(value);
    };
    return described;
}

inline constexpr std::string_view const_reason = "const in the decomp";

} // namespace raw_detail

template <typename T>
    requires(std::integral<T> && !std::same_as<T, bool>)
struct RawTypeOf<T> {
    static const RawType &type() {
        static const RawType described = raw_detail::integer_type<T>();
        return described;
    }
};

template <> struct RawTypeOf<bool> {
    static const RawType &type() {
        static const RawType described = raw_detail::boolean_type();
        return described;
    }
};

template <std::floating_point T> struct RawTypeOf<T> {
    static const RawType &type() {
        static const RawType described = raw_detail::number_type<T>();
        return described;
    }
};

template <typename T, std::size_t N> struct RawTypeOf<std::array<T, N>> {
    static const RawType &type() {
        static const RawType described = raw_detail::array_type<std::array<T, N>, T, N>();
        return described;
    }
};

template <typename T, std::size_t N> struct RawTypeOf<T[N]> {
    static const RawType &type() {
        static const RawType described = raw_detail::array_type<T[N], T, N>();
        return described;
    }
};

template <std::size_t N> struct RawTypeOf<std::array<char, N>> {
    static const RawType &type() {
        static const RawType described = raw_detail::string_type<N>();
        return described;
    }
};

template <std::size_t N> struct RawTypeOf<char[N]> {
    static const RawType &type() {
        static const RawType described = raw_detail::string_type<N>();
        return described;
    }
};

// A field of Parent reached through access, a lambda `[](Parent &p) -> auto & { return p.x; }`:
// the compiler checks the member's name, and its type picks the description. A const member is
// read-only.
template <typename Parent, auto access>
RawField raw_field(std::string_view name, RawLimits limits = {}) {
    using Member = std::remove_reference_t<decltype(access(std::declval<Parent &>()))>;
    using Bare = std::remove_cv_t<Member>;
    if (std::is_const_v<Member> && limits.read_only.empty())
        limits.read_only = raw_detail::const_reason;
    // A const member is read-only, so store is never called through its address.
    return {name, &RawTypeOf<Bare>::type(),
            [](void *parent) -> void * {
                return const_cast<Bare *>(std::addressof(access(*static_cast<Parent *>(parent))));
            },
            limits};
}

// A global reached through access, `[]() -> auto & { return player_state; }`. A const global is
// read-only.
template <auto access> RawGlobal raw_global(std::string_view name, RawLimits limits = {}) {
    using Object = std::remove_reference_t<decltype(access())>;
    using Bare = std::remove_cv_t<Object>;
    if (std::is_const_v<Object> && limits.read_only.empty())
        limits.read_only = raw_detail::const_reason;
    return {name, &RawTypeOf<Bare>::type(),
            []() -> void * { return const_cast<Bare *>(std::addressof(access())); }, limits};
}

inline RawType raw_struct_type(std::string_view name, std::span<const RawField> fields) {
    RawType described;
    described.kind = RawKind::Struct;
    described.name = name;
    described.fields = fields;
    return described;
}

// An enum stored as Stored: the enum itself, or a game's wrapper that converts to and from Enum
// (KF1's KfEnumStorage<Enum, u8>). Encoded is the integer the value is kept as, which bounds it.
template <typename Enum, typename Stored = Enum, typename Encoded = std::underlying_type_t<Enum>>
    requires std::is_enum_v<Enum>
RawType raw_enum_type(std::string_view name, std::span<const RawEnumValue> values) {
    RawType described;
    described.kind = RawKind::Enum;
    described.name = name;
    described.range = raw_detail::integer_range<Encoded>();
    described.values = values;
    described.load = raw_detail::load_enum<Enum, Stored>;
    described.store = raw_detail::store_enum<Enum, Stored>;
    return described;
}
template <typename Enum, typename Stored = Enum, typename Encoded = std::underlying_type_t<Enum>>
    requires std::is_enum_v<Enum>
RawType raw_flags_type(std::string_view name, std::span<const RawEnumValue> values) {
    RawType described = raw_enum_type<Enum, Stored, Encoded>(name, values);
    described.kind = RawKind::Flags;
    return described;
}

} // namespace verdite

#endif // VERDITE_RAW_BIND_H
