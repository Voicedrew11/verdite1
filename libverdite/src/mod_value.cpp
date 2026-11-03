// Copying values between mods' states (mod_value.h).
#include "mod_value.h"

#include <verdite/mods.h>

#include <lauxlib.h>
#include <lua.h>

#include <utility>

namespace verdite {
namespace {

constexpr int max_table_depth = 16;
// What a value costs the budget: a wide table reached many times is otherwise far more work than
// the few instructions that built it.
constexpr std::int64_t instructions_per_value = 10;
constexpr std::int64_t string_bytes_per_instruction = 64;

class ValueReader {
public:
    ValueReader(lua_State *L, std::size_t max_bytes) : L_(L), bytes_left_(max_bytes) {}

    ModValue read(int index, int depth) {
        take(sizeof(ModValueEntry), instructions_per_value);
        switch (lua_type(L_, index)) {
        case LUA_TNIL: return {};
        case LUA_TBOOLEAN: return {lua_toboolean(L_, index) != 0};
        case LUA_TNUMBER: return read_number(index);
        case LUA_TSTRING: return read_string(index);
        case LUA_TTABLE: return read_table(index, depth);
        default:
            luaL_error(L_, "a %s cannot cross between mods", luaL_typename(L_, index));
            return {};
        }
    }

private:
    void take(std::size_t bytes, std::int64_t instructions) {
        mod_spend_instructions(L_, instructions);
        if (bytes > bytes_left_)
            luaL_error(L_, "too large to cross between mods");
        bytes_left_ -= bytes;
    }

    ModValue read_number(int index) {
        if (lua_isinteger(L_, index))
            return {static_cast<std::int64_t>(lua_tointeger(L_, index))};
        return {static_cast<double>(lua_tonumber(L_, index))};
    }

    ModValue read_string(int index) {
        std::size_t size = 0;
        const char *text = lua_tolstring(L_, index, &size);
        take(size, static_cast<std::int64_t>(size) / string_bytes_per_instruction);
        return {std::string(text, size)};
    }

    ModValue read_table(int index, int depth) {
        if (depth >= max_table_depth)
            luaL_error(L_, "tables nested too deep to cross between mods (a table holding itself?)");
        luaL_checkstack(L_, 3, "tables too large to cross between mods");
        std::vector<ModValueEntry> entries;
        lua_pushnil(L_);
        while (lua_next(L_, index) != 0) {
            if (lua_type(L_, -2) == LUA_TTABLE)
                luaL_error(L_, "a table as a key cannot cross between mods");
            ModValue key = read(lua_gettop(L_) - 1, depth + 1);
            ModValue value = read(lua_gettop(L_), depth + 1);
            entries.push_back({std::move(key), std::move(value)});
            lua_pop(L_, 1);
        }
        return {std::move(entries)};
    }

    lua_State *L_;
    std::size_t bytes_left_;
};

void push_table(lua_State *L, const std::vector<ModValueEntry> &entries) {
    luaL_checkstack(L, 3, "tables too large to cross between mods");
    lua_createtable(L, 0, static_cast<int>(entries.size()));
    for (const ModValueEntry &entry : entries) {
        mod_value_push(L, entry.key);
        mod_value_push(L, entry.value);
        lua_rawset(L, -3);
    }
}

} // namespace

std::vector<ModValue> mod_values_from_lua(lua_State *L, int first, int count, std::size_t max_bytes) {
    ValueReader reader(L, max_bytes);
    std::vector<ModValue> values;
    values.reserve(static_cast<std::size_t>(count));
    for (int offset = 0; offset < count; ++offset)
        values.push_back(reader.read(first + offset, 0));
    return values;
}

void mod_value_push(lua_State *L, const ModValue &value) {
    luaL_checkstack(L, 1, "values too large to cross between mods");
    if (std::holds_alternative<std::monostate>(value.content)) {
        lua_pushnil(L);
    } else if (const auto *flag = std::get_if<bool>(&value.content)) {
        lua_pushboolean(L, *flag);
    } else if (const auto *integer = std::get_if<std::int64_t>(&value.content)) {
        lua_pushinteger(L, *integer);
    } else if (const auto *number = std::get_if<double>(&value.content)) {
        lua_pushnumber(L, *number);
    } else if (const auto *text = std::get_if<std::string>(&value.content)) {
        lua_pushlstring(L, text->data(), text->size());
    } else {
        push_table(L, std::get<std::vector<ModValueEntry>>(value.content));
    }
}

} // namespace verdite
