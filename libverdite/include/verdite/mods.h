#ifndef VERDITE_MODS_H
#define VERDITE_MODS_H
// Lua 5.4 mods with live reload (docs/MODDING.md). A mod is a folder under the mods folder with
// mod.json and Lua sources; each runs in its own sandboxed lua_State with an instruction budget per
// call and a memory cap. Saving a file reloads it; an error stops only that mod. Game-agnostic: the
// game hands in its API as tables of functions and fields, from which the host also writes the
// editors' definitions file, and stores each mod's enabled flag and config where it keeps settings.
//
// The host gives every mod, beside the game's tables:
//   mod.id, mod.log(...), mod.time() (seconds since the host started), mod.save_config(),
//   mod.fs.read(path), mod.fs.write(path, text), mod.fs.exists(path), mod.fs.list([dir]),
//   paths relative to the mod's folder and never outside it;
//   config, a table kept per mod (numbers, booleans, strings, tables with string keys);
//   save, a table kept per mod with each save slot (the same kinds of value), given back by
//   load_save_tables and read by save_tables;
//   the events the game raises: a global handler of the event's name gets a table of its fields,
//   and may change the writable integers (ModHost::raise);
//   the standard library without io, os, package.loadlib, C modules, string.dump or bytecode, and
//   with debug.traceback only; require, dofile and loadfile reach only the mod's folder.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct lua_State;

namespace verdite {

using ModCFunction = int (*)(lua_State *);

// A function the game binds in one of its tables: kf.context, ui.text.
struct ModFunction {
    std::string_view name;
    std::string_view signature;   // for the definitions file: "fun(name: string): boolean"
    std::string_view doc;         // one line
    ModCFunction run = nullptr;
};

// A field of a table the game binds (kf.player.hp). get pushes the value; set reads the value at
// index and raises a Lua error if it is wrong. No set: read-only.
struct ModField {
    std::string_view name;
    std::string_view type;        // "integer", "number", "boolean", "string"
    std::string_view doc;
    void (*get)(lua_State *) = nullptr;
    void (*set)(lua_State *, int index) = nullptr;
};

// One table of the game's API, by its path from the globals: "kf", "kf.player", "ui".
struct ModTable {
    std::string_view path;
    std::string_view doc;
    std::vector<ModFunction> functions;
    std::vector<ModField> fields;
};

// The global functions a mod may define, which the host calls.
enum class ModCallback : std::uint8_t {
    Load,       // on_load: after the script runs, on every (re)load
    Update,     // on_update (or on_frame, its old name): once per game update
    Draw,       // on_draw: once per displayed frame
    Settings,   // on_settings: while the mod's settings are shown
    Unload,     // on_unload: before a reload, or when disabled
};

// A field of an event's table, as the definitions file lists it. A handler may change a
// writable integer field within [min, max]; a value outside it is an error that stops that mod,
// as kf.player writes are. Only integer fields are read back; writable on another type is ignored.
struct ModEventField {
    std::string_view name;
    std::string_view type;        // "integer", "boolean", "string"
    std::string_view doc;
    bool writable = false;
    std::int64_t min = 0, max = 0;
};

// An event the game raises: its handler is a global function of this name taking one table.
struct ModEvent {
    std::string_view name;        // "on_damage"
    std::string_view doc;
    std::vector<ModEventField> fields;
};

using ModEventValue = std::variant<std::int64_t, bool, std::string_view>;

// What the game stores for a mod between runs.
struct ModStored {
    bool enabled = false;
    std::string config_json = "{}";   // the config table as a JSON object
};

struct ModHostConfig {
    std::filesystem::path mods_dir;
    std::string api_version;                  // a mod's kf.api_version, say; also in the definitions
    std::vector<ModTable> api;
    std::vector<ModEvent> events;             // the events the game raises (ModHost::raise)
    std::string definitions_name = "api.d.lua";   // written into mods_dir from api and events
    std::size_t memory_cap_bytes = 64u << 20;     // per mod
    std::uint64_t instructions_per_call = 100'000'000;   // per callback, and for the script's first run
    std::function<std::optional<ModStored>(std::string_view id)> read_stored;
    std::function<void(std::string_view id, const ModStored &stored)> write_stored;
    // Called with a mod's id each time its Lua state is discarded: before a reload, when it is
    // disabled or removed, when it fails, and when the host is destroyed. The game drops what it
    // keeps for the mod.
    std::function<void(std::string_view id)> unloaded;
};

enum class ModState : std::uint8_t { Disabled, Running, Failed };

// A mod as the Mods panel lists it.
struct ModInfo {
    std::string id, name, version, author, description;
    bool enabled = false;         // switched on: a Failed mod stays on, and reloads when fixed
    ModState state = ModState::Disabled;
    std::string error;            // set when Failed: the message and stack
    bool has_settings = false;    // defines on_settings
};

class ModHost {
public:
    // Creates mods_dir if it is missing and writes the definitions file if it changed.
    explicit ModHost(ModHostConfig config);
    ~ModHost();   // unloads every mod (on_unload, config saved)
    ModHost(const ModHost &) = delete;
    ModHost &operator=(const ModHost &) = delete;

    // Finds new, changed and removed mod folders and (re)loads enabled ones. The game paces it
    // (twice a second); a new folder is listed disabled.
    void scan();
    // Calls the callback in every running mod, in the listed order. A mod that errs, goes over its
    // budget or its memory cap is stopped and Failed; the others go on.
    void call_each(ModCallback callback);
    // The same for one mod (Settings, from the panel).
    void call(std::string_view id, ModCallback callback);

    void enable(std::string_view id);
    void disable(std::string_view id);
    void reload_all();   // every enabled mod, as if its files changed

    // Calls the event's handler in every running mod that defines it, in the listed order, under
    // a callback's budget. values follow the event's fields; each mod sees what the one before it
    // left, and the final values are written back. A raise during another raise is ignored.
    void raise(std::string_view event, std::span<ModEventValue> values);

    // The current game's save tables (each mod's `save` global, stored with a save slot), as a
    // JSON object keyed by mod id. A mod not running keeps the entry it had, so disabling a mod
    // does not lose its part of the save.
    [[nodiscard]] std::string save_tables() const;
    // Replaces them; each running mod's `save` becomes a fresh table. "{}" for a new game. A mod
    // that (re)loads gets its table from here.
    void load_save_tables(std::string_view json);

    [[nodiscard]] std::vector<ModInfo> mods() const;   // sorted by name
    [[nodiscard]] std::size_t running_count() const;
    // The ---@meta definitions file for the API, as written into mods_dir.
    [[nodiscard]] std::string definitions() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// For the game's bound functions, while a callback of the mod that owns L runs:
std::string_view mod_id(lua_State *L);
void mod_config_changed(lua_State *L);   // config is saved when the callback returns
void mod_log(lua_State *L, std::string_view text);   // "[id] text" on stdout

} // namespace verdite

#endif // VERDITE_MODS_H
