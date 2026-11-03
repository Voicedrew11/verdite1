#ifndef VERDITE_RAW_H
#define VERDITE_RAW_H
// The raw layer (docs/RAW_LAYER_PLAN.md): every named global and struct field of a game, described
// as plain data, read and written by path and checked against its type, its range and an array's
// bounds. A Lua value never reaches the game's memory unchecked and never becomes an address.
// Game-agnostic: the game's generated source describes its types (verdite/raw_bind.h) and lists
// its globals; the mod host hangs them at RawLayer::path (ModHostConfig::raw) and writes their
// part of the definitions file.
//
// In Lua (KF1 hangs it at kf.raw):
//   kf.raw.<global>                    a struct or array as a proxy; any other value as itself
//   proxy.<field>, proxy[i]            the same, one step down; arrays index from 0, as the game does
//   proxy.<field> = v, proxy[i] = v    writes one value: an integer, boolean, number or string
//   #proxy                             an array's length
//   pairs(proxy)                       a struct's fields in order, or an array's indices
//   kf.raw.copy(proxy)                 the whole struct or array as a table (arrays keyed from 0)
//   kf.raw.path(proxy), tostring(proxy)   "player_state.vitals"
// A proxy holds its path and resolves it again from the global on every access; it never holds an
// address, so a kept proxy never dangles. A wrong name, index, type or range, a write to a read-only
// field, or an assignment to a struct or array (not a single value) is a Lua error, which stops
// the mod as a bad kf.player write does. No global may be named copy or path.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace verdite {

enum class RawKind : std::uint8_t {
    Integer,   // within range
    Enum,      // one of values, within range
    Flags,     // any combination of values' bits, within range
    Boolean,
    Number,    // a float or double; a write must be finite and within a float's range
    String,    // a fixed char array: fewer than length bytes and no NUL; the rest is zeroed
    Struct,    // fields
    Array,     // length elements of element
};

// A single value as it crosses: Integer, Enum and Flags are integers, String is its bytes up to
// the first NUL.
using RawValue = std::variant<std::int64_t, bool, double, std::string>;

struct RawRange {
    std::int64_t min = 0, max = 0;
};

struct RawEnumValue {
    std::string_view name;   // "KF_MAP_VARIANT_DEFAULT"
    std::int64_t value = 0;
};

struct RawType;

// What the listed file adds to a field or a global.
struct RawLimits {
    std::optional<RawRange> range;   // narrows every integer or enum it reaches: itself or its elements
    std::string_view read_only;      // why it is read-only; empty: writable
    std::string_view doc;
};

struct RawField {
    std::string_view name;                    // the decomp's, or the listed file's alias
    const RawType *type = nullptr;
    void *(*member)(void *parent) = nullptr;  // a member access, generated as a lambda
    RawLimits limits;
};

// A type as the game declares it. Made by raw_bind.h's templates from the C++ type, never by
// hand, so its load, store and element_at always match the memory they are given.
struct RawType {
    RawKind kind = RawKind::Integer;
    std::string_view name;                     // "KfPlayerState", "s16", "KfMapVariant"
    RawRange range;                            // Integer, Enum, Flags: the storage's width and sign
    std::span<const RawEnumValue> values;      // Enum, Flags
    std::span<const RawField> fields;          // Struct, in declaration order
    const RawType *element = nullptr;          // Array
    std::size_t length = 0;                    // Array: elements; String: bytes with the NUL
    RawValue (*load)(const void *place) = nullptr;                 // every kind but Struct, Array
    void (*store)(void *place, const RawValue &value) = nullptr;   // the value already checked
    void *(*element_at)(void *array, std::size_t index) = nullptr;   // Array; index already checked
};

struct RawGlobal {
    std::string_view name;                    // "player_state"
    const RawType *type = nullptr;
    void *(*address)() = nullptr;             // the global, generated as a lambda
    RawLimits limits;
};

// The game's whole layer. The tables it views are static and outlive the host.
struct RawLayer {
    std::string_view path;                    // "kf.raw"; empty: no raw layer
    std::string_view doc;                     // for the definitions file: what it does not promise
    std::span<const RawGlobal> globals;
};

// The value at path, "player_state.vitals.current_hp" or "item_stock[0][5]", or why not. A struct
// or an array is not a value: read its leaves.
struct RawRead {
    std::optional<RawValue> value;
    std::string error;
};
[[nodiscard]] RawRead raw_read(const RawLayer &layer, std::string_view path);
// Writes one value after every check; empty, or why nothing was written.
[[nodiscard]] std::string raw_write(const RawLayer &layer, std::string_view path,
                                    const RawValue &value);

// The layer's part of the ---@meta definitions file: a class per struct, each integer's range and
// each enum's names in its comment, and doc at the top.
[[nodiscard]] std::string raw_definitions(const RawLayer &layer);

} // namespace verdite

#endif // VERDITE_RAW_H
