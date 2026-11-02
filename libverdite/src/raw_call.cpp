#include "verdite/raw_call.h"
#include "verdite/mods.h"
#include "verdite/raw_call_bind.h"

#include "raw_call_lua.h"
#include "raw_leaf.h"

#include <lauxlib.h>
#include <lua.h>

#include <algorithm>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace verdite {
namespace {

constexpr RawRange full_range{std::numeric_limits<std::int64_t>::min(),
                              std::numeric_limits<std::int64_t>::max()};
constexpr std::int64_t null_slot = -1;

bool takes_argument(const RawParam &param) {
    return param.role != RawParamRole::Out;
}
bool gives_result(const RawParam &param) {
    return param.role != RawParamRole::In;
}

// ---- checking one call's flattened arguments ----

// A parameter's name in a message, with the way to the leaf: "f(position).vx".
struct ArgumentName {
    const RawFunction &function;
    const RawParam &param;
    std::string below;
};

std::string argument_name(const void *context) {
    const auto &name = *static_cast<const ArgumentName *>(context);
    return std::string(name.function.name) + "(" + std::string(name.param.name) + ")" + name.below;
}

class ArgumentCheck {
public:
    ArgumentCheck(const RawFunction &function, std::span<const RawValue> given)
        : function_(function), given_(given) {
        converted_.reserve(given.size());
    }

    // The arguments as they will be stored, in converted(), or why they will not do.
    [[nodiscard]] std::string run() {
        for (const RawParam &param : function_.params) {
            if (!takes_argument(param))
                continue;
            ArgumentName name{function_, param, {}};
            std::string error = check(*param.type, param.limits.range.value_or(full_range), param, name);
            if (!error.empty())
                return error;
        }
        if (next_ != given_.size())
            return count_error();
        return {};
    }
    [[nodiscard]] std::span<const RawValue> converted() const { return converted_; }

private:
    [[nodiscard]] std::string count_error() const {
        return std::string(function_.name) + " takes " + std::to_string(leaf_count()) +
               " values, not " + std::to_string(given_.size());
    }
    [[nodiscard]] std::size_t leaf_count() const;

    std::string check(const RawType &type, RawRange range, const RawParam &param, ArgumentName &name) {
        if (type.kind == RawKind::Struct)
            return check_struct(type, range, param, name);
        if (type.kind == RawKind::Array)
            return check_array(type, range, param, name);
        if (next_ >= given_.size())
            return count_error();
        return check_leaf(type, range, param, name);
    }

    std::string check_struct(const RawType &type, RawRange range, const RawParam &param, ArgumentName &name) {
        const std::size_t kept = name.below.size();
        for (const RawField &field : type.fields) {
            name.below.append(".").append(field.name);
            const RawRange inner = field.limits.range ? raw_intersect(range, *field.limits.range) : range;
            std::string error = check(*field.type, inner, param, name);
            if (!error.empty())
                return error;
            name.below.resize(kept);
        }
        return {};
    }

    std::string check_array(const RawType &type, RawRange range, const RawParam &param, ArgumentName &name) {
        const std::size_t kept = name.below.size();
        for (std::size_t i = 0; i < type.length; ++i) {
            name.below.append("[").append(std::to_string(i)).append("]");
            std::string error = check(*type.element, range, param, name);
            if (!error.empty())
                return error;
            name.below.resize(kept);
        }
        return {};
    }

    std::string check_leaf(const RawType &type, RawRange range, const RawParam &param, ArgumentName &name) {
        const RawValue &given = given_[next_++];
        if (param.nil_passes_null && std::holds_alternative<std::int64_t>(given) &&
            std::get<std::int64_t>(given) == null_slot) {
            converted_.push_back(given);
            return {};
        }
        RawValue value = raw_as_kind(type, given);
        std::string error = raw_leaf_error(type, range, value, {&name, argument_name});
        if (!error.empty())
            return error;
        if (param.check) {
            const std::string_view why = param.check(std::get<std::int64_t>(value));
            if (!why.empty())
                return argument_name(&name) + ": " + std::string(why);
        }
        converted_.push_back(std::move(value));
        return {};
    }

    const RawFunction &function_;
    std::span<const RawValue> given_;
    std::vector<RawValue> converted_;
    std::size_t next_ = 0;
};

std::size_t leaves_of(const RawType &type) {
    switch (type.kind) {
    case RawKind::Struct: {
        std::size_t count = 0;
        for (const RawField &field : type.fields)
            count += leaves_of(*field.type);
        return count;
    }
    case RawKind::Array:
        return type.length * leaves_of(*type.element);
    default:
        return 1;
    }
}

std::size_t ArgumentCheck::leaf_count() const {
    std::size_t count = 0;
    for (const RawParam &param : function_.params)
        if (takes_argument(param))
            count += leaves_of(*param.type);
    return count;
}

// Stores a flattened value into place, which holds type. Every value is checked by now.
void store_leaves(const RawType &type, void *place, std::span<const RawValue> values, std::size_t &next) {
    if (type.kind == RawKind::Struct) {
        for (const RawField &field : type.fields)
            store_leaves(*field.type, field.member(place), values, next);
    } else if (type.kind == RawKind::Array) {
        for (std::size_t i = 0; i < type.length; ++i)
            store_leaves(*type.element, type.element_at(place, i), values, next);
    } else {
        type.store(place, values[next++]);
    }
}

void load_leaves(const RawType &type, const void *place, std::vector<RawValue> &values) {
    // The description's accessors take a mutable place; they only read here.
    void *readable = const_cast<void *>(place);
    if (type.kind == RawKind::Struct) {
        for (const RawField &field : type.fields)
            load_leaves(*field.type, field.member(readable), values);
    } else if (type.kind == RawKind::Array) {
        for (std::size_t i = 0; i < type.length; ++i)
            load_leaves(*type.element, type.element_at(readable, i), values);
    } else {
        values.push_back(type.load(place));
    }
}

// Tells the game a call is running, whichever way it ends.
class GameCallScope {
public:
    explicit GameCallScope(const RawCallLayer &layer) : layer_(layer) {
        if (layer_.call_began)
            layer_.call_began();
    }
    ~GameCallScope() {
        if (layer_.call_ended)
            layer_.call_ended();
    }
    GameCallScope(const GameCallScope &) = delete;
    GameCallScope &operator=(const GameCallScope &) = delete;

private:
    const RawCallLayer &layer_;
};

RawCallResult call_function(const RawCallLayer &layer, const RawFunction &function,
                            std::span<const RawValue> arguments) {
    ArgumentCheck check(function, arguments);
    RawCallResult result;
    result.error = check.run();
    if (!result.error.empty())
        return result;
    RawCallSite site(function, check.converted(), result.values);
    const GameCallScope scope(layer);
    function.invoke(site);
    return result;
}

// ---- Lua ----

// Adds the Lua value at index to arguments, flattened as type; raises an error if it does not fit.
void flatten_from_lua(lua_State *L, int index, const RawType &type, const RawFunction &function,
                      const RawParam &param, std::vector<RawValue> &arguments);

std::string lua_argument_name(const RawFunction &function, const RawParam &param) {
    return std::string(function.name) + "(" + std::string(param.name) + ")";
}

void flatten_table_entry(lua_State *L, int table, const RawType &type, const RawFunction &function,
                         const RawParam &param, std::vector<RawValue> &arguments) {
    lua_rawget(L, table);
    if (lua_isnil(L, -1))
        raw_lua_fail(L, lua_argument_name(function, param) + " is missing a part of its " +
                            std::string(type.name) + " table");
    flatten_from_lua(L, lua_gettop(L), type, function, param, arguments);
    lua_pop(L, 1);
}

void flatten_from_lua(lua_State *L, int index, const RawType &type, const RawFunction &function,
                      const RawParam &param, std::vector<RawValue> &arguments) {
    luaL_checkstack(L, 4, "a call's table is too deep");
    if (type.kind == RawKind::Struct || type.kind == RawKind::Array) {
        if (!lua_istable(L, index))
            raw_lua_fail(L, lua_argument_name(function, param) + " needs a table, not " + luaL_typename(L, index));
        if (type.kind == RawKind::Struct) {
            for (const RawField &field : type.fields) {
                lua_pushlstring(L, field.name.data(), field.name.size());
                flatten_table_entry(L, index, *field.type, function, param, arguments);
            }
            return;
        }
        for (std::size_t i = 0; i < type.length; ++i) {
            lua_pushinteger(L, static_cast<lua_Integer>(i));
            flatten_table_entry(L, index, *type.element, function, param, arguments);
        }
        return;
    }
    if (lua_isnil(L, index) && param.nil_passes_null) {
        arguments.emplace_back(null_slot);
        return;
    }
    std::optional<RawValue> value = raw_value_from_lua(L, index);
    if (!value)
        raw_lua_fail(L, lua_argument_name(function, param) + " needs " +
                            std::string(raw_expected_phrase(type.kind)) + ", not " + luaL_typename(L, index));
    arguments.push_back(std::move(*value));
}

// Pushes the value of type from the flattened results, as a table if it is a struct or an array.
void push_flattened(lua_State *L, const RawType &type, std::span<const RawValue> values, std::size_t &next) {
    luaL_checkstack(L, 4, "a call's result is too deep");
    if (type.kind == RawKind::Struct) {
        lua_createtable(L, 0, static_cast<int>(type.fields.size()));
        for (const RawField &field : type.fields) {
            push_flattened(L, *field.type, values, next);
            lua_setfield(L, -2, std::string(field.name).c_str());
        }
    } else if (type.kind == RawKind::Array) {
        lua_createtable(L, static_cast<int>(type.length), 1);
        for (std::size_t i = 0; i < type.length; ++i) {
            push_flattened(L, *type.element, values, next);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i));
        }
    } else {
        raw_push_value(L, values[next++]);
    }
}

// Pushes the return value, then each Out and InOut. The count pushed.
int push_results(lua_State *L, const RawFunction &function, std::span<const RawValue> values) {
    std::size_t next = 0;
    int pushed = 0;
    if (function.returns) {
        if (function.may_return_nil && std::get<std::int64_t>(values[next]) == null_slot) {
            lua_pushnil(L);
            ++next;
        } else {
            push_flattened(L, *function.returns, values, next);
        }
        ++pushed;
    }
    for (const RawParam &param : function.params) {
        if (!gives_result(param))
            continue;
        push_flattened(L, *param.type, values, next);
        ++pushed;
    }
    return pushed;
}

const RawCallLayer &layer_of(lua_State *L) {
    return *static_cast<const RawCallLayer *>(lua_touserdata(L, lua_upvalueindex(1)));
}

int run_function(lua_State *L) {
    const RawCallLayer &layer = layer_of(L);
    const RawFunction &function = layer.functions[static_cast<std::size_t>(lua_tointeger(L, lua_upvalueindex(2)))];
    if (function.when == RawCallWhen::BetweenUpdates && !mod_may_change_game(L))
        raw_lua_fail(L, std::string(function.name) +
                            " may be called only directly in on_between_updates, on the mod's main "
                            "thread: elsewhere the game may be walking what it changes");
    mod_spend_instructions(L, layer.instructions_per_call);
    const auto wanted = static_cast<int>(std::ranges::count_if(function.params, takes_argument));
    const int given = lua_gettop(L);
    if (given != wanted)
        raw_lua_fail(L, std::string(function.name) + " takes " + std::to_string(wanted) +
                            " arguments, not " + std::to_string(given));
    std::vector<RawValue> arguments;
    int index = 0;
    for (const RawParam &param : function.params)
        if (takes_argument(param))
            flatten_from_lua(L, ++index, *param.type, function, param, arguments);
    const RawCallResult result = call_function(layer, function, arguments);
    if (!result.error.empty())
        raw_lua_fail(L, result.error);
    return push_results(L, function, result.values);
}

// ---- the definitions file ----

std::string param_description(const RawParam &param) {
    std::string out = raw_field_description(*param.type, param.limits);
    if (param.nil_passes_null)
        out += out.empty() ? "nil for none" : " nil for none";
    return out;
}

// The signature's results: the return value, then each Out and InOut.
std::string results_text(const RawFunction &function, const std::string &prefix) {
    std::vector<std::string> results;
    if (function.returns)
        results.push_back(raw_type_text(*function.returns, prefix) + (function.may_return_nil ? "?" : ""));
    for (const RawParam &param : function.params)
        if (gives_result(param))
            results.push_back(raw_type_text(*param.type, prefix));
    std::string out;
    for (const std::string &result : results)
        out += (out.empty() ? "" : ", ") + result;
    return out;
}

void append_function(std::string &out, const RawFunction &function, const std::string &prefix,
                     RawTypeTables &tables) {
    if (!function.doc.empty())
        raw_append_doc(out, function.doc);
    if (function.when == RawCallWhen::AnyTime)
        out += "---May be called anywhere.\n";
    else
        out += "---Only directly in on_between_updates.\n";
    for (const RawParam &param : function.params) {
        raw_collect_types(*param.type, tables);
        if (takes_argument(param)) {
            out += "---@param " + std::string(param.name) + " " + raw_type_text(*param.type, prefix);
            out += (param.nil_passes_null ? "?" : "");
            const std::string description = param_description(param);
            if (!description.empty())
                out += " " + description;
            out += "\n";
        }
    }
    if (function.returns)
        raw_collect_types(*function.returns, tables);
    const std::string results = results_text(function, prefix);
    if (!results.empty())
        out += "---@return " + results + "\n";
    out += "function " + std::string(prefix) + std::string(function.name) + "(";
    bool first = true;
    for (const RawParam &param : function.params) {
        if (!takes_argument(param))
            continue;
        out += (first ? "" : ", ") + std::string(param.name);
        first = false;
    }
    out += ") end\n\n";
}

} // namespace

void RawCallSite::take(std::size_t param, void *place) {
    const RawParam &described = function_.params[param];
    if (takes_argument(described))
        store_leaves(*described.type, place, arguments_, next_argument_);
}

void RawCallSite::give(const RawType &type, const void *place) {
    load_leaves(type, place, results_);
}

void RawCallSite::give_param(std::size_t param, const void *place) {
    const RawParam &described = function_.params[param];
    if (gives_result(described))
        give(*described.type, place);
}

RawCallResult raw_call(const RawCallLayer &layer, std::string_view name, std::span<const RawValue> arguments) {
    const auto found = std::ranges::find_if(layer.functions, [&](const RawFunction &f) { return f.name == name; });
    if (found == layer.functions.end()) {
        RawCallResult result;
        result.error = "no function named '" + std::string(name) + "'";
        return result;
    }
    return call_function(layer, *found, arguments);
}

void raw_call_open(lua_State *L, const RawCallLayer &layer) {
    void *memory = lua_newuserdatauv(L, sizeof(RawCallLayer), 0);
    ::new (memory) RawCallLayer(layer);
    const int layer_index = lua_gettop(L);
    raw_push_table_at(L, layer.path);
    for (std::size_t i = 0; i < layer.functions.size(); ++i) {
        const std::string_view name = layer.functions[i].name;
        lua_pushlstring(L, name.data(), name.size());
        lua_pushvalue(L, layer_index);
        lua_pushinteger(L, static_cast<lua_Integer>(i));
        lua_pushcclosure(L, run_function, 2);
        lua_rawset(L, -3);
    }
    lua_settop(L, layer_index - 1);
}

std::string raw_call_definitions(const RawCallLayer &layer) {
    const std::string prefix = std::string(layer.path) + ".";
    RawTypeTables tables;
    std::string out;
    raw_append_doc(out, layer.doc);
    out += "---@class " + std::string(layer.path) + "\n";
    out += std::string(layer.path) + " = {}\n\n";
    for (const RawFunction &function : layer.functions)
        append_function(out, function, prefix, tables);
    raw_append_types(out, tables, prefix);
    return out;
}

} // namespace verdite
