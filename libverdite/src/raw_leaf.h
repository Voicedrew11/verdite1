#ifndef VERDITE_RAW_LEAF_H
#define VERDITE_RAW_LEAF_H
// What the raw layer (verdite/raw.h) and the call layer (verdite/raw_call.h) share, private to
// libverdite: the check of one value against its type and limits, its crossing to and from Lua, and
// the definitions file's text for a type.
#include "verdite/raw.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>

struct lua_State;

namespace verdite {

// Names a value in a message. Built only when there is a message to give.
struct RawLeafName {
    const void *context = nullptr;
    std::string (*text)(const void *context) = nullptr;
};

[[nodiscard]] RawRange raw_intersect(RawRange a, RawRange b);
[[nodiscard]] std::string raw_range_text(RawRange range);
[[nodiscard]] std::string_view raw_expected_phrase(RawKind kind);

// Why value may not be stored in a leaf of this type within range; empty if it may.
[[nodiscard]] std::string raw_leaf_error(const RawType &type, RawRange range, const RawValue &value,
                                         RawLeafName name);
// A whole number given to a float is a float: the one conversion a write makes.
[[nodiscard]] RawValue raw_as_kind(const RawType &type, const RawValue &value);

// Raises a Lua error. Lua is built as C++, so luaL_error throws and never returns here.
[[noreturn]] void raw_lua_fail(lua_State *L, const std::string &message);
void raw_push_value(lua_State *L, const RawValue &value);
// The Lua value as a RawValue; nullopt for nil, tables and functions.
[[nodiscard]] std::optional<RawValue> raw_value_from_lua(lua_State *L, int index);
// The table at path ("kf.raw") pushed, made where missing. Raw access: no metamethod runs.
void raw_push_table_at(lua_State *L, std::string_view path);

// The struct and enum types some roots reach, by name (sorted); each once.
struct RawTypeTables {
    std::map<std::string, const RawType *> structs;
    std::map<std::string, const RawType *> enums;
};
void raw_collect_types(const RawType &type, RawTypeTables &tables);
[[nodiscard]] std::string raw_type_text(const RawType &type, const std::string &prefix);
// What a field's description says after its type: its count, range or length, read-only reason, doc.
[[nodiscard]] std::string raw_field_description(const RawType &type, const RawLimits &limits);
void raw_append_doc(std::string &out, std::string_view doc);
void raw_append_types(std::string &out, const RawTypeTables &tables, const std::string &prefix);

} // namespace verdite

#endif // VERDITE_RAW_LEAF_H
