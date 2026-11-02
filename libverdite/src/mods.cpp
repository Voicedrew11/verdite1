// Lua mods (verdite/mods.h). A mod's lua_State allocates through the mod's own allocator, which
// keeps its memory cap, and a count hook spends its instruction budget; both find the mod from the
// state's allocator data. Lua is compiled as C++, so a Lua error is an exception that unwinds the
// bindings' C++ frames. Every use of a mod's state goes through run_protected, so no Lua error,
// not even running out of memory, reaches Lua's panic handler.
#include <verdite/crash.h>
#include <verdite/mods.h>

#include "raw_lua.h"

#include <cJSON.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <utility>
#include <variant>

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

// Each mod's save table as JSON text, by mod id: the host's copy, kept beside the mods.
using SaveTables = std::map<std::string, std::string, std::less<>>;

// A replacement's bytes, shared so the file on disk can change without changing what is loaded.
using AssetBytes = std::shared_ptr<const std::vector<std::uint8_t>>;
// The replacements a mod makes, by disc path: from its assets/ folder, or from mod.assets.set.
using AssetMap = std::map<std::string, AssetBytes, std::less<>>;

// The event being raised on a mod: its values in, and read back out by run_event.
struct RaisedEvent {
    const ModEvent *event = nullptr;
    std::vector<ModEventValue> values;
};

struct Mod {
    fs::path dir;
    std::string id, name, version, author, description, entry = "main.lua";
    bool default_enabled = false;   // mod.json: enabled when never stored
    bool enhancement = false;       // mod.json: held with the enhancements
    bool enabled = false;
    std::string error;
    std::string config_json = "{}";
    fs::file_time_type stamp = fs::file_time_type::min();
    std::size_t file_count = 0;
    bool config_dirty = false;
    bool has_settings = false;
    const ModHostConfig *host = nullptr;
    SaveTables *saves = nullptr;
    RaisedEvent raised;
    std::optional<std::pair<std::string, ModConfigValue>> config_write;   // for write_config_value
    Clock::time_point host_started;
    AssetMap assets;   // replacements for disc files, while the mod is loaded
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

// The file's bytes, if it is a regular file no larger than max_bytes and reads whole.
std::optional<std::vector<std::uint8_t>> read_bytes(const fs::path &path, std::uintmax_t max_bytes) {
    std::error_code error;
    if (!fs::is_regular_file(path, error))
        return std::nullopt;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error || size > max_bytes)
        return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file || file.gcount() != static_cast<std::streamsize>(bytes.size()))
        return std::nullopt;
    return bytes;
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

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool reloads_mod(const fs::path &path) {
    return lowercase(path.extension().string()) == ".lua" || path.filename() == "mod.json";
}

bool is_assets_name(const fs::path &name) {
    return lowercase(name.string()) == "assets";
}

// Whether path, through links too, is below the mod's assets/ folder. Unsure counts as yes.
bool below_assets(const Mod &mod, const fs::path &path) {
    std::error_code root_error, path_error;
    const fs::path root = fs::weakly_canonical(mod.dir, root_error);
    const fs::path real = fs::weakly_canonical(path, path_error);
    if (root_error || path_error)
        return true;
    const fs::path inside = real.lexically_relative(root);
    return !inside.empty() && is_assets_name(*inside.begin());
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

// ---- assets: the files that replace disc files ----

// A file as a replacement: its bytes, or why it may not replace its disc file.
struct AssetLoad {
    AssetBytes bytes;
    std::string refusal;   // set when bytes is not
};

AssetLoad refused(std::string reason) {
    return {nullptr, std::move(reason)};
}

const ModAssetKind *kind_of(const ModHostConfig &host, std::string_view disc_path) {
    const auto found = std::find_if(host.asset_kinds.begin(), host.asset_kinds.end(),
                                    [&](const ModAssetKind &kind) { return disc_path.ends_with(kind.suffix); });
    return found == host.asset_kinds.end() ? nullptr : &*found;
}

// The disc file's size at disc_path, or nothing if the disc has no such file.
std::optional<std::size_t> disc_size_of(const ModHostConfig &host, std::string_view disc_path) {
    if (!host.disc_file_size)
        return std::nullopt;
    return host.disc_file_size(disc_path);
}

// Loads file as the replacement for disc_path, after every check: a regular file that is not a link,
// a kind the game takes, a file on the disc no smaller than this one, and the kind's check.
AssetLoad load_asset(const ModHostConfig &host, const fs::path &file, std::string_view disc_path) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(file, error);
    if (error || !fs::exists(status))
        return refused("cannot be read");
    if (fs::is_symlink(status))
        return refused("is a link; a mod's files are never followed");
    if (!fs::is_regular_file(status))
        return refused("is not a regular file");
    const ModAssetKind *kind = kind_of(host, disc_path);
    if (!kind)
        return refused("is not a kind of file a mod may replace");
    const auto disc_size = disc_size_of(host, disc_path);
    if (!disc_size)
        return refused("is not a file on the disc");
    const std::uintmax_t size = fs::file_size(file, error);
    if (error)
        return refused("cannot be read");
    if (size == 0)
        return refused("is empty");
    if (size > *disc_size)
        return refused("is larger than the disc's file (" + std::to_string(size) + " > " +
                       std::to_string(*disc_size) + " bytes)");
    const auto text = read_file(file, *disc_size);
    if (!text)
        return refused("cannot be read");
    std::vector<std::uint8_t> bytes(text->begin(), text->end());
    if (!kind->check)
        return refused("has no check in the game for " + std::string(kind->suffix) + " files");
    if (auto reason = kind->check(bytes))
        return refused(std::move(*reason));
    return {std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)), {}};
}

// Loads every file under dir/assets/ as the replacement for its path below assets/, into assets.
// Nothing if there is no such folder; else the reason the mod fails, and assets is left as it was.
std::optional<std::string> load_asset_folder(const ModHostConfig &host, const fs::path &dir, AssetMap &assets) {
    const fs::path folder = dir / "assets";
    std::error_code error;
    const fs::file_status status = fs::symlink_status(folder, error);
    if (error || !fs::exists(status))
        return std::nullopt;
    if (fs::is_symlink(status))
        return "assets/ is a link; a mod's files are never followed";
    if (!fs::is_directory(status))
        return "assets is not a folder";
    if (host.asset_kinds.empty())
        return "assets/ holds replacements, and the game takes no kind of file to replace";
    AssetMap loaded;
    for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        if (fs::is_directory(it->symlink_status(error)))
            continue;
        const std::string disc_path = it->path().lexically_relative(folder).generic_string();
        AssetLoad load = load_asset(host, it->path(), disc_path);
        if (!load.refusal.empty())
            return "assets/" + disc_path + " " + load.refusal;
        loaded[disc_path] = std::move(load.bytes);
    }
    if (error)
        return "cannot read assets/";
    assets = std::move(loaded);
    return std::nullopt;
}

// A mod with an assets/ folder may leave out main.lua. An entry its mod.json names must be there.
bool script_left_out(const Mod &mod) {
    std::error_code error;
    if (mod.entry != "main.lua" || !fs::is_directory(mod.dir / "assets", error))
        return false;
    const auto path = inside_mod(mod, mod.entry);
    const bool exists = path && fs::exists(*path, error);
    return path && !error && !exists;
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
    const Mod &mod = mod_of_state(L);
    const auto path = inside_mod(mod, relative);
    if (!path)
        return fail_result(L, "outside the mod's folder");
    if (reloads_mod(*path))
        return fail_result(L, "a mod cannot write its .lua files or mod.json");
    // A write there would change the mod's files, so it would reload, and on_load could write again.
    if (below_assets(mod, *path))
        return fail_result(L, "a mod cannot write under its assets/ folder");
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

// Protected: mod.assets.set(disc_path, file): file replaces disc_path from the game's next load of it.
int assets_set(lua_State *L) {
    Mod &mod = mod_of_state(L);
    const std::string disc_path(check_view(L, 1));
    const std::string_view relative = check_view(L, 2);
    const auto file = inside_mod(mod, relative);
    if (!file)
        return luaL_error(L, "mod.assets.set: '%s' is outside the mod's folder", std::string(relative).c_str());
    AssetLoad load = load_asset(*mod.host, *file, disc_path);
    if (!load.refusal.empty())
        return luaL_error(L, "mod.assets.set: '%s' %s", disc_path.c_str(), load.refusal.c_str());
    mod.assets[disc_path] = std::move(load.bytes);
    return 0;
}

int assets_clear(lua_State *L) {
    Mod &mod = mod_of_state(L);
    const auto found = mod.assets.find(check_view(L, 1));
    if (found != mod.assets.end())
        mod.assets.erase(found);
    return 0;
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
                        "Writes the file, making its folders; not .lua files, mod.json or assets/.", fs_write},
                       {"exists", "fun(path: string): boolean", "Whether the file or folder exists.", fs_exists},
                       {"list", "fun(path?: string): string[]?, string?",
                        "The names in a folder, sorted; folders end in /.", fs_list}},
         .fields = {}},
        {.path = "mod.assets",
         .doc = "Disc files this mod replaces, by disc path (KF/B0/SND0.SEQ). The assets/ folder does the same.",
         .functions = {{"set", "fun(disc_path: string, file: string)",
                        "Replaces the disc file with the mod's file from the game's next load of it; a refusal is an error.",
                        assets_set},
                       {"clear", "fun(disc_path: string)",
                        "No longer replaces the disc file, from the game's next load of it.", assets_clear}},
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

// Pushes the JSON object as a table, or an empty table if it is not an object.
void push_table_json(lua_State *L, std::string_view json) {
    const Json parsed(cJSON_ParseWithLength(json.data(), json.size()));
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

Json json_of(const ModConfigValue &value) {
    if (const bool *flag = std::get_if<bool>(&value))
        return Json(cJSON_CreateBool(*flag));
    if (const double *number = std::get_if<double>(&value))
        return Json(cJSON_CreateNumber(*number));
    return Json(cJSON_CreateString(std::get<std::string>(value).c_str()));
}

// Protected: the pending config_write into the global config, made a table if it is not one.
int write_config_value(lua_State *L) {
    const Mod &mod = mod_of_state(L);
    if (!mod.config_write)
        return luaL_error(L, "no config value to write");
    const auto &[key, value] = *mod.config_write;
    lua_pushglobaltable(L);
    lua_pushliteral(L, "config");
    if (lua_rawget(L, -2) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushliteral(L, "config");
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    const Json json = json_of(value);
    lua_pushlstring(L, key.data(), key.size());
    json_to_lua(L, json.get(), 0);
    lua_rawset(L, -3);
    return 0;
}

// A config JSON object with key set to value; an object of that one key if json is not an object.
std::string with_config_value(std::string_view json, std::string_view key, const ModConfigValue &value) {
    Json object(cJSON_ParseWithLength(json.data(), json.size()));
    if (!cJSON_IsObject(object.get()))
        object = Json(cJSON_CreateObject());
    const std::string name(key);
    cJSON_DeleteItemFromObjectCaseSensitive(object.get(), name.c_str());
    cJSON_AddItemToObject(object.get(), name.c_str(), json_of(value).release());
    const std::unique_ptr<char, JsonTextFree> text(cJSON_PrintUnformatted(object.get()));
    return text ? std::string(text.get()) : std::string(json);
}

std::optional<ModConfigValue> config_value_in(std::string_view json, std::string_view key) {
    const Json object(cJSON_ParseWithLength(json.data(), json.size()));
    const cJSON *item = cJSON_IsObject(object.get()) ? cJSON_GetObjectItemCaseSensitive(object.get(), std::string(key).c_str())
                                                     : nullptr;
    if (cJSON_IsBool(item))
        return ModConfigValue{cJSON_IsTrue(item) != 0};
    if (cJSON_IsNumber(item))
        return ModConfigValue{item->valuedouble};
    if (cJSON_IsString(item))
        return ModConfigValue{std::string(item->valuestring)};
    return std::nullopt;
}

// Protected: the global save, as JSON, into the host's copy for the mod. An error, and the old
// copy kept, if save is not a table of values that serialize.
int encode_save(lua_State *L) {
    Mod &mod = mod_of_state(L);
    lua_pushglobaltable(L);
    lua_pushliteral(L, "save");
    lua_rawget(L, -2);
    if (lua_type(L, -1) != LUA_TTABLE)
        return luaL_error(L, "save is not a table");
    const Json json = lua_to_json(L, -1, 0);
    if (!json)
        return luaL_error(L, "save holds a value that cannot be saved");
    const std::unique_ptr<char, JsonTextFree> text(cJSON_PrintUnformatted(json.get()));
    if (!text)
        return luaL_error(L, "save cannot be printed");
    (*mod.saves)[mod.id] = text.get();
    return 0;
}

// Sets the global save from the mod's entry in the host's copy, or an empty table if it has none.
int apply_save(lua_State *L) {
    const Mod &mod = mod_of_state(L);
    const auto found = mod.saves->find(mod.id);
    push_table_json(L, found == mod.saves->end() ? std::string_view("{}") : std::string_view(found->second));
    lua_setglobal(L, "save");
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
    if (!mod.host->raw.path.empty())
        raw_open(L, mod.host->raw);
    push_table_json(L, mod.config_json);
    lua_setglobal(L, "config");
    apply_save(L);
    if (script_left_out(mod))
        return 0;
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

// ---- events ----

const ModEvent *find_event(const std::vector<ModEvent> &events, std::string_view name) {
    const auto found = std::find_if(events.begin(), events.end(), [&](const ModEvent &event) { return event.name == name; });
    return found == events.end() ? nullptr : &*found;
}

bool is_writable(const ModEventField &field) {
    return field.writable && (field.type == "integer" || field.type == "boolean");
}

bool value_has_type(const ModEventField &field, const ModEventValue &value) {
    if (field.type == "integer")
        return std::holds_alternative<std::int64_t>(value);
    if (field.type == "boolean")
        return std::holds_alternative<bool>(value);
    if (field.type == "string")
        return std::holds_alternative<std::string_view>(value);
    return false;
}

bool values_fit(const ModEvent &event, std::span<const ModEventValue> values) {
    if (values.size() != event.fields.size())
        return false;
    for (std::size_t i = 0; i < values.size(); ++i)
        if (!value_has_type(event.fields[i], values[i]))
            return false;
    return true;
}

void push_value(lua_State *L, const ModEventValue &value) {
    if (const auto *integer = std::get_if<std::int64_t>(&value)) {
        lua_pushinteger(L, *integer);
    } else if (const auto *flag = std::get_if<bool>(&value)) {
        lua_pushboolean(L, *flag);
    } else {
        const std::string_view text = std::get<std::string_view>(value);
        lua_pushlstring(L, text.data(), text.size());
    }
}

// The event's table at index table: each field, by name, from values.
void push_event_fields(lua_State *L, int table, const ModEvent &event, const std::vector<ModEventValue> &values) {
    for (std::size_t i = 0; i < event.fields.size(); ++i) {
        const ModEventField &field = event.fields[i];
        lua_pushlstring(L, field.name.data(), field.name.size());
        push_value(L, values[i]);
        lua_rawset(L, table);
    }
}

// Raises an error unless the handler left a whole number within the field's range; returns it.
std::int64_t integer_from_handler(lua_State *L, const ModEventField &field) {
    int whole = 0;
    const lua_Integer number = lua_tointegerx(L, -1, &whole);
    const bool in_range = number >= field.min && number <= field.max;
    if (lua_type(L, -1) != LUA_TNUMBER || !whole || !in_range) {
        const std::string message = "field '" + std::string(field.name) + "' must be an integer from " +
                                    std::to_string(field.min) + " to " + std::to_string(field.max);
        luaL_error(L, "%s", message.c_str());
    }
    return static_cast<std::int64_t>(number);
}

// Raises an error unless the handler left a boolean; returns it.
bool boolean_from_handler(lua_State *L, const ModEventField &field) {
    if (!lua_isboolean(L, -1)) {
        const std::string message = "field '" + std::string(field.name) + "' must be a boolean";
        luaL_error(L, "%s", message.c_str());
    }
    return lua_toboolean(L, -1) != 0;
}

ModEventValue value_from_handler(lua_State *L, const ModEventField &field) {
    if (field.type == "boolean")
        return boolean_from_handler(L, field);
    return integer_from_handler(L, field);
}

// Reads the writable fields of the table at index table back into values.
void read_back(lua_State *L, int table, const ModEvent &event, std::vector<ModEventValue> &values) {
    for (std::size_t i = 0; i < event.fields.size(); ++i) {
        const ModEventField &field = event.fields[i];
        if (!is_writable(field))
            continue;
        lua_pushlstring(L, field.name.data(), field.name.size());
        lua_rawget(L, table);
        values[i] = value_from_handler(L, field);
        lua_pop(L, 1);
    }
}

// Protected: calls the raised event's handler in the mod, if it defines one, with the table of its
// fields; the writable fields are read back into the mod's copy of the values.
int run_event(lua_State *L) {
    RaisedEvent &raised = mod_of_state(L).raised;
    const ModEvent &event = *raised.event;
    if (!push_global_function(L, event.name))
        return 0;
    const int handler = lua_gettop(L);
    lua_createtable(L, 0, static_cast<int>(event.fields.size()));
    const int table = lua_gettop(L);
    push_event_fields(L, table, event, raised.values);
    lua_pushvalue(L, handler);
    lua_pushvalue(L, table);
    lua_call(L, 1, 0);
    read_back(L, table, event, raised.values);
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

// The event's table class is event.<name>, and its handler takes one of them.
void write_event_definition(std::string &out, const ModEvent &event) {
    const std::string table_class = "event." + std::string(event.name);
    out += "\n";
    write_doc(out, event.doc);
    out.append("---@class ").append(table_class).append("\n");
    for (const ModEventField &field : event.fields) {
        out.append("---@field ").append(field.name).append(" ").append(field.type).append(" ");
        if (!is_writable(field))
            out += "(read-only) ";
        else if (field.type == "integer")
            out.append("(writable ").append(std::to_string(field.min)).append(" to ")
                .append(std::to_string(field.max)).append(") ");
        else
            out += "(writable) ";
        out.append(field.doc).append("\n");
    }
    out.append("---@type fun(e: ").append(table_class).append(")?\n").append(event.name).append(" = nil\n");
}

std::string make_definitions(const ModHostConfig &config) {
    std::string out = "---@meta\n-- The API a mod is given, version " + config.api_version +
                      ".\n-- Written by the game from its binding tables at start, for the Lua Language Server.\n"
                      "-- Do not edit; it is written again when it changes.\n";
    out += "\n---A table kept per mod between runs: numbers, booleans, strings, tables with string keys.\n"
           "---@type table<string, any>\nconfig = {}\n";
    out += "\n---A table kept per mod with each save slot: the same kinds of value as config.\n"
           "---@type table<string, any>\nsave = {}\n";
    for (const CallbackName &name : callback_names) {
        out += "\n";
        write_doc(out, name.doc);
        out.append("---@type fun()?\n").append(name.global).append(" = nil\n");
    }
    for (const ModEvent &event : config.events)
        write_event_definition(out, event);
    for (const ModTable &table : host_tables())
        write_table_definition(out, table);
    for (const ModTable &table : config.api)
        write_table_definition(out, table);
    if (!config.raw.path.empty())
        out += "\n" + raw_definitions(config.raw);
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
    const auto flag = [&](const char *key) { return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json.get(), key)) != 0; };
    mod.default_enabled = flag("default_enabled");
    mod.enhancement = flag("enhancement");
}

struct Fingerprint {
    // min, not {}: libstdc++'s file_clock counts from 2174, so every real time is below zero.
    fs::file_time_type stamp = fs::file_time_type::min();
    std::size_t count = 0;
    bool operator==(const Fingerprint &) const = default;
};

// Whether a change to the entry reloads the mod: mod.json, a .lua file, or anything under assets/.
bool counts_for_reload(const fs::path &dir, const fs::directory_entry &entry) {
    std::error_code error;
    if (entry.is_regular_file(error) && reloads_mod(entry.path()))
        return true;
    const fs::path inside = entry.path().lexically_relative(dir);
    return !inside.empty() && is_assets_name(*inside.begin());
}

// The newest write over the files that reload the mod, and how many: adding or deleting one counts.
Fingerprint fingerprint(const fs::path &dir) {
    Fingerprint print;
    std::error_code error;
    for (fs::recursive_directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
        if (!counts_for_reload(dir, *it))
            continue;
        print.stamp = std::max(print.stamp, it->last_write_time(error));
        ++print.count;
    }
    return print;
}

// Holds a raise's flag for the raise, so a throw cannot leave it set.
class RaiseScope {
public:
    explicit RaiseScope(bool &raising) : raising_(raising) { raising_ = true; }
    ~RaiseScope() { raising_ = false; }
    RaiseScope(const RaiseScope &) = delete;
    RaiseScope &operator=(const RaiseScope &) = delete;

private:
    bool &raising_;
};

bool is_mod_folder(const fs::directory_entry &entry) {
    std::error_code error;
    return entry.is_directory(error) &&
           (fs::exists(entry.path() / "mod.json", error) || fs::exists(entry.path() / "main.lua", error) ||
            fs::is_directory(entry.path() / "assets", error));
}

} // namespace

struct ModHost::State {
    ModHostConfig config;
    Clock::time_point started = Clock::now();
    std::vector<std::unique_ptr<Mod>> mods;
    SaveTables saves;
    bool raising = false;   // set while a raise runs: a raise inside it is ignored
    bool enhancements_held = false;

    bool held(const Mod &mod) const { return enhancements_held && mod.enhancement; }

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
        mod.assets.clear();   // the replacements go with the state: a reload, disable or failure drops them
        if (!mod.lua)
            return;
        if (mod.config_dirty)
            save_config(mod);
        // Finalizers run in lua_close within one budget; their errors are dropped.
        mod.instructions_left = static_cast<std::int64_t>(config.instructions_per_call);
        lua_sethook(mod.lua.get(), count_instructions, LUA_MASKCOUNT, hook_every_instructions);
        mod.lua.reset();
        mod.has_settings = false;
        if (config.unloaded)
            config.unloaded(mod.id);
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

    // Copies a running mod's save table into saves, so a reload or disable keeps it.
    void keep_save(Mod &mod) {
        if (!mod.lua)
            return;
        if (auto error = run_protected(mod, encode_save))
            print_line(mod.id, "cannot keep its save table: " + *error);
    }

    // A running mod's save table is set from saves; one that cannot take it fails.
    void reset_save(Mod &mod) {
        if (!mod.lua)
            return;
        if (auto error = run_protected(mod, apply_save))
            fail(mod, "cannot set its save table: " + *error);
    }

    // Calls the event in one running mod, with values as the mods before it left them. A mod that
    // errs is stopped and leaves values as they were.
    void raise_in(Mod &mod, const ModEvent &event, std::vector<ModEventValue> &values) {
        if (!mod.lua)
            return;
        mod.raised = RaisedEvent{&event, values};
        if (auto error = run_protected(mod, run_event)) {
            fail(mod, std::string(event.name) + ": " + *error);
            return;
        }
        if (mod.config_dirty)
            save_config(mod);
        values = std::move(mod.raised.values);
    }

    void stop(Mod &mod) {
        if (mod.lua && call(mod, ModCallback::Unload))
            keep_save(mod);
        unload(mod);
    }

    void load(Mod &mod) {
        stop(mod);
        mod.error.clear();
        const auto began = Clock::now();
        if (auto error = load_asset_folder(config, mod.dir, mod.assets)) {
            fail(mod, *error);
            return;
        }
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
        } else {
            mod.enabled = mod.default_enabled;
        }
    }

    // A folder seen for the first time: listed, and loaded if it was left enabled.
    void add(const fs::path &dir, const Fingerprint &print) {
        auto mod = std::make_unique<Mod>();
        mod->dir = dir;
        mod->host = &config;
        mod->saves = &saves;
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
        if (!state_->held(*mod))
            state_->call(*mod, callback);
}

void ModHost::call(std::string_view id, ModCallback callback) {
    if (Mod *mod = state_->find(id); mod && !state_->held(*mod))
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

void ModHost::hold_enhancements() { state_->enhancements_held = true; }

void ModHost::release_enhancements() { state_->enhancements_held = false; }

bool ModHost::set_config_value(std::string_view id, std::string_view key, const ModConfigValue &value) {
    Mod *mod = state_->find(id);
    const double *number = std::get_if<double>(&value);
    if (!mod || (number && !std::isfinite(*number)))
        return false;
    if (!mod->lua) {
        mod->config_json = with_config_value(mod->config_json, key, value);
        state_->store(*mod);
        return true;
    }
    mod->config_write.emplace(std::string(key), value);
    const auto error = run_protected(*mod, write_config_value);
    mod->config_write.reset();
    if (error) {
        print_line(mod->id, "cannot set config." + std::string(key) + ": " + *error);
        return false;
    }
    state_->save_config(*mod);
    return true;
}

std::optional<ModConfigValue> ModHost::config_value(std::string_view id, std::string_view key) const {
    Mod *mod = state_->find(id);
    if (!mod)
        return std::nullopt;
    if (mod->lua) {
        if (auto error = run_protected(*mod, encode_config))
            print_line(mod->id, "cannot read config: " + *error);
    }
    return config_value_in(mod->config_json, key);
}

void ModHost::raise(std::string_view event_name, std::span<ModEventValue> values) {
    State &state = *state_;
    if (state.raising)
        return;
    const ModEvent *event = find_event(state.config.events, event_name);
    if (!event)
        return;
    if (!values_fit(*event, values)) {
        assert(false && "raise: values do not match the event's fields");
        return;
    }
    std::vector<ModEventValue> current(values.begin(), values.end());
    {
        const RaiseScope scope(state.raising);
        for (auto &mod : state.mods)
            if (!state.held(*mod))
                state.raise_in(*mod, *event, current);
    }
    std::ranges::copy(current, values.begin());
}

std::string ModHost::save_tables() const {
    for (auto &mod : state_->mods)
        state_->keep_save(*mod);
    const Json all(cJSON_CreateObject());
    for (const auto &[id, text] : state_->saves) {
        Json table(cJSON_ParseWithLength(text.data(), text.size()));
        if (cJSON_IsObject(table.get()))
            cJSON_AddItemToObject(all.get(), id.c_str(), table.release());
    }
    const std::unique_ptr<char, JsonTextFree> printed(cJSON_PrintUnformatted(all.get()));
    return printed ? std::string(printed.get()) : std::string("{}");
}

void ModHost::load_save_tables(std::string_view json) {
    State &state = *state_;
    state.saves.clear();
    const Json parsed(cJSON_ParseWithLength(json.data(), json.size()));
    if (!cJSON_IsObject(parsed.get())) {
        std::printf("[mods] save tables are not a JSON object; starting empty\n");
    } else {
        for (const cJSON *child = parsed->child; child; child = child->next) {
            if (!cJSON_IsObject(child))
                continue;
            const std::unique_ptr<char, JsonTextFree> text(cJSON_PrintUnformatted(child));
            if (text)
                state.saves[child->string] = text.get();
        }
    }
    for (auto &mod : state.mods)
        state.reset_save(*mod);
}

std::vector<ModInfo> ModHost::mods() const {
    std::vector<ModInfo> list;
    list.reserve(state_->mods.size());
    for (const auto &mod : state_->mods) {
        const ModState shown = !mod->error.empty() ? ModState::Failed
                               : mod->lua          ? ModState::Running
                                                   : ModState::Disabled;
        list.push_back({mod->id, mod->name, mod->version, mod->author, mod->description, mod->enabled, shown,
                        mod->error, mod->has_settings, mod->enhancement,
                        shown == ModState::Running && state_->held(*mod)});
    }
    return list;
}

std::size_t ModHost::running_count() const {
    return static_cast<std::size_t>(
        std::count_if(state_->mods.begin(), state_->mods.end(),
                      [this](const auto &mod) { return mod->lua != nullptr && !state_->held(*mod); }));
}

std::shared_ptr<const std::vector<std::uint8_t>> ModHost::asset_override(std::string_view disc_path) const {
    AssetBytes replacement;
    for (const auto &mod : state_->mods) {
        if (!mod->lua || !mod->error.empty() || state_->held(*mod))
            continue;
        if (const auto found = mod->assets.find(disc_path); found != mod->assets.end())
            replacement = found->second;
    }
    return replacement;
}

std::string ModHost::definitions() const {
    return make_definitions(state_->config);
}

std::string_view mod_id(lua_State *L) {
    return mod_of_state(L).id;
}

std::optional<std::vector<std::uint8_t>> mod_file_bytes(lua_State *L, std::string_view relative, std::size_t max_bytes) {
    const auto path = inside_mod(mod_of_state(L), relative);
    if (!path)
        return std::nullopt;
    return read_bytes(*path, max_bytes);
}

void mod_config_changed(lua_State *L) {
    mod_of_state(L).config_dirty = true;
}

void mod_log(lua_State *L, std::string_view text) {
    print_line(mod_of_state(L).id, text);
}

} // namespace verdite
