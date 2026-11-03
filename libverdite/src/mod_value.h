#ifndef VERDITE_MOD_VALUE_H
#define VERDITE_MOD_VALUE_H
// A Lua value on its way from one mod's state to another's, private to libverdite. States share
// nothing, so a value is copied out of one into plain C++ and pushed into the other: nil, booleans,
// numbers, strings, and tables of them. Metatables stay behind, and a table reached twice arrives as
// two tables.
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

struct lua_State;

namespace verdite {

struct ModValueEntry;

struct ModValue {
    std::variant<std::monostate, bool, std::int64_t, double, std::string, std::vector<ModValueEntry>> content;
};

struct ModValueEntry {
    ModValue key, value;
};

// Copies the count values from index first, which spends the running mod's budget. Raises a Lua
// error on a value that cannot cross (a function, a coroutine, a table as a key), on tables nested
// too deep, which is also what a cycle comes to, or if the copy would hold over max_bytes in all.
[[nodiscard]] std::vector<ModValue> mod_values_from_lua(lua_State *L, int first, int count, std::size_t max_bytes);

// Pushes the value. Raises a Lua error if the mod's memory cap refuses it.
void mod_value_push(lua_State *L, const ModValue &value);

} // namespace verdite

#endif // VERDITE_MOD_VALUE_H
