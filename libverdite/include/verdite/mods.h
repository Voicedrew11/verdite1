#ifndef VERDITE_MODS_H
#define VERDITE_MODS_H
// Lua 5.4 mods with live reload (docs/MODDING.md). A mod is a folder under the mods folder with
// mod.json and Lua sources; each runs in its own sandboxed lua_State with an instruction budget per
// call and a memory cap. Saving a file reloads it; an error stops only that mod. A bound function may
// raise an event, so a handler can run inside its own mod's call: it spends that call's budget, and
// if it errs the mod stops as soon as the bound function returns. Game-agnostic: the game hands in
// its API as tables of functions and fields, from which the host also writes the editors'
// definitions file, and stores each mod's enabled flag and config where it keeps settings.
//
// mod.json may also say "default_enabled": true, so a mod never stored before starts enabled (a mod
// the game ships on), and "enhancement": true, for a mod the game holds while it plays as the
// original did (hold_enhancements).
//
// The host gives every mod, beside the game's tables:
//   mod.id, mod.log(...), mod.time() (seconds since the host started), mod.save_config(),
//   mod.fs.read(path), mod.fs.write(path, text), mod.fs.exists(path), mod.fs.list([dir]),
//   paths relative to the mod's folder and never outside it;
//   config, a table kept per mod (numbers, booleans, strings, tables with string keys);
//   save, a table kept per mod with each save slot (the same kinds of value), given back by
//   load_save_tables and read by save_tables;
//   the events the game raises: a global handler of the event's name gets a table of its fields,
//   and may change the writable integers and booleans (ModHost::raise);
//   mod.assets.set(disc_path, file) and mod.assets.clear(disc_path): the disc file disc_path is
//   replaced by the mod's file, or no longer is, the next time the game loads it;
//   the standard library without io, os, package.loadlib, C modules, string.dump or bytecode, and
//   with debug.traceback only; require, dofile and loadfile reach only the mod's folder.
//
// A mod's assets/ folder holds replacements for disc files: assets/KF/B0/SND0.SEQ replaces the disc
// file KF/B0/SND0.SEQ. Each file is checked when the mod loads, and a bad one fails the whole mod.
// A mod may have assets/ and no main.lua.
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

#include "verdite/raw.h"
#include "verdite/raw_call.h"
#include "verdite/raw_hook.h"

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
    // on_between_updates: between two world updates, where no menu or script holds the game. Calls
    // into the game that change state run here (verdite/raw_call.h).
    BetweenUpdates,
};

// A field of an event's table, as the definitions file lists it. A handler may change a
// writable integer field within [min, max], and a writable boolean field to a boolean; any other
// value is an error that stops that mod, as kf.player writes are. Only integer and boolean fields
// are read back; writable on a string is ignored.
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

// A value of a mod's config as the game's own settings set it (verdite/options.h's OptionValue).
using ModConfigValue = std::variant<bool, double, std::string>;

// What the game stores for a mod between runs.
struct ModStored {
    bool enabled = false;
    std::string config_json = "{}";   // the config table as a JSON object
};

// A disc file kind a mod may replace; a new kind is a new row. A file matches the first row whose
// suffix its disc path ends with.
struct ModAssetKind {
    std::string_view suffix;   // ".TIM"
    // Nothing if the game can load these bytes, else why not.
    std::optional<std::string> (*check)(std::span<const std::uint8_t> bytes);
};

struct ModHostConfig {
    std::filesystem::path mods_dir;
    std::string api_version;                  // a mod's kf.api_version, say; also in the definitions
    std::vector<ModTable> api;
    std::vector<ModEvent> events;             // the events the game raises (ModHost::raise)
    std::vector<ModAssetKind> asset_kinds;    // the disc files a mod may replace (its assets/ folder)
    // The size of the disc's file at a disc path ("KF/B0/SND0.SEQ"); nothing if the disc has none.
    // Without it, no file is on the disc.
    std::function<std::optional<std::size_t>(std::string_view path)> disc_file_size;
    RawLayer raw;                             // the raw layer, hung at raw.path; an empty path: none
    RawCallLayer calls;                       // the call layer, hung at calls.path; an empty path: none
    RawHookLayer hooks;                       // the hook layer, hung at hooks.path; an empty path: none
    std::string definitions_name = "api.d.lua";   // written into mods_dir from api and events
    std::size_t memory_cap_bytes = 64u << 20;     // per mod
    std::uint64_t instructions_per_call = 100'000'000;   // per callback, and for the script's first run
    // Nothing for a mod never stored: it starts as its mod.json's default_enabled says.
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
    bool enhancement = false;     // mod.json "enhancement"
    bool held = false;            // Running, but held: an enhancement while hold_enhancements lasts
};

class ModHost {
public:
    // Creates mods_dir if it is missing and writes the definitions file if it changed. Answers the hook
    // layer from here on, on this thread only.
    explicit ModHost(ModHostConfig config);
    ~ModHost();   // unloads every mod (on_unload, config saved)
    ModHost(const ModHost &) = delete;
    ModHost &operator=(const ModHost &) = delete;

    // Finds new, changed and removed mod folders and (re)loads enabled ones. The game paces it
    // (twice a second); a new folder is listed disabled. Does nothing during a call that runs frames.
    void scan();
    // Calls the callback in every running mod, in the listed order. A mod that errs, goes over its
    // budget or its memory cap is stopped and Failed; the others go on.
    void call_each(ModCallback callback);
    // The same for one mod (Settings, from the panel).
    void call(std::string_view id, ModCallback callback);
    // The game presented a frame. A mod inside a call that runs frames (verdite/raw_call.h) gets a
    // whole budget for its next ones, and each mod's hook handlers (verdite/raw_hook.h), which share
    // one budget a frame, get theirs back.
    void frame_passed();

    // During a call that runs frames these wait for it to end, as a mod's state is in use.
    void enable(std::string_view id);
    void disable(std::string_view id);
    void reload_all();   // every enabled mod, as if its files changed

    // Until release_enhancements, an enhancement stays loaded but gets no callback (on_load and
    // on_unload still run, so a reload or a disable works) and no event: the game's classic mode.
    void hold_enhancements();
    void release_enhancements();

    // Sets a mod's config value by key, as the mod's own `config.key = value` would, and stores
    // it; a mod not running gets it when it next loads. False for an unknown mod, a number that is
    // not finite, or a mod over its budget.
    bool set_config_value(std::string_view id, std::string_view key, const ModConfigValue &value);
    // Nothing for an unknown mod, or a key missing or not a boolean, number or string.
    [[nodiscard]] std::optional<ModConfigValue> config_value(std::string_view id, std::string_view key) const;

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

    // The replacement for a disc file, from the last running mod in the listed order (the one
    // call_each reaches last), or nothing to use the disc's. Held and failed mods replace nothing.
    // The bytes are held, so a file changed on disk after its load changes nothing until a rescan.
    [[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> asset_override(std::string_view disc_path) const;

    [[nodiscard]] std::vector<ModInfo> mods() const;   // sorted by name
    [[nodiscard]] std::size_t running_count() const;   // not counting held mods
    // The ---@meta definitions file for the API, as written into mods_dir.
    [[nodiscard]] std::string definitions() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// For the game's bound functions, while a callback of the mod that owns L runs:
std::string_view mod_id(lua_State *L);
// The mod's file at a path relative to its folder, resolved as mod.fs resolves it: nothing if it is
// missing, not a file, outside the folder, unreadable, or larger than max_bytes.
std::optional<std::vector<std::uint8_t>> mod_file_bytes(lua_State *L, std::string_view relative, std::size_t max_bytes);
void mod_config_changed(lua_State *L);   // config is saved when the callback returns
void mod_log(lua_State *L, std::string_view text);   // "[id] text" on stdout
// Charges native work to the running callback's budget, which otherwise counts only Lua
// instructions. Over the budget it raises a Lua error, as the instruction count does.
void mod_spend_instructions(lua_State *L, std::int64_t instructions);

} // namespace verdite

#endif // VERDITE_MODS_H
