#include "verdite/raw_hook.h"
#include "verdite/raw_hook_bind.h"

#include "raw_call_lua.h"
#include "raw_hook_lua.h"
#include "raw_leaf.h"

#include <lauxlib.h>
#include <lua.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace verdite {
namespace {

bool takes_argument(const RawParam &param) {
    return param.role != RawParamRole::Out;
}
bool gives_result(const RawParam &param) {
    return param.role != RawParamRole::In;
}

// What a replacement must return, as parameters to check: the return value, then each Out and InOut.
std::vector<RawParam> result_params(const RawHookPoint &point) {
    std::vector<RawParam> params;
    if (point.returned.type)
        params.push_back(point.returned);
    for (RawParam param : point.function.params) {
        if (!gives_result(param))
            continue;
        param.role = RawParamRole::In;
        params.push_back(param);
    }
    return params;
}

struct KindBinding {
    RawHookKind kind;
    std::string_view table;
    RawHookReach needs;
};
constexpr std::array<KindBinding, raw_hook_kinds> kind_bindings{{
    {RawHookKind::Before, "before", RawHookReach::Watch},
    {RawHookKind::After, "after", RawHookReach::Watch},
    {RawHookKind::Replace, "replace", RawHookReach::Replace},
}};

int set_hook(lua_State *L) {
    const bool one_handler = lua_gettop(L) == 1 && (lua_isfunction(L, 1) || lua_isnil(L, 1));
    if (!one_handler)
        raw_lua_fail(L, "a hook takes one function, or nil to remove it");
    mod_set_hook(L, static_cast<std::size_t>(lua_tointeger(L, lua_upvalueindex(1))),
                 static_cast<RawHookKind>(lua_tointeger(L, lua_upvalueindex(2))));
    return 0;
}

// The handler's type text: the function's arguments and results as a kind of hook crosses them.
std::string argument_text(const RawFunction &function, const std::string &prefix) {
    std::string out;
    for (const RawParam &param : function.params) {
        if (!takes_argument(param))
            continue;
        out += (out.empty() ? "" : ", ") + std::string(param.name) + ": " + raw_type_text(*param.type, prefix) +
               (param.nil_passes_null ? "?" : "");
    }
    return out;
}

std::string result_names(const RawFunction &function, const std::string &prefix) {
    std::string out;
    if (function.returns)
        out = "result: " + raw_type_text(*function.returns, prefix) + (function.may_return_nil ? "?" : "");
    for (const RawParam &param : function.params)
        if (gives_result(param))
            out += (out.empty() ? "" : ", ") + std::string(param.name) + "_out: " + raw_type_text(*param.type, prefix);
    return out;
}

std::string result_types(const RawFunction &function, const std::string &prefix) {
    std::string out;
    if (function.returns)
        out = raw_type_text(*function.returns, prefix) + (function.may_return_nil ? "?" : "");
    for (const RawParam &param : function.params)
        if (gives_result(param))
            out += (out.empty() ? "" : ", ") + raw_type_text(*param.type, prefix);
    return out;
}

std::string argument_types(const RawFunction &function, const std::string &prefix) {
    std::string out;
    for (const RawParam &param : function.params)
        if (takes_argument(param))
            out += (out.empty() ? "" : ", ") + raw_type_text(*param.type, prefix) + (param.nil_passes_null ? "?" : "");
    return out;
}

std::string joined(const std::string &a, const std::string &b) {
    return a + (a.empty() || b.empty() ? "" : ", ") + b;
}

std::string handler_text(const RawHookPoint &point, RawHookKind kind, const std::string &prefix) {
    const RawFunction &function = point.function;
    const std::string arguments = argument_text(function, prefix);
    const std::string results = result_types(function, prefix);
    switch (kind) {
    case RawHookKind::Before:
        return "fun(" + arguments + ")" +
               (point.reach == RawHookReach::Watch || arguments.empty() ? "" : ": " + argument_types(function, prefix) + "|nil");
    case RawHookKind::After:
        return "fun(" + joined(result_names(function, prefix), arguments) + ")";
    case RawHookKind::Replace:
        return "fun(" + joined("original: fun(" + arguments + ")" + (results.empty() ? "" : ": " + results), arguments) +
               ")" + (results.empty() ? "" : ": " + results);
    }
    return {};
}

std::string_view reach_text(const KindBinding &binding, RawHookReach reach) {
    if (binding.kind == RawHookKind::After)
        return "---After it runs: sees what it returned and what it was given.\n";
    if (binding.kind == RawHookKind::Replace)
        return "---In place of it; original runs it. What the handler returns is checked.\n";
    return reach == RawHookReach::Watch ? "---Before it runs: sees what it was given, and may not change it.\n"
                                        : "---Before it runs: may return every argument to change them, each checked.\n";
}

} // namespace

void RawHookResults::take(const RawType &type, void *place) {
    raw_store_leaves(type, place, values_, next_);
}

RawCallResult RawHookCall::check_arguments(std::span<const RawValue> arguments) const {
    RawCheckedValues checked = raw_check_arguments(point_.function, arguments, given_);
    RawCallResult result;
    if (checked.error.empty() && !checked.all_trusted && point_.reach == RawHookReach::Watch)
        checked.error = std::string(point_.function.name) + " may only be watched: its arguments cannot change";
    result.error = std::move(checked.error);
    result.values = std::move(checked.converted);
    return result;
}

RawCallResult RawHookCall::call_game(std::span<const RawValue> arguments) {
    RawCallResult result;
    if (std::ranges::equal(arguments, given_)) {
        run_as_given_(run_context_, result.values);
    } else {
        RawCallResult checked = check_arguments(arguments);
        if (!checked.error.empty())
            return checked;
        raw_run_checked(point_.function, checked.values, result.values);
    }
    last_results_ = result.values;
    return result;
}

RawCallResult RawHookCall::check_results(std::span<const RawValue> results) const {
    const std::vector<RawParam> params = result_params(point_);
    RawCheckedValues checked = raw_check_values(point_.function.name, params, results, last_results_);
    RawCallResult result;
    result.error = std::move(checked.error);
    result.values = std::move(checked.converted);
    return result;
}

void raw_hook_open(lua_State *L, const RawHookLayer &layer) {
    for (const KindBinding &binding : kind_bindings) {
        raw_push_table_at(L, std::string(layer.path) + "." + std::string(binding.table));
        for (std::size_t i = 0; i < layer.points.size(); ++i) {
            const RawHookPoint &point = layer.points[i];
            if (point.reach < binding.needs)
                continue;
            const std::string_view name = point.function.name;
            lua_pushlstring(L, name.data(), name.size());
            lua_pushinteger(L, static_cast<lua_Integer>(i));
            lua_pushinteger(L, static_cast<lua_Integer>(binding.kind));
            lua_pushcclosure(L, set_hook, 2);
            lua_rawset(L, -3);
        }
        lua_pop(L, 1);
    }
}

std::string raw_hook_definitions(const RawHookLayer &layer) {
    const std::string prefix = std::string(layer.path) + ".";
    RawTypeTables tables;
    std::string out;
    raw_append_doc(out, layer.doc);
    out += "---@class " + std::string(layer.path) + "\n" + std::string(layer.path) + " = {}\n\n";
    for (const KindBinding &binding : kind_bindings) {
        const std::string table = prefix + std::string(binding.table);
        out += table + " = {}\n\n";
        for (const RawHookPoint &point : layer.points) {
            if (point.reach < binding.needs)
                continue;
            for (const RawParam &param : point.function.params)
                raw_collect_types(*param.type, tables);
            if (point.function.returns)
                raw_collect_types(*point.function.returns, tables);
            out += reach_text(binding, point.reach);
            out += "---@param handler (" + handler_text(point, binding.kind, prefix) + ")? nil removes this mod's\n";
            out += "function " + table + "." + std::string(point.function.name) + "(handler) end\n\n";
        }
    }
    raw_append_types(out, tables, prefix);
    return out;
}

} // namespace verdite
