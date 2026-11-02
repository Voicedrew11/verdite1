#include "verdite/raw.h"

#include "raw_leaf.h"
#include "raw_lua.h"

#include <lauxlib.h>
#include <lua.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace verdite {
namespace {

constexpr std::size_t max_depth = 16;
constexpr std::size_t max_index = 1'000'000'000;
constexpr RawRange full_range{std::numeric_limits<std::int64_t>::min(),
                              std::numeric_limits<std::int64_t>::max()};

// ---- paths ----

// One step down: a struct's field (at: its position in fields) or an array's element.
struct Step {
    bool element = false;
    std::uint32_t at = 0;
};

struct StepList {
    std::array<Step, max_depth> items{};
    std::size_t count = 0;
};

// A place the path reaches: the global and the steps that locate it, and what the way there adds.
// Resolving never builds the path's text: path_text does, for a message or kf.raw.path only.
struct Place {
    const RawLayer *layer = nullptr;
    std::size_t global = 0; // index into the layer's globals
    StepList steps;
    const RawType *type = nullptr;
    void *address = nullptr;
    RawRange range = full_range;     // narrowed by every limit on the way
    std::string_view read_only;      // the first read-only reason on the way; empty: writable
    std::size_t read_only_steps = 0; // the steps taken when that reason applied
};

// A place, or why the path does not reach one.
struct Lookup {
    std::optional<Place> place;
    std::string error;
};

Lookup reached(Place place) {
    return {place, {}};
}
Lookup refused(std::string error) {
    return {std::nullopt, std::move(error)};
}

// One part of a path as written: ".name", or "[index]".
struct Part {
    bool element = false;
    std::string_view name;
    std::size_t index = 0;
};

bool is_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The name at pos, which moves past it; empty if there is none.
std::string_view take_name(std::string_view text, std::size_t &pos) {
    const std::size_t start = pos;
    while (pos < text.size() && is_name_char(text[pos]))
        ++pos;
    return text.substr(start, pos - start);
}

// The decimal index in brackets at pos ("[12]"), which moves past it; nullopt if malformed.
std::optional<std::size_t> take_index(std::string_view text, std::size_t &pos) {
    std::size_t cursor = pos + 1;
    std::size_t value = 0;
    const std::size_t digits_start = cursor;
    while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor]))) {
        value = value * 10 + static_cast<std::size_t>(text[cursor] - '0');
        if (value > max_index)
            return std::nullopt;
        ++cursor;
    }
    if (cursor == digits_start || cursor >= text.size() || text[cursor] != ']')
        return std::nullopt;
    pos = cursor + 1;
    return value;
}

// The part at pos, which moves past it; false if the text there is neither ".name" nor "[index]".
bool take_part(std::string_view text, std::size_t &pos, Part &part) {
    if (text[pos] == '[') {
        const std::optional<std::size_t> index = take_index(text, pos);
        if (!index)
            return false;
        part.element = true;
        part.index = *index;
        return true;
    }
    if (text[pos] != '.')
        return false;
    ++pos;
    part.name = take_name(text, pos);
    return !part.name.empty();
}

// The syntax error in a path, or empty if it is well formed. Checked before any lookup, so a
// malformed path is reported as one even after a name that does not exist.
std::string syntax_error(std::string_view path) {
    std::size_t pos = 0;
    if (take_name(path, pos).empty())
        return path.empty() ? "empty path" : "bad path '" + std::string(path) + "'";
    while (pos < path.size()) {
        Part part;
        if (!take_part(path, pos, part))
            return "bad path '" + std::string(path) + "'";
    }
    return {};
}

// A limit on a global or a field: the range it narrows to, and why it is read-only.
void apply_limits(Place &place, const RawLimits &limits) {
    if (limits.range)
        place.range = raw_intersect(place.range, *limits.range);
    if (!limits.read_only.empty() && place.read_only.empty()) {
        place.read_only = limits.read_only;
        place.read_only_steps = place.steps.count;
    }
}

// The path text of the first steps of place: "player_state.vitals[2]". Built only when asked for.
std::string path_prefix(const Place &place, std::size_t steps) {
    const RawGlobal &global = place.layer->globals[place.global];
    std::string text(global.name);
    const RawType *type = global.type;
    for (std::size_t i = 0; i < steps; ++i) {
        const Step step = place.steps.items[i];
        if (step.element) {
            text += "[" + std::to_string(step.at) + "]";
            type = type->element;
        } else {
            const RawField &field = type->fields[step.at];
            text += "." + std::string(field.name);
            type = field.type;
        }
    }
    return text;
}

std::string path_text(const Place &place) {
    return path_prefix(place, place.steps.count);
}

Place global_place(const RawLayer &layer, std::size_t index) {
    const RawGlobal &global = layer.globals[index];
    Place place;
    place.layer = &layer;
    place.global = index;
    place.type = global.type;
    place.address = global.address();
    apply_limits(place, global.limits);
    return place;
}

std::optional<std::size_t> global_index(const RawLayer &layer, std::string_view name) {
    for (std::size_t i = 0; i < layer.globals.size(); ++i) {
        if (layer.globals[i].name == name)
            return i;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> field_position(const RawType &type, std::string_view name) {
    for (std::size_t i = 0; i < type.fields.size(); ++i) {
        if (type.fields[i].name == name)
            return static_cast<std::uint32_t>(i);
    }
    return std::nullopt;
}

Lookup field_at(const Place &parent, std::size_t at) {
    if (parent.type->kind != RawKind::Struct)
        return refused(path_text(parent) + " is not a struct");
    if (parent.steps.count >= max_depth)
        return refused("path too deep");
    const RawField &field = parent.type->fields[at];
    Place child = parent;
    child.steps.items[child.steps.count++] = Step{false, static_cast<std::uint32_t>(at)};
    child.type = field.type;
    child.address = field.member(parent.address);
    apply_limits(child, field.limits);
    return reached(child);
}

Lookup element_at_index(const Place &parent, std::size_t index) {
    if (parent.type->kind != RawKind::Array)
        return refused(path_text(parent) + " is not an array");
    if (index >= parent.type->length) {
        const std::string element_path = path_text(parent) + "[" + std::to_string(index) + "]";
        return refused(element_path + " is past the end (" + std::to_string(parent.type->length) +
                       " elements)");
    }
    if (parent.steps.count >= max_depth)
        return refused("path too deep");
    Place child = parent;
    child.steps.items[child.steps.count++] = Step{true, static_cast<std::uint32_t>(index)};
    child.type = parent.type->element;
    child.address = parent.type->element_at(parent.address, index);
    return reached(child);
}

// The step a part of a written path takes from parent.
Lookup step_text(const Place &parent, const Part &part) {
    if (part.element)
        return element_at_index(parent, part.index);
    if (parent.type->kind != RawKind::Struct)
        return refused(path_text(parent) + " is not a struct");
    const std::optional<std::uint32_t> at = field_position(*parent.type, part.name);
    if (!at)
        return refused(path_text(parent) + " has no field '" + std::string(part.name) + "'");
    return field_at(parent, *at);
}

Lookup locate_text(const RawLayer &layer, std::string_view path) {
    const std::string error = syntax_error(path);
    if (!error.empty())
        return refused(error);
    std::size_t pos = 0;
    const std::string_view global_name = take_name(path, pos);
    const std::optional<std::size_t> index = global_index(layer, global_name);
    if (!index)
        return refused("no global '" + std::string(global_name) + "'");
    Place place = global_place(layer, *index);
    while (pos < path.size()) {
        Part part;
        take_part(path, pos, part); // well formed: syntax_error has checked the whole path
        Lookup next = step_text(place, part);
        if (!next.place)
            return next;
        place = *next.place;
    }
    return reached(place);
}

// The place a proxy's steps reach now, from its global.
Lookup locate_steps(const RawLayer &layer, std::size_t global, const StepList &steps) {
    Place place = global_place(layer, global);
    for (std::size_t i = 0; i < steps.count; ++i) {
        const Step step = steps.items[i];
        Lookup next = step.element ? element_at_index(place, step.at) : field_at(place, step.at);
        if (!next.place)
            return next;
        place = *next.place;
    }
    return reached(place);
}

// ---- values ----

bool is_container(const RawType &type) {
    return type.kind == RawKind::Struct || type.kind == RawKind::Array;
}

std::string not_a_value(const Place &place) {
    const std::string_view noun = place.type->kind == RawKind::Struct ? "a struct" : "an array";
    return path_text(place) + " is " + std::string(noun) + ", not a value";
}

std::string path_of_place(const void *place) {
    return path_text(*static_cast<const Place *>(place));
}

// Whether value may be stored in place. Store then needs no check of its own.
std::string check_value(const Place &place, const RawValue &value) {
    return raw_leaf_error(*place.type, place.range, value, {&place, path_of_place});
}

std::string write_place(const Place &place, const RawValue &given) {
    if (is_container(*place.type))
        return not_a_value(place);
    if (!place.read_only.empty())
        return path_prefix(place, place.read_only_steps) +
               " is read-only: " + std::string(place.read_only);
    const RawValue value = raw_as_kind(*place.type, given);
    std::string error = check_value(place, value);
    if (!error.empty())
        return error;
    place.type->store(place.address, value);
    return {};
}

RawRead read_place(const Place &place) {
    if (is_container(*place.type))
        return {std::nullopt, not_a_value(place)};
    return {place.type->load(place.address), {}};
}

// ---- Lua ----

constexpr const char *proxy_metatable = "verdite.raw.proxy";

// A proxy: the global and steps its place is reached by. Trivially destructible; never an address.
struct Proxy {
    std::size_t global = 0;
    StepList steps;
};
static_assert(std::is_trivially_destructible_v<Proxy>);
static_assert(std::is_trivially_destructible_v<RawLayer>);

Place resolve_or_raise(lua_State *L, Lookup lookup) {
    if (!lookup.place)
        raw_lua_fail(L, lookup.error);
    return *lookup.place;
}

// The layer, kept as a copy in a full userdata that every binding has as its first upvalue.
const RawLayer &layer_of(lua_State *L) {
    return *static_cast<const RawLayer *>(lua_touserdata(L, lua_upvalueindex(1)));
}

void push_bound(lua_State *L, int layer_index, lua_CFunction function) {
    lua_pushvalue(L, layer_index);
    lua_pushcclosure(L, function, 1);
}

void push_proxy(lua_State *L, const Place &place) {
    void *memory = lua_newuserdatauv(L, sizeof(Proxy), 0);
    ::new (memory) Proxy{place.global, place.steps};
    luaL_setmetatable(L, proxy_metatable);
}

// A value of a leaf, or a proxy of a struct or an array.
void push_place(lua_State *L, const Place &place) {
    if (is_container(*place.type)) {
        push_proxy(L, place);
        return;
    }
    raw_push_value(L, place.type->load(place.address));
}

void write_from_lua(lua_State *L, const Place &target, int index) {
    if (is_container(*target.type))
        raw_lua_fail(L, not_a_value(target));
    const std::optional<RawValue> value = raw_value_from_lua(L, index);
    if (!value)
        raw_lua_fail(L, path_text(target) + " needs " +
                        std::string(raw_expected_phrase(target.type->kind)) + ", not " +
                        luaL_typename(L, index));
    const std::string error = write_place(target, *value);
    if (!error.empty())
        raw_lua_fail(L, error);
}

// The proxy at index, resolved again from its global: the place it reaches now.
Place place_of_proxy(lua_State *L, int index) {
    const auto *proxy = static_cast<const Proxy *>(luaL_testudata(L, index, proxy_metatable));
    if (!proxy)
        raw_lua_fail(L, std::string("expected a raw proxy, got ") + luaL_typename(L, index));
    return resolve_or_raise(L, locate_steps(layer_of(L), proxy->global, proxy->steps));
}

// A key of a proxy: a name for a struct's field, an index from 0 for an array's element.
Part key_part(lua_State *L, int index) {
    Part part;
    if (lua_type(L, index) == LUA_TSTRING) {
        std::size_t size = 0;
        const char *text = lua_tolstring(L, index, &size);
        part.name = std::string_view(text, size);
        return part;
    }
    int is_integer = 0;
    const lua_Integer integer = lua_tointegerx(L, index, &is_integer);
    if (!is_integer)
        raw_lua_fail(L, std::string("a raw proxy is indexed by a name or a number, not ") +
                        luaL_typename(L, index));
    if (integer < 0)
        raw_lua_fail(L, "raw arrays index from 0, and " + std::to_string(integer) + " is not an index");
    part.element = true;
    part.index = static_cast<std::size_t>(integer);
    return part;
}

int proxy_index(lua_State *L) {
    const Place place = place_of_proxy(L, 1);
    push_place(L, resolve_or_raise(L, step_text(place, key_part(L, 2))));
    return 1;
}

int proxy_newindex(lua_State *L) {
    const Place place = place_of_proxy(L, 1);
    write_from_lua(L, resolve_or_raise(L, step_text(place, key_part(L, 2))), 3);
    return 0;
}

int proxy_len(lua_State *L) {
    const Place place = place_of_proxy(L, 1);
    if (place.type->kind != RawKind::Array)
        raw_lua_fail(L, path_text(place) + " is a struct, not an array");
    lua_pushinteger(L, static_cast<lua_Integer>(place.type->length));
    return 1;
}

// The place's path as a Lua string.
void push_path(lua_State *L, const Place &place) {
    const std::string text = path_text(place);
    lua_pushlstring(L, text.data(), text.size());
}

int proxy_tostring(lua_State *L) {
    push_path(L, place_of_proxy(L, 1));
    return 1;
}

// The iterator of pairs(proxy): its state is the proxy, its counter the second upvalue.
int pairs_next(lua_State *L) {
    const Place place = place_of_proxy(L, 1);
    const auto next = static_cast<std::size_t>(lua_tointeger(L, lua_upvalueindex(2)));
    const std::size_t count =
        place.type->kind == RawKind::Struct ? place.type->fields.size() : place.type->length;
    if (next >= count)
        return 0;
    lua_pushinteger(L, static_cast<lua_Integer>(next) + 1);
    lua_replace(L, lua_upvalueindex(2));
    if (place.type->kind == RawKind::Struct) {
        const RawField &field = place.type->fields[next];
        lua_pushlstring(L, field.name.data(), field.name.size());
        push_place(L, resolve_or_raise(L, field_at(place, next)));
        return 2;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(next));
    push_place(L, resolve_or_raise(L, element_at_index(place, next)));
    return 2;
}

int proxy_pairs(lua_State *L) {
    place_of_proxy(L, 1);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, pairs_next, 2);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}

// A struct or array as a plain table; arrays keyed from 0.
void push_copy(lua_State *L, const Place &place) {
    if (!is_container(*place.type)) {
        raw_push_value(L, place.type->load(place.address));
        return;
    }
    lua_newtable(L);
    if (place.type->kind == RawKind::Struct) {
        for (std::size_t i = 0; i < place.type->fields.size(); ++i) {
            const RawField &field = place.type->fields[i];
            lua_pushlstring(L, field.name.data(), field.name.size());
            push_copy(L, resolve_or_raise(L, field_at(place, i)));
            lua_settable(L, -3);
        }
        return;
    }
    for (std::size_t i = 0; i < place.type->length; ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(i));
        push_copy(L, resolve_or_raise(L, element_at_index(place, i)));
        lua_settable(L, -3);
    }
}

int raw_copy(lua_State *L) {
    push_copy(L, place_of_proxy(L, 1));
    return 1;
}

int raw_path(lua_State *L) {
    push_path(L, place_of_proxy(L, 1));
    return 1;
}

std::string_view global_name_key(lua_State *L, int index) {
    if (lua_type(L, index) != LUA_TSTRING)
        raw_lua_fail(L, std::string("the raw layer is indexed by a global's name, not ") +
                        luaL_typename(L, index));
    std::size_t size = 0;
    const char *text = lua_tolstring(L, index, &size);
    return {text, size};
}

// kf.raw.<global>: a leaf's value, or a proxy of a struct or an array.
int raw_index(lua_State *L) {
    const RawLayer &layer = layer_of(L);
    const std::string_view name = global_name_key(L, 2);
    const std::optional<std::size_t> index = global_index(layer, name);
    if (!index)
        raw_lua_fail(L, std::string(layer.path) + " has no global '" + std::string(name) + "'");
    push_place(L, global_place(layer, *index));
    return 1;
}

int raw_newindex(lua_State *L) {
    const RawLayer &layer = layer_of(L);
    const std::string_view name = global_name_key(L, 2);
    const std::optional<std::size_t> index = global_index(layer, name);
    if (!index)
        raw_lua_fail(L, std::string(layer.path) + " has no global '" + std::string(name) + "'");
    write_from_lua(L, global_place(layer, *index), 3);
    return 0;
}

void make_proxy_metatable(lua_State *L, int layer_index) {
    luaL_newmetatable(L, proxy_metatable);
    push_bound(L, layer_index, proxy_index);
    lua_setfield(L, -2, "__index");
    push_bound(L, layer_index, proxy_newindex);
    lua_setfield(L, -2, "__newindex");
    push_bound(L, layer_index, proxy_len);
    lua_setfield(L, -2, "__len");
    push_bound(L, layer_index, proxy_pairs);
    lua_setfield(L, -2, "__pairs");
    push_bound(L, layer_index, proxy_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pushliteral(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
}

void check_no_clash(lua_State *L, const RawLayer &layer) {
    for (const RawGlobal &global : layer.globals) {
        if (global.name == "copy" || global.name == "path")
            raw_lua_fail(L, "a raw global may not be named '" + std::string(global.name) + "'");
    }
}

} // namespace

// Binds kf.raw: a proxy metatable, the layer's functions, and the table's global lookup.
void raw_open(lua_State *L, const RawLayer &layer) {
    check_no_clash(L, layer);
    void *memory = lua_newuserdatauv(L, sizeof(RawLayer), 0);
    ::new (memory) RawLayer(layer);
    const int layer_index = lua_gettop(L);
    make_proxy_metatable(L, layer_index);
    raw_push_table_at(L, layer.path);
    push_bound(L, layer_index, raw_copy);
    lua_setfield(L, -2, "copy");
    push_bound(L, layer_index, raw_path);
    lua_setfield(L, -2, "path");
    lua_newtable(L);
    push_bound(L, layer_index, raw_index);
    lua_setfield(L, -2, "__index");
    push_bound(L, layer_index, raw_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushliteral(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_settop(L, layer_index - 1);
}

RawRead raw_read(const RawLayer &layer, std::string_view path) {
    Lookup lookup = locate_text(layer, path);
    if (!lookup.place)
        return {std::nullopt, std::move(lookup.error)};
    return read_place(*lookup.place);
}

std::string raw_write(const RawLayer &layer, std::string_view path, const RawValue &value) {
    Lookup lookup = locate_text(layer, path);
    if (!lookup.place)
        return std::move(lookup.error);
    return write_place(*lookup.place, value);
}

std::string raw_definitions(const RawLayer &layer) {
    const std::string prefix = std::string(layer.path) + ".";
    RawTypeTables tables;
    std::string out;
    raw_append_doc(out, layer.doc);
    out += "---@class " + std::string(layer.path) + "\n";
    for (const RawGlobal &global : layer.globals) {
        raw_collect_types(*global.type, tables);
        out += "---@field " + std::string(global.name) + " " + raw_type_text(*global.type, prefix);
        const std::string description = raw_field_description(*global.type, global.limits);
        if (!description.empty())
            out += " " + description;
        out += "\n";
    }
    out += "---@field copy fun(proxy: any): table A struct or array as a plain table, arrays from "
           "0.\n";
    out += "---@field path fun(proxy: any): string The path a proxy stands for.\n";
    out += std::string(layer.path) + " = {}\n";
    raw_append_types(out, tables, prefix);
    return out;
}

} // namespace verdite
