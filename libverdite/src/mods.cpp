// Lua mods (verdite/mods.h). A mod's lua_State allocates through the mod's own allocator, which
// keeps its memory cap, and a count hook spends its instruction budget; both find the mod from the
// state's allocator data. Lua is compiled as C++, so a Lua error is an exception that unwinds the
// bindings' C++ frames. Every use of a mod's state goes through run_protected, so no Lua error,
// not even running out of memory, reaches Lua's panic handler.
#include <verdite/crash.h>
#include <verdite/mods.h>

#include <cJSON.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace verdite {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr int hook_every_instructions = 1000;
constexpr int max_config_depth = 8;
constexpr std::uintmax_t max_manifest_bytes = 1u << 20;

struct StateClose {
    void operator()(lua_State *L) const { lua_close(L); }
};
struct JsonDelete {
    void operator()(cJSON *json) const { cJSON_Delete(json); }
};
struct JsonTextFree {
    void operator()(char *text) const { cJSON_free(text); }
};
using Json = std::unique_ptr<cJSON, JsonDelete>;

struct Mod {
    fs::path dir;
    std::string id, name, version, author, description, entry = "main.lua";
    bool enabled = false;
    std::string error;
    std::string config_json = "{}";
    fs::file_time_type stamp = fs::file_time_type::min();
    std::size_t file_count = 0;
    bool config_dirty = false;
    bool has_settings = false;
    const ModHostConfig *host = nullptr;
    Clock::time_point host_started;
    // The budget, spent by allocate and count_instructions during a call.
    std::size_t memory_bytes = 0;
    std::int64_t instructions_left = 0;
    bool out_of_instructions = false;   // stays set for the call, even if the mod catches the error
    bool memory_refused = false;        // cleared by the next allocation that fits
    std::unique_ptr<lua_State, StateClose> lua;   // last: closed before the counters above go
};

Mod &mod_of_state(lua_State *L) {
    void *mod = nullptr;
    lua_getallocf(L, &mod);
    return *static_cast<Mod *>(mod);
}

void print_line(std::string_view id, std::string_view text) {
    std::printf("[%.*s] %.*s\n", static_cast<int>(id.size()), id.data(), static_cast<int>(text.size()),
                text.data());
}

fs::path utf8_path(std::string_view text) {
    return {std::u8string(text.begin(), text.end())};
}

// ---- the budget ----

void *allocate(void *data, void *block, std::size_t old_size, std::size_t new_size) {
    Mod &mod = *static_cast<Mod *>(data);
    if (!block)
        old_size = 0;   // Lua passes the kind of object being made
    if (new_size == 0) {
        std::free(block);   // Lua's allocator interface is realloc's
        mod.memory_bytes -= old_size;
        return nullptr;
    }
    const std::size_t after = mod.memory_bytes - old_size + new_size;
    if (new_size > old_size && after > mod.host->memory_cap_bytes) {
        mod.memory_refused = true;
        return nullptr;
    }
    void *moved = std::realloc(block, new_size);
    if (!moved)
        return nullptr;
    mod.memory_bytes = after;
    if (new_size > old_size)
        mod.memory_refused = false;   // Lua's retry after an emergency collection fitted
    return moved;
}

void count_instructions(lua_State *L, lua_Debug * /*unused*/) {
    Mod &mod = mod_of_state(L);
    mod.instructions_left -= hook_every_instructions;
    if (mod.instructions_left > 0)
        return;
    // Every instruction from now on errs, so a mod that catches the error errs again in the frame
    // that caught it, out to the host's call.
    mod.out_of_instructions = true;
    lua_sethook(L, count_instructions, LUA_MASKCOUNT, 1);
    luaL_error(L, "over its instruction budget");
}

int add_traceback(lua_State *L) {
    const char *message = lua_tostring(L, 1);
    luaL_traceback(L, L, message ? message : luaL_tolstring(L, 1, nullptr), 1);
    return 1;
}

// Calls function(argument) in the mod's state with a fresh budget. The error, with its stack, or
// none. Going over the budget is an error even when the mod caught it.
std::optional<std::string> run_protected(Mod &mod, lua_CFunction function, lua_Integer argument = 0) {
    lua_State *L = mod.lua.get();
    mod.instructions_left = static_cast<std::int64_t>(mod.host->instructions_per_call);
    mod.out_of_instructions = false;
    mod.memory_refused = false;
    lua_sethook(L, count_instructions, LUA_MASKCOUNT, hook_every_instructions);
    lua_pushcfunction(L, add_traceback);
    lua_pushcfunction(L, function);
    lua_pushinteger(L, argument);
    const int status = lua_pcall(L, 1, 0, -3);
    std::optional<std::string> error;
    if (status != LUA_OK) {
        std::size_t size = 0;
        const char *message = lua_tolstring(L, -1, &size);
        error = message ? std::string(message, size) : std::string("unknown error");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (mod.out_of_instructions)
        return "over its instruction budget (" + std::to_string(mod.host->instructions_per_call) + " per call)";
    if (mod.memory_refused || status == LUA_ERRMEM)
        return "over its memory cap (" + std::to_string(mod.host->memory_cap_bytes >> 20) + " MiB)";
    return error;
}

// ---- files in the mod's folder ----

std::optional<std::string> read_file(const fs::path &path, std::uintmax_t max_bytes) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error || size > max_bytes)
        return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::ostringstream text;
    text << file.rdbuf();
    if (file.bad())
        return std::nullopt;
    return std::move(text).str();
}

// The path below the mod's folder that relative names, or none if it reaches outside the folder,
// by "..", a root or a link.
std::optional<fs::path> inside_mod(const Mod &mod, std::string_view relative) {
    if (relative.find('\0') != std::string_view::npos)
        return std::nullopt;
    const fs::path path = utf8_path(relative);
    if (path.has_root_name() || path.has_root_directory())
        return std::nullopt;
    if (std::any_of(path.begin(), path.end(), [](const fs::path &part) { return part == ".."; }))
        return std::nullopt;
    fs::path joined = (mod.dir / path).lexically_normal();
    std::error_code error;
    const fs::path root = fs::weakly_canonical(mod.dir, error);
    if (error)
        return std::nullopt;
    const fs::path real = fs::weakly_canonical(joined, error);
    if (error)
        return std::nullopt;
    const auto [root_end, real_end] = std::mismatch(root.begin(), root.end(), real.begin(), real.end());
    if (root_end != root.end())
        return std::nullopt;
    return joined;
}

bool reloads_mod(const fs::path &path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".lua" || path.filename() == "mod.json";
}

std::string_view check_view(lua_State *L, int index) {
    std::size_t size = 0;
    const char *text = luaL_checklstring(L, index, &size);
    return {text, size};
}

int fail_result(lua_State *L, const std::string &message) {
    lua_pushnil(L);
    lua_pushlstring(L, message.data(), message.size());
    return 2;
}

// Loads the text chunk at relative, for require, loadfile and dofile. Pushes it, or returns the error.
std::optional<std::string> load_mod_file(lua_State *L, std::string_view relative) {
    const Mod &mod = mod_of_state(L);
    const auto path = inside_mod(mod, relative);
    if (!path)
        return "'" + std::string(relative) + "' is outside the mod's folder";
    const auto text = read_file(*path, mod.host->memory_cap_bytes);
    if (!text)
        return "cannot read '" + std::string(relative) + "'";
    const std::string chunk_name = "@" + std::string(relative);
    if (luaL_loadbufferx(L, text->data(), text->size(), chunk_name.c_str(), "t") != LUA_OK) {
        std::string message = lua_tostring(L, -1) ? lua_tostring(L, -1) : "cannot load";
        lua_pop(L, 1);
        return message;
    }
    return std::nullopt;
}

// ---- the host's own functions: the sandbox's replacements and mod.* ----

std::string joined_arguments(lua_State *L) {
    std::string text;
    const int count = lua_gettop(L);
    for (int i = 1; i <= count; ++i) {
        if (i > 1)
            text += '\t';
        std::size_t size = 0;
        const char *piece = luaL_tolstring(L, i, &size);
        text.append(piece, size);
        lua_pop(L, 1);
    }
    return text;
}

int mod_print(lua_State *L) {
    print_line(mod_of_state(L).id, joined_arguments(L));
    return 0;
}

// load with its mode forced to text: bytecode can crash Lua.
int load_text_only(lua_State *L) {
    lua_settop(L, lua_gettop(L) < 4 ? 3 : 4);
    lua_pushliteral(L, "t");
    lua_replace(L, 3);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

int mod_loadfile(lua_State *L) {
    if (auto error = load_mod_file(L, check_view(L, 1)))
        return fail_result(L, *error);
    if (!lua_isnoneornil(L, 3)) {
        lua_pushvalue(L, 3);
        if (!lua_setupvalue(L, -2, 1))
            lua_pop(L, 1);
    }
    return 1;
}

int mod_dofile(lua_State *L) {
    if (auto error = load_mod_file(L, check_view(L, 1)))
        return luaL_error(L, "%s", error->c_str());
    const int base = lua_gettop(L) - 1;
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L) - base;
}

bool module_name_valid(std::string_view name) {
    if (name.empty() || name.front() == '.' || name.back() == '.' || name.find("..") != std::string_view::npos)
        return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.';
    });
}

// require(name): name.lua or name/init.lua in the mod's folder, dots as folders; run once.
int mod_require(lua_State *L) {
    const std::string name(check_view(L, 1));
    if (!module_name_valid(name))
        return luaL_error(L, "require: '%s' is not a module name in the mod's folder", name.c_str());
    lua_settop(L, 1);
    luaL_getsubtable(L, LUA_REGISTRYINDEX, "verdite.modules");
    if (lua_getfield(L, 2, name.c_str()) != LUA_TNIL)
        return 1;
    lua_pop(L, 1);
    std::string base = name;
    std::replace(base.begin(), base.end(), '.', '/');
    auto error = load_mod_file(L, base + ".lua");
    if (error && load_mod_file(L, base + "/init.lua"))
        return luaL_error(L, "require: %s", error->c_str());
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, 2, name.c_str());
    return 1;
}

int mod_time(lua_State *L) {
    const auto elapsed = Clock::now() - mod_of_state(L).host_started;
    lua_pushnumber(L, std::chrono::duration<double>(elapsed).count());
    return 1;
}

int mod_save_config(lua_State *L) {
    mod_of_state(L).config_dirty = true;
    return 0;
}

void mod_get_id(lua_State *L) {
    const std::string &id = mod_of_state(L).id;
    lua_pushlstring(L, id.data(), id.size());
}

void mod_get_api_version(lua_State *L) {
    const std::string &version = mod_of_state(L).host->api_version;
    lua_pushlstring(L, version.data(), version.size());
}

int fs_read(lua_State *L) {
    const Mod &mod = mod_of_state(L);
    const std::string_view relative = check_view(L, 1);
    const auto path = inside_mod(mod, relative);
    if (!path)
        return fail_result(L, "outside the mod's folder");
    const auto text = read_file(*path, mod.host->memory_cap_bytes);
    if (!text)
        return fail_result(L, "cannot read '" + std::string(relative) + "'");
    lua_pushlstring(L, text->data(), text->size());
    return 1;
}

int fs_write(lua_State *L) {
    const std::string_view relative = check_view(L, 1);
    const std::string_view text = check_view(L, 2);
    const auto path = inside_mod(mod_of_state(L), relative);
    if (!path)
        return fail_result(L, "outside the mod's folder");
    if (reloads_mod(*path))
        return fail_result(L, "a mod cannot write its .lua files or mod.json");
    std::error_code error;
    fs::create_directories(path->parent_path(), error);
    std::ofstream file(*path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    if (!file)
        return fail_result(L, "cannot write '" + std::string(relative) + "'");
    lua_pushboolean(L, 1);
    return 1;
}

int fs_exists(lua_State *L) {
    const auto path = inside_mod(mod_of_state(L), check_view(L, 1));
    std::error_code error;
    lua_pushboolean(L, path && fs::exists(*path, error));
    return 1;
}

// The names in a folder of the mod's, sorted; a folder's name ends in "/".
int fs_list(lua_State *L) {
    const auto path = inside_mod(mod_of_state(L), lua_isnoneornil(L, 1) ? std::string_view() : check_view(L, 1));
    if (!path)
        return fail_result(L, "outside the mod's folder");
    std::vector<std::string> names;
    std::error_code error;
    for (fs::directory_iterator it(*path, error), end; !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().u8string();
        names.emplace_back(name.begin(), name.end());
        if (it->is_directory(error))
            names.back() += '/';
    }
    if (error)
        return fail_result(L, "cannot list the folder");
    std::sort(names.begin(), names.end());
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (std::size_t i = 0; i < names.size(); ++i) {
        lua_pushlstring(L, names[i].data(), names[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    return 1;
}

const std::vector<ModTable> &host_tables() {
    static const std::vector<ModTable> tables = {
        {.path = "mod",
         .doc = "The mod itself: given by the host to every mod.",
         .functions = {{"log", "fun(...: any)", "Prints [id] and the values to the console.", mod_print},
                       {"time", "fun(): number", "Seconds since the game started.", mod_time},
                       {"save_config", "fun()", "Saves config when the callback returns.", mod_save_config}},
         .fields = {{"id", "string", "The mod's id, from mod.json or its folder.", mod_get_id, nullptr},
                    {"api_version", "string", "The game's API version.", mod_get_api_version, nullptr}}},
        {.path = "mod.fs",
         .doc = "Files in the mod's own folder; a path never reaches outside it.",
         .functions = {{"read", "fun(path: string): string?, string?", "The file's bytes, or nil and why.", fs_read},
                       {"write", "fun(path: string, text: string): boolean?, string?",
                        "Writes the file, making its folders; not .lua files or mod.json.", fs_write},
                       {"exists", "fun(path: string): boolean", "Whether the file or folder exists.", fs_exists},
                       {"list", "fun(path?: string): string[]?, string?",
                        "The names in a folder, sorted; folders end in /.", fs_list}},
         .fields = {}},
    };
    return tables;
}

// ---- binding the tables ----

// The table at path ("kf.player") pushed, made where missing. Raw access: no metamethod runs.
void push_table_at(lua_State *L, std::string_view path) {
    lua_pushglobaltable(L);
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = path.find('.', start);
        const std::string_view part = path.substr(start, dot == std::string_view::npos ? dot : dot - start);
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

const ModField *find_field(const ModTable &table, std::string_view name) {
    const auto found = std::find_if(table.fields.begin(), table.fields.end(),
                                    [&](const ModField &field) { return field.name == name; });
    return found == table.fields.end() ? nullptr : &*found;
}

const ModTable &upvalue_table(lua_State *L) {
    return *static_cast<const ModTable *>(lua_touserdata(L, lua_upvalueindex(1)));
}

int index_field(lua_State *L) {
    if (lua_type(L, 2) != LUA_TSTRING) {
        lua_pushnil(L);
        return 1;
    }
    const ModTable &table = upvalue_table(L);
    const std::string_view name = check_view(L, 2);
    const ModField *field = find_field(table, name);
    if (!field)
        return luaL_error(L, "%s has no field '%s'", std::string(table.path).c_str(), std::string(name).c_str());
    field->get(L);
    return 1;
}

int newindex_field(lua_State *L) {
    const ModTable &table = upvalue_table(L);
    const std::string_view name = check_view(L, 2);
    const ModField *field = find_field(table, name);
    if (!field)
        return luaL_error(L, "%s has no field '%s'", std::string(table.path).c_str(), std::string(name).c_str());
    if (!field->set)
        return luaL_error(L, "%s.%s is read-only", std::string(table.path).c_str(), std::string(name).c_str());
    field->set(L, 3);
    return 0;
}

void bind_table(lua_State *L, const ModTable &table) {
    push_table_at(L, table.path);
    for (const ModFunction &function : table.functions) {
        lua_pushlstring(L, function.name.data(), function.name.size());
        lua_pushcfunction(L, function.run);
        lua_rawset(L, -3);
    }
    if (!table.fields.empty()) {
        lua_newtable(L);
        lua_pushlightuserdata(L, const_cast<ModTable *>(&table));   // read back as const
        lua_pushcclosure(L, index_field, 1);
        lua_setfield(L, -2, "__index");
        lua_pushlightuserdata(L, const_cast<ModTable *>(&table));   // read back as const
        lua_pushcclosure(L, newindex_field, 1);
        lua_setfield(L, -2, "__newindex");
        lua_pushliteral(L, "locked");
        lua_setfield(L, -2, "__metatable");
        lua_setmetatable(L, -2);
    }
    lua_pop(L, 1);
}

// ---- config as JSON ----

Json lua_to_json(lua_State *L, int index, int depth) {
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
    case LUA_TBOOLEAN: return Json(cJSON_CreateBool(lua_toboolean(L, index)));
    case LUA_TNUMBER: return Json(cJSON_CreateNumber(lua_tonumber(L, index)));
    case LUA_TSTRING: return Json(cJSON_CreateString(lua_tostring(L, index)));
    case LUA_TTABLE: break;
    default: return nullptr;
    }
    if (depth >= max_config_depth)
        return nullptr;
    luaL_checkstack(L, 3, "config");
    Json object(cJSON_CreateObject());
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            if (Json value = lua_to_json(L, -1, depth + 1))
                cJSON_AddItemToObject(object.get(), lua_tostring(L, -2), value.release());
        }
        lua_pop(L, 1);
    }
    return object;
}

void json_to_lua(lua_State *L, const cJSON *item, int depth) {
    luaL_checkstack(L, 3, "config");
    if (cJSON_IsBool(item)) {
        lua_pushboolean(L, cJSON_IsTrue(item));
    } else if (cJSON_IsNumber(item)) {
        const double number = item->valuedouble;
        const bool whole = number >= -9.0e15 && number <= 9.0e15 && number == static_cast<double>(static_cast<lua_Integer>(number));
        if (whole)
            lua_pushinteger(L, static_cast<lua_Integer>(number));
        else
            lua_pushnumber(L, number);
    } else if (cJSON_IsString(item)) {
        lua_pushstring(L, item->valuestring);
    } else if (cJSON_IsObject(item) && depth < max_config_depth) {
        lua_newtable(L);
        for (const cJSON *child = item->child; child; child = child->next) {
            json_to_lua(L, child, depth + 1);
            lua_setfield(L, -2, child->string);
        }
    } else {
        lua_pushnil(L);
    }
}

void push_config(lua_State *L, const std::string &config_json) {
    const Json parsed(cJSON_ParseWithLength(config_json.data(), config_json.size()));
    if (cJSON_IsObject(parsed.get()))
        json_to_lua(L, parsed.get(), 0);
    else
        lua_newtable(L);
}

// Protected: the global config, as JSON, into the mod's config_json.
int encode_config(lua_State *L) {
    Mod &mod = mod_of_state(L);
    lua_pushglobaltable(L);
    lua_pushliteral(L, "config");
    if (lua_rawget(L, -2) != LUA_TTABLE)
        return 0;
    const Json json = lua_to_json(L, -1, 0);
    if (!json)
        return 0;
    const std::unique_ptr<char, JsonTextFree> text(cJSON_PrintUnformatted(json.get()));
    if (text)
        mod.config_json = text.get();
    return 0;
}

// ---- opening a mod ----

void open_safe_libraries(lua_State *L) {
    constexpr std::array<luaL_Reg, 6> libraries{{{LUA_GNAME, luaopen_base},
                                                 {LUA_COLIBNAME, luaopen_coroutine},
                                                 {LUA_TABLIBNAME, luaopen_table},
                                                 {LUA_STRLIBNAME, luaopen_string},
                                                 {LUA_MATHLIBNAME, luaopen_math},
                                                 {LUA_UTF8LIBNAME, luaopen_utf8}}};
    for (const luaL_Reg &library : libraries) {
        luaL_requiref(L, library.name, library.func, 1);
        lua_pop(L, 1);
    }
    lua_getglobal(L, LUA_STRLIBNAME);
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);
    // debug.traceback alone: the rest of debug reaches the registry and other functions' locals.
    lua_pushcfunction(L, luaopen_debug);
    lua_call(L, 0, 1);
    lua_createtable(L, 0, 1);
    lua_getfield(L, -2, "traceback");
    lua_setfield(L, -2, "traceback");
    lua_setglobal(L, LUA_DBLIBNAME);
    lua_pop(L, 1);
    lua_getglobal(L, "load");
    lua_pushcclosure(L, load_text_only, 1);
    lua_setglobal(L, "load");
    lua_register(L, "loadfile", mod_loadfile);
    lua_register(L, "dofile", mod_dofile);
    lua_register(L, "require", mod_require);
    lua_register(L, "print", mod_print);
}

// Protected: the sandbox, the tables, config, then the entry script run.
int open_mod(lua_State *L) {
    const Mod &mod = mod_of_state(L);
    open_safe_libraries(L);
    for (const ModTable &table : host_tables())
        bind_table(L, table);
    for (const ModTable &table : mod.host->api)
        bind_table(L, table);
    push_config(L, mod.config_json);
    lua_setglobal(L, "config");
    if (auto error = load_mod_file(L, mod.entry))
        return luaL_error(L, "%s", error->c_str());
    lua_call(L, 0, 0);
    return 0;
}

// ---- callbacks ----

struct CallbackName {
    ModCallback callback;
    std::string_view global, old_global, doc;
};
constexpr std::array<CallbackName, 5> callback_names{{
    {ModCallback::Load, "on_load", "", "After the script runs, on every (re)load."},
    {ModCallback::Update, "on_update", "on_frame", "Once per game update: read and change game state here."},
    {ModCallback::Draw, "on_draw", "", "Once per displayed frame: draw, do not change game state."},
    {ModCallback::Settings, "on_settings", "", "While the mod's settings are shown in the Mods panel."},
    {ModCallback::Unload, "on_unload", "", "Before a reload, or when the mod is disabled."},
}};

const CallbackName &name_of(ModCallback callback) {
    return *std::find_if(callback_names.begin(), callback_names.end(),
                         [&](const CallbackName &name) { return name.callback == callback; });
}

bool push_global_function(lua_State *L, std::string_view name) {
    if (name.empty())
        return false;
    lua_pushglobaltable(L);
    lua_pushlstring(L, name.data(), name.size());
    lua_rawget(L, -2);
    lua_remove(L, -2);
    if (lua_type(L, -1) == LUA_TFUNCTION)
        return true;
    lua_pop(L, 1);
    return false;
}

// Protected: the callback numbered by argument 1, if the mod defines it.
int run_callback(lua_State *L) {
    const auto callback = static_cast<ModCallback>(lua_tointeger(L, 1));
    const CallbackName &name = name_of(callback);
    if (push_global_function(L, name.global) || push_global_function(L, name.old_global))
        lua_call(L, 0, 0);
    if (callback == ModCallback::Load) {
        mod_of_state(L).has_settings = push_global_function(L, "on_settings");
        lua_settop(L, 1);
    }
    return 0;
}

// ---- the definitions file ----

void write_doc(std::string &out, std::string_view doc) {
    if (!doc.empty())
        out.append("---").append(doc).append("\n");
}

void write_table_definition(std::string &out, const ModTable &table) {
    out += "\n";
    write_doc(out, table.doc);
    out.append("---@class ").append(table.path).append("\n");
    for (const ModField &field : table.fields) {
        out.append("---@field ").append(field.name).append(" ").append(field.type).append(" ");
        if (!field.set)
            out += "(read-only) ";
        out.append(field.doc).append("\n");
    }
    for (const ModFunction &function : table.functions)
        out.append("---@field ").append(function.name).append(" ").append(function.signature).append(" ")
            .append(function.doc).append("\n");
    out.append(table.path).append(" = {}\n");
}

std::string make_definitions(const ModHostConfig &config) {
    std::string out = "---@meta\n-- The API a mod is given, version " + config.api_version +
                      ".\n-- Written by the game from its binding tables at start, for the Lua Language Server.\n"
                      "-- Do not edit; it is written again when it changes.\n";
    out += "\n---A table kept per mod between runs: numbers, booleans, strings, tables with string keys.\n"
           "---@type table<string, any>\nconfig = {}\n";
    for (const CallbackName &name : callback_names) {
        out += "\n";
        write_doc(out, name.doc);
        out.append("---@type fun()?\n").append(name.global).append(" = nil\n");
    }
    for (const ModTable &table : host_tables())
        write_table_definition(out, table);
    for (const ModTable &table : config.api)
        write_table_definition(out, table);
    return out;
}

// ---- discovery ----

void read_manifest(Mod &mod) {
    const auto folder = mod.dir.filename().u8string();
    mod.id = mod.name = std::string(folder.begin(), folder.end());
    mod.version.clear();
    mod.author.clear();
    mod.description.clear();
    mod.entry = "main.lua";
    std::error_code error;
    const fs::path manifest = mod.dir / "mod.json";
    if (!fs::exists(manifest, error))
        return;
    const auto text = read_file(manifest, max_manifest_bytes);
    const Json json(text ? cJSON_ParseWithLength(text->data(), text->size()) : nullptr);
    if (!cJSON_IsObject(json.get())) {
        mod.error = "mod.json is not a JSON object";
        return;
    }
    const auto read = [&](const char *key, std::string &target) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(json.get(), key);
        if (cJSON_IsString(item) && *item->valuestring)
            target = item->valuestring;
    };
    read("id", mod.id);
    mod.name = mod.id;
    read("name", mod.name);
    read("version", mod.version);
    read("author", mod.author);
    read("description", mod.description);
    read("entry", mod.entry);
}

struct Fingerprint {
    // min, not {}: libstdc++'s file_clock counts from 2174, so every real time is below zero.
    fs::file_time_type stamp = fs::file_time_type::min();
    std::size_t count = 0;
    bool operator==(const Fingerprint &) const = default;
};

// The newest write over mod.json and the .lua files, and how many: adding or deleting one counts.
Fingerprint fingerprint(const fs::path &dir) {
    Fingerprint print;
    std::error_code error;
    for (fs::recursive_directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error) || !reloads_mod(it->path()))
            continue;
        print.stamp = std::max(print.stamp, it->last_write_time(error));
        ++print.count;
    }
    return print;
}

bool is_mod_folder(const fs::directory_entry &entry) {
    std::error_code error;
    return entry.is_directory(error) &&
           (fs::exists(entry.path() / "mod.json", error) || fs::exists(entry.path() / "main.lua", error));
}

} // namespace

struct ModHost::State {
    ModHostConfig config;
    Clock::time_point started = Clock::now();
    std::vector<std::unique_ptr<Mod>> mods;

    Mod *find(std::string_view id) {
        const auto found = std::find_if(mods.begin(), mods.end(), [&](const auto &mod) { return mod->id == id; });
        return found == mods.end() ? nullptr : found->get();
    }

    void store(const Mod &mod) const {
        if (config.write_stored)
            config.write_stored(mod.id, ModStored{mod.enabled, mod.config_json});
    }

    void save_config(Mod &mod) {
        mod.config_dirty = false;
        if (auto error = run_protected(mod, encode_config))
            print_line(mod.id, "cannot save config: " + *error);
        store(mod);
    }

    void unload(Mod &mod) {
        if (!mod.lua)
            return;
        if (mod.config_dirty)
            save_config(mod);
        // Finalizers run in lua_close within one budget; their errors are dropped.
        mod.instructions_left = static_cast<std::int64_t>(config.instructions_per_call);
        lua_sethook(mod.lua.get(), count_instructions, LUA_MASKCOUNT, hook_every_instructions);
        mod.lua.reset();
        mod.has_settings = false;
    }

    void fail(Mod &mod, const std::string &message) {
        mod.error = message;
        print_line(mod.id, "error: " + message);
        crash_event("mod " + mod.id + " failed: " + message);
        unload(mod);
    }

    // Runs the callback; a mod that errs is stopped. False if it failed.
    bool call(Mod &mod, ModCallback callback) {
        if (!mod.lua)
            return false;
        if (auto error = run_protected(mod, run_callback, static_cast<lua_Integer>(callback))) {
            fail(mod, std::string(name_of(callback).global) + ": " + *error);
            return false;
        }
        if (mod.config_dirty)
            save_config(mod);
        return true;
    }

    void stop(Mod &mod) {
        if (mod.lua)
            call(mod, ModCallback::Unload);
        unload(mod);
    }

    void load(Mod &mod) {
        stop(mod);
        mod.error.clear();
        const auto began = Clock::now();
        mod.memory_bytes = 0;
        mod.lua.reset(lua_newstate(allocate, &mod));
        if (!mod.lua) {
            fail(mod, "cannot make a Lua state");
            return;
        }
        if (auto error = run_protected(mod, open_mod)) {
            fail(mod, *error);
            return;
        }
        if (!call(mod, ModCallback::Load))
            return;
        const double elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
        char line[64];
        std::snprintf(line, sizeof line, " (%.1f ms)", elapsed_ms);
        print_line(mod.id, "loaded " + mod.name + (mod.version.empty() ? "" : " " + mod.version) + line);
    }

    void read_stored(Mod &mod) const {
        if (!config.read_stored)
            return;
        if (const auto stored = config.read_stored(mod.id)) {
            mod.enabled = stored->enabled;
            mod.config_json = stored->config_json;
        }
    }

    // A folder seen for the first time: listed, and loaded if it was left enabled.
    void add(const fs::path &dir, const Fingerprint &print) {
        auto mod = std::make_unique<Mod>();
        mod->dir = dir;
        mod->host = &config;
        mod->host_started = started;
        mod->stamp = print.stamp;
        mod->file_count = print.count;
        read_manifest(*mod);
        if (const Mod *other = find(mod->id)) {
            mod->error = "another mod (" + other->dir.filename().string() + ") has the id '" + mod->id + "'";
            std::printf("[mods] %s: %s\n", dir.filename().string().c_str(), mod->error.c_str());
            mods.push_back(std::move(mod));
            return;
        }
        read_stored(*mod);
        std::printf("[mods] found %s%s\n", mod->id.c_str(), mod->enabled ? " (enabled)" : "");
        if (mod->enabled && mod->error.empty())
            load(*mod);
        mods.push_back(std::move(mod));
    }

    void changed(Mod &mod, const Fingerprint &print) {
        mod.stamp = print.stamp;
        mod.file_count = print.count;
        const std::string id = mod.id;
        mod.error.clear();
        read_manifest(mod);
        if (mod.id != id) {
            mod.id = id;   // the stored settings and the panel keep the first id until a restart
            print_line(id, "a changed id in mod.json takes effect after a restart");
        }
        if (const Mod *first = find(mod.id); first != &mod)
            mod.error = "another mod (" + first->dir.filename().string() + ") has the id '" + mod.id + "'";
        print_line(mod.id, "changed on disk");
        if (mod.enabled && mod.error.empty())
            load(mod);
    }

    void remove_missing(const std::vector<fs::path> &found) {
        for (auto it = mods.begin(); it != mods.end();) {
            if (std::find(found.begin(), found.end(), (*it)->dir) != found.end()) {
                ++it;
                continue;
            }
            stop(**it);
            std::printf("[mods] %s removed\n", (*it)->id.c_str());
            it = mods.erase(it);
        }
    }
};

ModHost::ModHost(ModHostConfig config) : state_(std::make_unique<State>()) {
    state_->config = std::move(config);
    std::error_code error;
    fs::create_directories(state_->config.mods_dir, error);
    const fs::path path = state_->config.mods_dir / state_->config.definitions_name;
    const std::string text = definitions();
    if (read_file(path, text.size() + 1) == text)
        return;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    file.close();
    if (!file)
        std::printf("[mods] cannot write %s\n", path.string().c_str());
}

ModHost::~ModHost() {
    for (auto &mod : state_->mods)
        state_->stop(*mod);
}

void ModHost::scan() {
    std::vector<fs::path> found;
    std::error_code error;
    for (fs::directory_iterator it(state_->config.mods_dir, error), end; !error && it != end; it.increment(error))
        if (is_mod_folder(*it))
            found.push_back(it->path());
    std::sort(found.begin(), found.end());
    state_->remove_missing(found);
    for (const fs::path &dir : found) {
        const Fingerprint print = fingerprint(dir);
        const auto existing =
            std::find_if(state_->mods.begin(), state_->mods.end(), [&](const auto &mod) { return mod->dir == dir; });
        if (existing == state_->mods.end())
            state_->add(dir, print);
        else if (Fingerprint{(*existing)->stamp, (*existing)->file_count} != print)
            state_->changed(**existing, print);
    }
    std::stable_sort(state_->mods.begin(), state_->mods.end(),
                     [](const auto &a, const auto &b) { return a->name < b->name; });
}

void ModHost::call_each(ModCallback callback) {
    for (auto &mod : state_->mods)
        state_->call(*mod, callback);
}

void ModHost::call(std::string_view id, ModCallback callback) {
    if (Mod *mod = state_->find(id))
        state_->call(*mod, callback);
}

void ModHost::enable(std::string_view id) {
    Mod *mod = state_->find(id);
    if (!mod || mod->enabled)
        return;
    mod->enabled = true;
    state_->store(*mod);
    state_->load(*mod);
}

void ModHost::disable(std::string_view id) {
    Mod *mod = state_->find(id);
    if (!mod || !mod->enabled)
        return;
    mod->enabled = false;
    state_->stop(*mod);
    mod->error.clear();
    state_->store(*mod);
    print_line(mod->id, "disabled");
}

void ModHost::reload_all() {
    for (auto &mod : state_->mods)
        if (mod->enabled)
            state_->load(*mod);
}

std::vector<ModInfo> ModHost::mods() const {
    std::vector<ModInfo> list;
    list.reserve(state_->mods.size());
    for (const auto &mod : state_->mods) {
        const ModState shown = !mod->error.empty() ? ModState::Failed
                               : mod->lua          ? ModState::Running
                                                   : ModState::Disabled;
        list.push_back({mod->id, mod->name, mod->version, mod->author, mod->description, mod->enabled, shown,
                        mod->error, mod->has_settings});
    }
    return list;
}

std::size_t ModHost::running_count() const {
    return static_cast<std::size_t>(
        std::count_if(state_->mods.begin(), state_->mods.end(), [](const auto &mod) { return mod->lua != nullptr; }));
}

std::string ModHost::definitions() const {
    return make_definitions(state_->config);
}

std::string_view mod_id(lua_State *L) {
    return mod_of_state(L).id;
}

void mod_config_changed(lua_State *L) {
    mod_of_state(L).config_dirty = true;
}

void mod_log(lua_State *L, std::string_view text) {
    print_line(mod_of_state(L).id, text);
}

} // namespace verdite
