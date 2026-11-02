#include "raw_leaf.h"

#include <lauxlib.h>
#include <lua.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace verdite {
namespace {

// A leaf being checked: its type, the range the limits on the way leave, and how to name it.
struct Leaf {
    const RawType &type;
    RawRange range;
    RawLeafName name;
};

std::string leaf_name(const Leaf &leaf) {
    return leaf.name.text(leaf.name.context);
}

std::string_view value_phrase(const RawValue &value) {
    if (std::holds_alternative<std::int64_t>(value))
        return "an integer";
    if (std::holds_alternative<bool>(value))
        return "a boolean";
    if (std::holds_alternative<double>(value))
        return "a number";
    return "a string";
}

std::string mismatch(const Leaf &leaf, const RawValue &value) {
    return leaf_name(leaf) + " needs " + std::string(raw_expected_phrase(leaf.type.kind)) +
           ", not " + std::string(value_phrase(value));
}

// The checks every integer kind shares: its width and the limits on the way to it.
std::string check_integer(const Leaf &leaf, const RawValue &value) {
    const auto *number = std::get_if<std::int64_t>(&value);
    if (!number)
        return mismatch(leaf, value);
    const RawRange range = raw_intersect(leaf.range, leaf.type.range);
    if (*number < range.min || *number > range.max)
        return std::to_string(*number) + " is out of range for " + leaf_name(leaf) + " " +
               raw_range_text(range);
    return {};
}

std::string check_enum(const Leaf &leaf, const RawValue &value) {
    std::string error = check_integer(leaf, value);
    if (!error.empty())
        return error;
    const std::int64_t number = std::get<std::int64_t>(value);
    const bool declared =
        std::any_of(leaf.type.values.begin(), leaf.type.values.end(),
                    [&](const RawEnumValue &item) { return item.value == number; });
    if (!declared)
        return std::to_string(number) + " is not a value of " + leaf_name(leaf);
    return {};
}

std::string check_flags(const Leaf &leaf, const RawValue &value) {
    std::string error = check_integer(leaf, value);
    if (!error.empty())
        return error;
    std::uint64_t mask = 0;
    for (const RawEnumValue &item : leaf.type.values)
        mask |= static_cast<std::uint64_t>(item.value);
    const auto bits = static_cast<std::uint64_t>(std::get<std::int64_t>(value));
    if ((bits & ~mask) != 0)
        return std::to_string(bits & ~mask) + " is not a flag of " + leaf_name(leaf);
    return {};
}

std::string check_boolean(const Leaf &leaf, const RawValue &value) {
    if (!std::holds_alternative<bool>(value))
        return mismatch(leaf, value);
    return {};
}

std::string check_number(const Leaf &leaf, const RawValue &value) {
    const auto *number = std::get_if<double>(&value);
    if (!number)
        return mismatch(leaf, value);
    if (!std::isfinite(*number))
        return leaf_name(leaf) + " is not a finite number";
    if (std::fabs(*number) > static_cast<double>(std::numeric_limits<float>::max()))
        return leaf_name(leaf) + " is out of range for a float";
    return {};
}

std::string check_string(const Leaf &leaf, const RawValue &value) {
    const auto *text = std::get_if<std::string>(&value);
    if (!text)
        return mismatch(leaf, value);
    if (text->size() >= leaf.type.length)
        return leaf_name(leaf) + " is too long (at most " +
               std::to_string(leaf.type.length - 1) + " bytes)";
    if (text->find('\0') != std::string::npos)
        return leaf_name(leaf) + " contains a NUL byte";
    return {};
}

// The innermost element of nested arrays, or the type itself.
const RawType &innermost(const RawType &type) {
    const RawType *leaf = &type;
    while (leaf->kind == RawKind::Array)
        leaf = leaf->element;
    return *leaf;
}

std::string join(const std::vector<std::string> &parts) {
    std::string out;
    for (const std::string &part : parts) {
        if (!out.empty())
            out += " ";
        out += part;
    }
    return out;
}

void append_field(std::string &out, std::string_view name, const RawType &type,
                  const RawLimits &limits, const std::string &prefix) {
    out += "---@field " + std::string(name) + " " + raw_type_text(type, prefix);
    const std::string description = raw_field_description(type, limits);
    if (!description.empty())
        out += " " + description;
    out += "\n";
}

void append_struct(std::string &out, const RawType &type, const std::string &prefix) {
    out += "\n---@class " + prefix + std::string(type.name) + "\n";
    for (const RawField &field : type.fields)
        append_field(out, field.name, *field.type, field.limits, prefix);
}

std::string value_list(std::span<const RawEnumValue> values, std::string_view separator) {
    std::string out;
    for (const RawEnumValue &item : values) {
        if (!out.empty())
            out += std::string(separator);
        out += std::string(item.name) + " = " + std::to_string(item.value);
    }
    return out;
}

void append_enum(std::string &out, const RawType &type, const std::string &prefix) {
    out += "\n";
    if (type.kind == RawKind::Flags) {
        out += "---bits: " + value_list(type.values, ", ") + "\n";
    } else {
        for (const RawEnumValue &item : type.values)
            out += "---" + std::string(item.name) + " = " + std::to_string(item.value) + "\n";
    }
    out += "---@alias " + prefix + std::string(type.name) + " integer\n";
}

} // namespace

RawRange raw_intersect(RawRange a, RawRange b) {
    return {std::max(a.min, b.min), std::min(a.max, b.max)};
}

std::string raw_range_text(RawRange range) {
    return "(" + std::to_string(range.min) + " to " + std::to_string(range.max) + ")";
}

std::string_view raw_expected_phrase(RawKind kind) {
    switch (kind) {
    case RawKind::Boolean:
        return "a boolean";
    case RawKind::Number:
        return "a number";
    case RawKind::String:
        return "a string";
    case RawKind::Integer:
    case RawKind::Enum:
    case RawKind::Flags:
        return "an integer";
    case RawKind::Struct:
    case RawKind::Array:
        return "a value";
    }
    return "a value";
}

std::string raw_leaf_error(const RawType &type, RawRange range, const RawValue &value,
                           RawLeafName name) {
    const Leaf leaf{type, range, name};
    switch (leaf.type.kind) {
    case RawKind::Integer:
        return check_integer(leaf, value);
    case RawKind::Enum:
        return check_enum(leaf, value);
    case RawKind::Flags:
        return check_flags(leaf, value);
    case RawKind::Boolean:
        return check_boolean(leaf, value);
    case RawKind::Number:
        return check_number(leaf, value);
    case RawKind::String:
        return check_string(leaf, value);
    case RawKind::Struct:
    case RawKind::Array:
        return leaf_name(leaf) + " is " +
               (type.kind == RawKind::Struct ? "a struct" : "an array") + ", not a value";
    }
    return {};
}

RawValue raw_as_kind(const RawType &type, const RawValue &value) {
    const auto *integer = std::get_if<std::int64_t>(&value);
    if (type.kind == RawKind::Number && integer)
        return static_cast<double>(*integer);
    return value;
}

void raw_lua_fail(lua_State *L, const std::string &message) {
    luaL_error(L, "%s", message.c_str());
    std::abort();
}

void raw_push_value(lua_State *L, const RawValue &value) {
    if (const auto *integer = std::get_if<std::int64_t>(&value)) {
        lua_pushinteger(L, static_cast<lua_Integer>(*integer));
    } else if (const auto *flag = std::get_if<bool>(&value)) {
        lua_pushboolean(L, *flag ? 1 : 0);
    } else if (const auto *number = std::get_if<double>(&value)) {
        lua_pushnumber(L, *number);
    } else {
        const std::string &text = std::get<std::string>(value);
        lua_pushlstring(L, text.data(), text.size());
    }
}

std::optional<RawValue> raw_value_from_lua(lua_State *L, int index) {
    switch (lua_type(L, index)) {
    case LUA_TBOOLEAN:
        return lua_toboolean(L, index) != 0;
    case LUA_TSTRING: {
        std::size_t size = 0;
        const char *text = lua_tolstring(L, index, &size);
        return std::string(text, size);
    }
    case LUA_TNUMBER: {
        int is_integer = 0;
        const lua_Integer integer = lua_tointegerx(L, index, &is_integer);
        if (is_integer)
            return static_cast<std::int64_t>(integer);
        return lua_tonumber(L, index);
    }
    default:
        return std::nullopt;
    }
}

void raw_push_table_at(lua_State *L, std::string_view path) {
    lua_pushglobaltable(L);
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = path.find('.', start);
        const std::string_view part =
            path.substr(start, dot == std::string_view::npos ? dot : dot - start);
        lua_pushlstring(L, part.data(), part.size());
        if (lua_rawget(L, -2) != LUA_TTABLE) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushlstring(L, part.data(), part.size());
            lua_pushvalue(L, -2);
            lua_rawset(L, -4);
        }
        lua_remove(L, -2);
        if (dot == std::string_view::npos)
            return;
        start = dot + 1;
    }
}

void raw_collect_types(const RawType &type, RawTypeTables &tables) {
    switch (type.kind) {
    case RawKind::Struct:
        if (!tables.structs.try_emplace(std::string(type.name), &type).second)
            return;
        for (const RawField &field : type.fields)
            raw_collect_types(*field.type, tables);
        return;
    case RawKind::Enum:
    case RawKind::Flags:
        tables.enums.try_emplace(std::string(type.name), &type);
        return;
    case RawKind::Array:
        raw_collect_types(*type.element, tables);
        return;
    case RawKind::Integer:
    case RawKind::Number:
    case RawKind::Boolean:
    case RawKind::String:
        return;
    }
}

std::string raw_type_text(const RawType &type, const std::string &prefix) {
    switch (type.kind) {
    case RawKind::Integer:
        return "integer";
    case RawKind::Number:
        return "number";
    case RawKind::Boolean:
        return "boolean";
    case RawKind::String:
        return "string";
    case RawKind::Enum:
    case RawKind::Flags:
    case RawKind::Struct:
        return prefix + std::string(type.name);
    case RawKind::Array:
        return "{[integer]: " + raw_type_text(*type.element, prefix) + "}";
    }
    return "any";
}

std::string raw_field_description(const RawType &type, const RawLimits &limits) {
    std::vector<std::string> parts;
    const RawType &leaf = innermost(type);
    if (type.kind == RawKind::Array)
        parts.push_back("(" + std::to_string(type.length) + " elements, from 0)");
    if (leaf.kind == RawKind::Integer) {
        RawRange range = leaf.range;
        if (limits.range)
            range = raw_intersect(range, *limits.range);
        parts.push_back(raw_range_text(range));
    } else if ((leaf.kind == RawKind::Enum || leaf.kind == RawKind::Flags) && limits.range) {
        parts.push_back(raw_range_text(raw_intersect(leaf.range, *limits.range)));
    }
    if (leaf.kind == RawKind::String)
        parts.push_back("(at most " + std::to_string(leaf.length - 1) + " bytes)");
    if (!limits.read_only.empty())
        parts.emplace_back("(read-only: " + std::string(limits.read_only) + ")");
    if (!limits.doc.empty())
        parts.emplace_back(limits.doc);
    return join(parts);
}

void raw_append_doc(std::string &out, std::string_view doc) {
    std::size_t start = 0;
    while (start < doc.size()) {
        const std::size_t end = doc.find('\n', start);
        out += "---" +
               std::string(doc.substr(start, end == std::string_view::npos ? end : end - start)) +
               "\n";
        if (end == std::string_view::npos)
            return;
        start = end + 1;
    }
}

void raw_append_types(std::string &out, const RawTypeTables &tables, const std::string &prefix) {
    for (const auto &entry : tables.structs)
        append_struct(out, *entry.second, prefix);
    for (const auto &entry : tables.enums)
        append_enum(out, *entry.second, prefix);
}

} // namespace verdite
