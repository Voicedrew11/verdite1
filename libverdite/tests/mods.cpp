#include <verdite/mods.h>

#include <doctest/doctest.h>

#include <lauxlib.h>
#include <lua.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace verdite;
namespace fs = std::filesystem;

namespace {

// What the test table's functions saw, reset by each Mods.
std::vector<std::string> notes;
lua_Integer value;

int note(lua_State *L) {
    notes.emplace_back(luaL_checkstring(L, 1));
    return 0;
}
int changed(lua_State *L) {
    mod_config_changed(L);
    return 0;
}
void get_value(lua_State *L) { lua_pushinteger(L, value); }
void set_value(lua_State *L, int index) { value = luaL_checkinteger(L, index); }
void get_fixed(lua_State *L) { lua_pushinteger(L, 7); }
// The size of a file the mod may read, up to 16 bytes, by mod_file_bytes; nil for none.
int file_size(lua_State *L) {
    const char *relative = luaL_checkstring(L, 1);
    const auto bytes = mod_file_bytes(L, relative, 16);
    if (!bytes) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(bytes->size()));
    return 1;
}

// The host the nested-raise test raises through, from inside a handler.
ModHost *nested_host = nullptr;
int again(lua_State *) {
    std::array<ModEventValue, 5> values{std::int64_t{1}, std::int64_t{2}, false, std::string_view("n"), false};
    nested_host->raise("on_test", values);
    return 0;
}
int again_twice(lua_State *L) {
    again(L);
    return again(L);
}
int spend(lua_State *L) {
    mod_spend_instructions(L, 100'000);
    return 0;
}

// The test game's replaceable files: a .TIM must begin with TIM, a .SEQ may hold anything.
std::optional<std::string> check_tim(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 3 || bytes[0] != 'T' || bytes[1] != 'I' || bytes[2] != 'M')
        return "is not a TIM: it must begin with TIM";
    return std::nullopt;
}
std::optional<std::string> check_anything(std::span<const std::uint8_t>) { return std::nullopt; }

const std::vector<ModAssetKind> &test_kinds() {
    static const std::vector<ModAssetKind> kinds = {{.suffix = ".TIM", .check = check_tim},
                                                    {.suffix = ".SEQ", .check = check_anything}};
    return kinds;
}

std::string text_of(const std::shared_ptr<const std::vector<std::uint8_t>> &bytes) {
    return bytes ? std::string(bytes->begin(), bytes->end()) : std::string("(none)");
}

// A mods folder of its own, and a host over it whose stored settings are a map.
struct Mods {
    fs::path dir;
    std::map<std::string, ModStored, std::less<>> stored;
    std::map<std::string, std::size_t, std::less<>> disc;   // the disc's files, by disc path
    std::unique_ptr<ModHost> host;
    int writes = 0;
    std::vector<std::string> unloaded_ids;   // each discard of a mod's Lua state, in order

    explicit Mods(std::size_t memory_cap_bytes = 64u << 20, std::uint64_t instructions = 1'000'000,
                  std::vector<ModAssetKind> kinds = test_kinds()) {
        static int count;
        dir = fs::temp_directory_path() /
              ("verdite-mods-" + std::to_string(::getpid()) + "-" + std::to_string(++count));
        fs::remove_all(dir);
        notes.clear();
        value = 0;
        ModHostConfig config;
        config.mods_dir = dir;
        config.api_version = "9.9";
        config.memory_cap_bytes = memory_cap_bytes;
        config.instructions_per_call = instructions;
        config.api = {{.path = "t",
                       .doc = "The test's table.",
                       .functions = {{"note", "fun(text: string)", "Notes text.", note},
                                     {"changed", "fun()", "Marks config changed.", changed},
                                     {"again", "fun()", "Raises on_test from inside a handler.", again},
                                     {"again_twice", "fun()", "Raises on_test twice.", again_twice},
                                     {"spend", "fun()", "Spends 100,000 of the budget.", spend},
                                     {"size", "fun(path: string): integer?", "The size of a file, up to 16 bytes.", file_size}},
                       .fields = {{"value", "integer", "A number.", get_value, set_value},
                                  {"fixed", "integer", "Seven.", get_fixed, nullptr}}}};
        config.events = {{.name = "on_test",
                          .doc = "A test event.",
                          .fields = {{"amount", "integer", "Changeable from 0 to 100.", true, 0, 100},
                                     {"fixed", "integer", "Seven, not changeable.", false, 0, 0},
                                     {"flag", "boolean", "A flag."},
                                     {"label", "string", "A label."},
                                     {"cancel", "boolean", "Changeable.", true}}}};
        config.read_stored = [this](std::string_view id) -> std::optional<ModStored> {
            const auto found = stored.find(id);
            if (found == stored.end())
                return std::nullopt;
            return found->second;
        };
        config.write_stored = [this](std::string_view id, const ModStored &now) {
            stored[std::string(id)] = now;
            ++writes;
        };
        config.unloaded = [this](std::string_view id) { unloaded_ids.emplace_back(id); };
        config.asset_kinds = std::move(kinds);
        config.disc_file_size = [this](std::string_view path) -> std::optional<std::size_t> {
            const auto found = disc.find(path);
            if (found == disc.end())
                return std::nullopt;
            return found->second;
        };
        host = std::make_unique<ModHost>(std::move(config));
    }
    ~Mods() {
        host.reset();
        std::error_code error;
        fs::remove_all(dir, error);
    }
    Mods(const Mods &) = delete;
    Mods &operator=(const Mods &) = delete;

    // Writes a file, dated later than anything before it so a scan sees the change.
    void write(const std::string &relative, const std::string &text) {
        static int seconds;
        const fs::path path = dir / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        fs::last_write_time(path, fs::file_time_type::clock::now() + std::chrono::seconds(++seconds));
    }

    // A mod named id with main.lua holding script, found and enabled.
    void run(const std::string &id, const std::string &script) {
        write(id + "/main.lua", script);
        host->scan();
        host->enable(id);
    }

    [[nodiscard]] ModInfo info(std::string_view id) const {
        const auto list = host->mods();
        const auto found = std::find_if(list.begin(), list.end(), [&](const ModInfo &mod) { return mod.id == id; });
        REQUIRE(found != list.end());
        return *found;
    }
};

bool noted(std::string_view text) {
    return std::find(notes.begin(), notes.end(), text) != notes.end();
}

// The test event's values as the game raises it: amount 50, fixed 7, flag true, label "hi",
// cancel false.
std::array<ModEventValue, 5> event_values() {
    return {std::int64_t{50}, std::int64_t{7}, true, std::string_view("hi"), false};
}

} // namespace

TEST_CASE("a new mod is listed disabled, and runs its script and on_load when enabled") {
    Mods mods;
    mods.write("a/main.lua", "t.note('script') function on_load() t.note('load') end");
    mods.host->scan();
    CHECK(mods.info("a").state == ModState::Disabled);
    CHECK(notes.empty());
    mods.host->enable("a");
    CHECK(mods.info("a").state == ModState::Running);
    CHECK(notes == std::vector<std::string>{"script", "load"});
    CHECK(mods.stored.at("a").enabled);
    CHECK(mods.host->running_count() == 1);
}

TEST_CASE("mod.json names the mod and its entry script") {
    Mods mods;
    mods.write("folder/mod.json", R"({"id": "named", "name": "Named", "version": "2", "entry": "start.lua"})");
    mods.write("folder/start.lua", "t.note('start')");
    mods.host->scan();
    const ModInfo info = mods.info("named");
    CHECK(info.name == "Named");
    CHECK(info.version == "2");
    mods.host->enable("named");
    CHECK(noted("start"));
}

TEST_CASE("on_update runs each update, and on_frame is its old name") {
    Mods mods;
    mods.run("new", "function on_update() t.note('update') end");
    mods.run("old", "function on_frame() t.note('frame') end");
    mods.host->call_each(ModCallback::Update);
    CHECK(noted("update"));
    CHECK(noted("frame"));
}

TEST_CASE("a changed file reloads the mod: on_unload, then the script again") {
    Mods mods;
    mods.run("a", "function on_unload() t.note('unload') end");
    notes.clear();
    mods.write("a/main.lua", "t.note('second')");
    mods.host->scan();
    CHECK(notes == std::vector<std::string>{"unload", "second"});
}

TEST_CASE("an error stops only that mod, and fixing the file starts it again") {
    Mods mods;
    mods.run("bad", "function on_update() error('broken') end");
    mods.run("good", "function on_update() t.note('good') end");
    mods.host->call_each(ModCallback::Update);
    const ModInfo bad = mods.info("bad");
    CHECK(bad.state == ModState::Failed);
    CHECK(bad.error.find("broken") != std::string::npos);
    CHECK(bad.error.find("on_update") != std::string::npos);
    CHECK(noted("good"));
    mods.write("bad/main.lua", "function on_update() end");
    mods.host->scan();
    CHECK(mods.info("bad").state == ModState::Running);
}

TEST_CASE("a removed folder is unloaded and no longer listed") {
    Mods mods;
    mods.run("a", "function on_unload() t.note('unload') end");
    fs::remove_all(mods.dir / "a");
    mods.host->scan();
    CHECK(noted("unload"));
    CHECK(mods.host->mods().empty());
}

TEST_CASE("a second mod with the same id is listed failed and never runs") {
    Mods mods;
    mods.write("one/mod.json", R"({"id": "same"})");
    mods.write("one/main.lua", "t.note('one')");
    mods.write("two/mod.json", R"({"id": "same"})");
    mods.write("two/main.lua", "t.note('two')");
    mods.host->scan();
    mods.host->enable("same");
    CHECK(noted("one"));
    CHECK_FALSE(noted("two"));
    const auto list = mods.host->mods();
    CHECK(std::count_if(list.begin(), list.end(), [](const ModInfo &mod) { return mod.state == ModState::Failed; }) == 1);
}

TEST_CASE("the sandbox has no io, os, C modules, bytecode or debug beyond traceback") {
    Mods mods;
    mods.run("a", R"(
        t.note(tostring(io) .. tostring(os) .. tostring(package))
        t.note(tostring(string.dump) .. tostring(debug.getinfo) .. tostring(debug.getregistry))
        t.note(type(debug.traceback))
        local f, err = load("\27Lua")
        t.note(tostring(f))
        t.note(tostring(load("return 1 + 1")()))
        t.note(tostring(pcall(require, "..x")))
        t.note(tostring(pcall(dofile, "/etc/passwd")))
        t.note(tostring(loadfile("../outside.lua")))
    )");
    CHECK(mods.info("a").state == ModState::Running);
    CHECK(notes == std::vector<std::string>{"nilnilnil", "nilnilnil", "function", "nil", "2", "false", "false", "nil"});
}

TEST_CASE("require loads a module from the mod's folder once") {
    Mods mods;
    mods.write("a/lib/util.lua", "t.note('loaded') return { twice = function(n) return n * 2 end }");
    mods.run("a", "local u = require('lib.util') require('lib.util') t.note(tostring(u.twice(4)))");
    CHECK(notes == std::vector<std::string>{"loaded", "8"});
}

TEST_CASE("a callback over its instruction budget stops the mod, even if it catches the error") {
    Mods mods;
    mods.run("loop", "function on_update() while true do end end");
    mods.run("caught", "function on_update() while true do pcall(function() while true do end end) end end");
    mods.host->call_each(ModCallback::Update);
    CHECK(mods.info("loop").state == ModState::Failed);
    CHECK(mods.info("loop").error.find("instruction budget") != std::string::npos);
    CHECK(mods.info("caught").state == ModState::Failed);
}

TEST_CASE("a mod over its memory cap is stopped, even if it catches the error") {
    Mods mods(1u << 20, 100'000'000);
    mods.run("big", "function on_update() local t = {} for i = 1, 1e7 do t[i] = i end end");
    mods.run("caught", "function on_update() pcall(function() local s = string.rep('x', 4 << 20) end) end");
    mods.host->call_each(ModCallback::Update);
    CHECK(mods.info("big").state == ModState::Failed);
    CHECK(mods.info("big").error.find("memory cap") != std::string::npos);
    CHECK(mods.info("caught").state == ModState::Failed);
}

TEST_CASE("mod.fs reads and writes inside the mod's folder only") {
    Mods mods;
    mods.write("a/data/seed.txt", "seed");
    mods.run("a", R"(
        t.note(mod.fs.read("data/seed.txt"))
        t.note(tostring(mod.fs.write("out/saved.txt", "kept")))
        t.note(mod.fs.read("out/saved.txt"))
        t.note(tostring(mod.fs.exists("out")) .. tostring(mod.fs.exists("nope")))
        t.note(table.concat(mod.fs.list(), ","))
        t.note(tostring(mod.fs.read("../b/secret.txt")))
        t.note(tostring(mod.fs.read("/etc/passwd")))
        t.note(tostring(mod.fs.write("other.lua", "x")))
        t.note(tostring(mod.fs.write("mod.json", "x")))
    )");
    CHECK(notes == std::vector<std::string>{"seed", "true", "kept", "truefalse", "data/,main.lua,out/", "nil", "nil",
                                            "nil", "nil"});
    CHECK_FALSE(fs::exists(mods.dir / "a" / "other.lua"));
}

TEST_CASE("mod.fs does not follow a link out of the folder") {
    Mods mods;
    mods.write("outside/secret.txt", "secret");
    fs::create_directories(mods.dir / "a");
    fs::create_directory_symlink(mods.dir / "outside", mods.dir / "a" / "link");
    mods.run("a", "t.note(tostring(mod.fs.read('link/secret.txt')))");
    CHECK(notes == std::vector<std::string>{"nil"});
}

TEST_CASE("config comes from the stored settings and is stored when changed") {
    Mods mods;
    mods.stored["a"] = {.enabled = true, .config_json = R"({"speed": 2, "name": "x", "deep": {"on": true}})"};
    mods.write("a/main.lua", R"(
        t.note(tostring(config.speed) .. config.name .. tostring(config.deep.on))
        function on_update() config.speed = 3 t.changed() end
    )");
    mods.host->scan();
    CHECK(noted("2xtrue"));
    mods.host->call_each(ModCallback::Update);
    const std::string &json = mods.stored.at("a").config_json;
    CHECK(json.find("\"speed\":3") != std::string::npos);
    CHECK(json.find("\"deep\":{\"on\":true}") != std::string::npos);
    CHECK(mods.stored.at("a").enabled);
}

TEST_CASE("mod.save_config stores config when the callback returns, and disabling keeps it") {
    Mods mods;
    mods.run("a", "function on_update() config.n = 5 mod.save_config() end");
    mods.host->call_each(ModCallback::Update);
    CHECK(mods.stored.at("a").config_json == R"({"n":5})");
    mods.host->disable("a");
    CHECK_FALSE(mods.stored.at("a").enabled);
    CHECK(mods.stored.at("a").config_json == R"({"n":5})");
    CHECK(mods.info("a").state == ModState::Disabled);
}

TEST_CASE("a bound table's fields are read, written, and refused by name") {
    Mods mods;
    mods.run("a", R"(
        t.value = 41
        t.note(tostring(t.value + 1) .. tostring(t.fixed))
        t.note(tostring(pcall(function() t.fixed = 1 end)))
        t.note(tostring(pcall(function() return t.missing end)))
        t.note(mod.id .. mod.api_version)
    )");
    CHECK(value == 41);
    CHECK(notes == std::vector<std::string>{"427", "false", "false", "a9.9"});
}

TEST_CASE("the definitions file lists the callbacks and every table, and is written into the folder") {
    Mods mods;
    const std::string text = mods.host->definitions();
    CHECK(text.rfind("---@meta", 0) == 0);
    CHECK(text.find("on_update = nil") != std::string::npos);
    CHECK(text.find("---@class mod.fs") != std::string::npos);
    CHECK(text.find("---@field note fun(text: string) Notes text.") != std::string::npos);
    CHECK(text.find("---@field fixed integer (read-only) Seven.") != std::string::npos);
    CHECK(text.find("version 9.9") != std::string::npos);
    std::ifstream file(mods.dir / "api.d.lua", std::ios::binary);
    const std::string written((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK(written == text);
}

TEST_CASE("print writes the mod's id, and a mod that never loads reports its error") {
    Mods mods;
    mods.run("a", "print('hello')");
    CHECK(mods.info("a").state == ModState::Running);
    mods.run("syntax", "this is not lua");
    CHECK(mods.info("syntax").state == ModState::Failed);
    CHECK_FALSE(mods.info("syntax").error.empty());
}

TEST_CASE("disabling a mod reports its state discarded once") {
    Mods mods;
    mods.run("a", "");
    CHECK(mods.unloaded_ids.empty());
    mods.host->disable("a");
    CHECK(mods.unloaded_ids == std::vector<std::string>{"a"});
}

TEST_CASE("a live reload reports the discarded state once, not once per load") {
    Mods mods;
    mods.run("a", "");
    mods.write("a/main.lua", "t.note('second')");
    mods.host->scan();
    CHECK(noted("second"));
    CHECK(mods.unloaded_ids == std::vector<std::string>{"a"});
}

TEST_CASE("a mod that errs reports its state discarded once, and disabling it later reports nothing") {
    Mods mods;
    mods.run("bad", "function on_update() error('broken') end");
    mods.host->call_each(ModCallback::Update);
    CHECK(mods.info("bad").state == ModState::Failed);
    CHECK(mods.unloaded_ids == std::vector<std::string>{"bad"});
    mods.host->disable("bad");
    CHECK(mods.unloaded_ids == std::vector<std::string>{"bad"});
}

TEST_CASE("destroying the host reports each running mod's state discarded") {
    Mods mods;
    mods.run("a", "");
    mods.run("b", "");
    mods.host.reset();
    CHECK(std::count(mods.unloaded_ids.begin(), mods.unloaded_ids.end(), std::string("a")) == 1);
    CHECK(std::count(mods.unloaded_ids.begin(), mods.unloaded_ids.end(), std::string("b")) == 1);
    CHECK(mods.unloaded_ids.size() == 2);
}

TEST_CASE("an event reaches the handler with its fields, and an unknown event does nothing") {
    Mods mods;
    mods.run("a", "function on_test(e) t.note(e.amount .. ',' .. e.fixed .. ',' .. tostring(e.flag) .. ',' .. e.label) end");
    auto values = event_values();
    mods.host->raise("on_test", values);
    CHECK(notes == std::vector<std::string>{"50,7,true,hi"});
    mods.host->raise("on_missing", values);
    CHECK(notes.size() == 1);
}

TEST_CASE("a writable field changed by one mod is seen by the next and written back") {
    Mods mods;
    mods.run("a", "function on_test(e) e.amount = e.amount - 10 end");
    mods.run("b", "function on_test(e) t.note(tostring(e.amount)) e.amount = e.amount * 2 end");
    auto values = event_values();
    mods.host->raise("on_test", values);
    CHECK(notes == std::vector<std::string>{"40"});
    CHECK(std::get<std::int64_t>(values[0]) == 80);
}

TEST_CASE("a writable value out of range or of the wrong type fails that mod, and the next sees the value before") {
    Mods mods;
    mods.run("a", "function on_test(e) e.amount = 101 end");
    mods.run("b", "function on_test(e) e.amount = 'text' end");
    mods.run("c", "function on_test(e) t.note(tostring(e.amount)) e.amount = e.amount + 1 end");
    auto values = event_values();
    mods.host->raise("on_test", values);
    CHECK(mods.info("a").state == ModState::Failed);
    CHECK(mods.info("a").error.find("on_test") != std::string::npos);
    CHECK(mods.info("a").error.find("'amount'") != std::string::npos);
    CHECK(mods.info("b").state == ModState::Failed);
    CHECK(mods.info("b").error.find("'amount'") != std::string::npos);
    CHECK(notes == std::vector<std::string>{"50"});
    CHECK(std::get<std::int64_t>(values[0]) == 51);
}

TEST_CASE("a writable boolean changed by one mod is seen by the next and written back") {
    Mods mods;
    mods.run("a", "function on_test(e) e.cancel = true end");
    mods.run("b", "function on_test(e) t.note(tostring(e.cancel)) end");
    auto values = event_values();
    mods.host->raise("on_test", values);
    CHECK(notes == std::vector<std::string>{"true"});
    CHECK(std::get<bool>(values[4]));
}

TEST_CASE("a writable boolean left as another type fails that mod, and the next sees the value before") {
    Mods mods;
    mods.run("a", "function on_test(e) e.cancel = 1 end");
    mods.run("b", "function on_test(e) e.cancel = nil end");
    mods.run("c", "function on_test(e) t.note(tostring(e.cancel)) end");
    auto values = event_values();
    mods.host->raise("on_test", values);
    CHECK(mods.info("a").state == ModState::Failed);
    CHECK(mods.info("a").error.find("'cancel' must be a boolean") != std::string::npos);
    CHECK(mods.info("b").state == ModState::Failed);
    CHECK(notes == std::vector<std::string>{"false"});
    CHECK_FALSE(std::get<bool>(values[4]));
}

TEST_CASE("a field that is not writable is not read back") {
    Mods mods;
    mods.run("a", "function on_test(e) e.fixed = 99 e.flag = false e.label = 'changed' end");
    mods.run("b", "function on_test(e) t.note(tostring(e.fixed) .. tostring(e.flag) .. e.label) end");
    auto values = event_values();
    mods.host->raise("on_test", values);
    CHECK(mods.info("a").state == ModState::Running);
    CHECK(notes == std::vector<std::string>{"7truehi"});
    CHECK(std::get<std::int64_t>(values[1]) == 7);
    CHECK(std::get<bool>(values[2]));
    CHECK(std::get<std::string_view>(values[3]) == "hi");
}

TEST_CASE("a mod without the handler is skipped") {
    Mods mods;
    mods.run("a", "t.note('loaded')");
    mods.run("b", "function on_test(e) t.note('b') end");
    auto values = event_values();
    mods.host->raise("on_test", values);
    CHECK(mods.info("a").state == ModState::Running);
    CHECK(notes == std::vector<std::string>{"loaded", "b"});
}

TEST_CASE("a handler error fails only that mod") {
    Mods mods;
    mods.run("bad", "function on_test(e) error('boom') end");
    mods.run("good", "function on_test(e) t.note('good') end");
    auto values = event_values();
    mods.host->raise("on_test", values);
    CHECK(mods.info("bad").state == ModState::Failed);
    CHECK(mods.info("bad").error.find("boom") != std::string::npos);
    CHECK(mods.info("bad").error.find("on_test") != std::string::npos);
    CHECK(noted("good"));
    CHECK(mods.info("good").state == ModState::Running);
}

TEST_CASE("an endless handler hits the budget and fails only that mod") {
    Mods mods;
    mods.run("loop", "function on_test(e) while true do end end");
    mods.run("good", "function on_test(e) t.note('good') end");
    auto values = event_values();
    mods.host->raise("on_test", values);
    CHECK(mods.info("loop").state == ModState::Failed);
    CHECK(mods.info("loop").error.find("instruction budget") != std::string::npos);
    CHECK(noted("good"));
}

TEST_CASE("a raise inside a handler is ignored, and a later raise runs again") {
    Mods mods;
    nested_host = mods.host.get();
    mods.run("a", "function on_test(e) t.note('outer') t.again() end");
    auto values = event_values();
    mods.host->raise("on_test", values);
    CHECK(notes == std::vector<std::string>{"outer"});
    CHECK(mods.info("a").state == ModState::Running);
    mods.host->raise("on_test", values);
    CHECK(notes == std::vector<std::string>{"outer", "outer"});
    nested_host = nullptr;
}

// A bound function that runs game code raising an event, as a call into the game will: the handler
// runs on the state the calling mod is already using.
TEST_CASE("a handler that errs inside its own mod's call stops the mod when the call returns") {
    Mods mods;
    nested_host = mods.host.get();
    mods.run("a", "function on_test(e) error('boom') end\n"
                  "function on_update() t.again() t.note('after') end");
    mods.host->call_each(ModCallback::Update);
    CHECK_FALSE(noted("after"));
    CHECK(mods.info("a").state == ModState::Failed);
    CHECK(mods.info("a").error.find("boom") != std::string::npos);
    CHECK(mods.info("a").error.find("on_test") != std::string::npos);
    CHECK(mods.info("a").error.find("on_update") != std::string::npos);
    CHECK(mods.unloaded_ids == std::vector<std::string>{"a"});
    nested_host = nullptr;
}

TEST_CASE("a mod cannot catch a failure inside its own call and go on") {
    Mods mods;
    nested_host = mods.host.get();
    mods.run("a", "function on_test(e) error('boom') end\n"
                  "function on_update() pcall(t.again) t.note('after') end");
    mods.host->call_each(ModCallback::Update);
    CHECK_FALSE(noted("after"));
    CHECK(mods.info("a").state == ModState::Failed);
    CHECK(mods.info("a").error.find("boom") != std::string::npos);
    nested_host = nullptr;
}

TEST_CASE("a second event in the same call does not run in a mod already stopping") {
    Mods mods;
    nested_host = mods.host.get();
    mods.run("a", "function on_test(e) t.note('handler') error('boom') end\n"
                  "function on_update() t.again_twice() end");
    mods.host->call_each(ModCallback::Update);
    CHECK(notes == std::vector<std::string>{"handler"});
    CHECK(mods.info("a").state == ModState::Failed);
    nested_host = nullptr;
}

TEST_CASE("another mod's handler that errs inside a call stops that mod, and the caller goes on") {
    Mods mods;
    nested_host = mods.host.get();
    mods.run("a", "function on_update() t.again() t.note('after') end");
    mods.run("b", "function on_test(e) error('boom') end");
    mods.host->call_each(ModCallback::Update);
    CHECK(noted("after"));
    CHECK(mods.info("a").state == ModState::Running);
    CHECK(mods.info("b").state == ModState::Failed);
    nested_host = nullptr;
}

TEST_CASE("a mod stopped inside a call reloads when fixed") {
    Mods mods;
    nested_host = mods.host.get();
    mods.run("a", "function on_test(e) error('boom') end\nfunction on_update() t.again() end");
    mods.host->call_each(ModCallback::Update);
    REQUIRE(mods.info("a").state == ModState::Failed);
    mods.write("a/main.lua", "function on_update() t.note('fixed') end");
    mods.host->scan();
    mods.host->call_each(ModCallback::Update);
    CHECK(mods.info("a").state == ModState::Running);
    CHECK(noted("fixed"));
    nested_host = nullptr;
}

TEST_CASE("events raised inside a call do not refill the calling mod's budget") {
    Mods mods;
    nested_host = mods.host.get();
    mods.run("a", "function on_test(e) end\nfunction on_update() while true do t.again() end end");
    mods.host->call_each(ModCallback::Update);
    CHECK(mods.info("a").state == ModState::Failed);
    CHECK(mods.info("a").error.find("instruction budget") != std::string::npos);
    nested_host = nullptr;
}

TEST_CASE("a handler inside a call spends the caller's budget, and its overrun stops the mod") {
    Mods mods;
    nested_host = mods.host.get();
    mods.run("a", "function on_test(e) while true do end end\nfunction on_update() t.again() t.note('after') end");
    mods.host->call_each(ModCallback::Update);
    CHECK_FALSE(noted("after"));
    CHECK(mods.info("a").state == ModState::Failed);
    CHECK(mods.info("a").error.find("instruction budget") != std::string::npos);
}

TEST_CASE("mod_spend_instructions spends the budget of the running callback") {
    Mods mods;   // a budget of 1,000,000: t.spend() costs 100,000
    mods.run("a", "function on_update() while true do t.value = t.value + 1 pcall(t.spend) end end");
    mods.host->call_each(ModCallback::Update);
    CHECK(mods.info("a").state == ModState::Failed);
    CHECK(mods.info("a").error.find("instruction budget") != std::string::npos);
    CHECK(value <= 10);
    CHECK(value >= 9);
}

TEST_CASE("a budget spent by one callback is whole for the next") {
    Mods mods;
    mods.run("a", "function on_update() t.spend() t.spend() end");
    for (int i = 0; i < 20; ++i)
        mods.host->call_each(ModCallback::Update);
    CHECK(mods.info("a").state == ModState::Running);
}

TEST_CASE("save survives a reload of the mod") {
    Mods mods;
    mods.run("a", "t.note('start:' .. tostring(save.n or 0)) function on_update() save.n = (save.n or 0) + 1 end");
    mods.host->call_each(ModCallback::Update);
    mods.host->call_each(ModCallback::Update);
    mods.write("a/main.lua", "t.note('again:' .. tostring(save.n or 0))");
    mods.host->scan();
    CHECK(noted("start:0"));
    CHECK(noted("again:2"));
    CHECK(mods.host->save_tables() == R"({"a":{"n":2}})");
}

TEST_CASE("save_tables keeps a disabled mod's entry") {
    Mods mods;
    mods.run("a", "function on_update() save.k = 'v' end");
    mods.host->call_each(ModCallback::Update);
    mods.host->disable("a");
    CHECK(mods.host->save_tables() == R"({"a":{"k":"v"}})");
}

TEST_CASE("a save that is not a table keeps the mod's last saved table") {
    Mods mods;
    mods.run("a", "function on_update() save.k = 'kept' end");
    mods.host->call_each(ModCallback::Update);
    mods.write("a/main.lua", "function on_update() save = 5 end");
    mods.host->scan();
    mods.host->call_each(ModCallback::Update);
    CHECK(mods.info("a").state == ModState::Running);
    CHECK(mods.host->save_tables() == R"({"a":{"k":"kept"}})");
}

TEST_CASE("load_save_tables replaces the running mods' save, and keeps entries of mods not running") {
    Mods mods;
    mods.run("a", "function on_update() t.note('k=' .. tostring(save.k)) end");
    mods.host->load_save_tables(R"({"a":{"k":"loaded"},"gone":{"x":1},"bad":5})");
    mods.host->call_each(ModCallback::Update);
    CHECK(noted("k=loaded"));
    CHECK(mods.host->save_tables() == R"({"a":{"k":"loaded"},"gone":{"x":1}})");
}

TEST_CASE("invalid save JSON gives empty save tables") {
    Mods mods;
    mods.run("a", "function on_update() t.note(tostring(next(save))) end");
    mods.host->load_save_tables("not json");
    mods.host->call_each(ModCallback::Update);
    CHECK(noted("nil"));
    CHECK(mods.host->save_tables() == R"({"a":{}})");
    Mods fresh;
    fresh.host->load_save_tables("not json");
    CHECK(fresh.host->save_tables() == "{}");
}

TEST_CASE("the definitions file declares each event's table and handler, and save") {
    Mods mods;
    const std::string text = mods.host->definitions();
    CHECK(text.find("---@class event.on_test") != std::string::npos);
    CHECK(text.find("---@field amount integer (writable 0 to 100) Changeable from 0 to 100.") != std::string::npos);
    CHECK(text.find("---@field fixed integer (read-only) Seven, not changeable.") != std::string::npos);
    CHECK(text.find("---@field cancel boolean (writable) Changeable.") != std::string::npos);
    CHECK(text.find("---@type fun(e: event.on_test)?\non_test = nil") != std::string::npos);
    CHECK(text.find("---@type table<string, any>\nsave = {}") != std::string::npos);
}

TEST_CASE("a mod with default_enabled starts enabled until it is stored otherwise") {
    Mods mods;
    mods.write("on/mod.json", R"({"default_enabled": true})");
    mods.write("on/main.lua", "t.note('on')");
    mods.write("off/mod.json", R"({"default_enabled": true})");
    mods.write("off/main.lua", "t.note('off')");
    mods.stored["off"] = {.enabled = false};
    mods.host->scan();
    CHECK(mods.info("on").state == ModState::Running);
    CHECK(mods.info("off").state == ModState::Disabled);
    CHECK(noted("on"));
    CHECK_FALSE(noted("off"));
}

TEST_CASE("held enhancements get no callback or event until released, and other mods carry on") {
    Mods mods;
    mods.write("e/mod.json", R"({"enhancement": true})");
    mods.run("e", "function on_update() t.note('e update') end function on_test() t.note('e event') end");
    mods.run("plain", "function on_update() t.note('plain update') end");
    mods.host->hold_enhancements();
    mods.host->call_each(ModCallback::Update);
    mods.host->call("e", ModCallback::Update);
    auto values = event_values();
    mods.host->raise("on_test", values);
    CHECK(notes == std::vector<std::string>{"plain update"});
    CHECK(mods.info("e").held);
    CHECK(mods.info("e").state == ModState::Running);
    CHECK_FALSE(mods.info("plain").held);
    CHECK(mods.host->running_count() == 1);

    mods.host->release_enhancements();
    notes.clear();
    mods.host->call("e", ModCallback::Update);
    mods.host->raise("on_test", values);
    CHECK(notes == std::vector<std::string>{"e update", "e event"});
    CHECK_FALSE(mods.info("e").held);
    CHECK(mods.host->running_count() == 2);
}

TEST_CASE("set_config_value changes a running mod's config table and stores it") {
    Mods mods;
    mods.run("a", "config.size = 3 function on_update() t.note(tostring(config.minimap) .. config.size .. config.name) end");
    CHECK(mods.host->set_config_value("a", "minimap", false));
    CHECK(mods.host->set_config_value("a", "size", 28.0));
    CHECK(mods.host->set_config_value("a", "name", std::string("n")));
    mods.host->call_each(ModCallback::Update);
    CHECK(noted("false28n"));
    CHECK(mods.stored.at("a").config_json.find("\"minimap\":false") != std::string::npos);
    CHECK(mods.host->config_value("a", "size") == ModConfigValue{28.0});
    CHECK_FALSE(mods.host->set_config_value("missing", "x", true));
    CHECK_FALSE(mods.host->set_config_value("a", "x", std::nan("")));
}

TEST_CASE("set_config_value on a mod not running is stored and seen when it loads") {
    Mods mods;
    mods.write("a/main.lua", "t.note(tostring(config.minimap))");
    mods.host->scan();
    CHECK(mods.host->set_config_value("a", "minimap", true));
    CHECK(mods.host->config_value("a", "minimap") == ModConfigValue{true});
    CHECK_FALSE(mods.info("a").enabled);
    mods.host->enable("a");
    CHECK(noted("true"));
}

TEST_CASE("config_value reads what the mod set itself, and nothing for a missing key or a table") {
    Mods mods;
    mods.run("a", "function on_update() config.zoom = 8 config.deep = {} end");
    mods.host->call_each(ModCallback::Update);
    CHECK(mods.host->config_value("a", "zoom") == ModConfigValue{8.0});
    CHECK_FALSE(mods.host->config_value("a", "deep"));
    CHECK_FALSE(mods.host->config_value("a", "none"));
    CHECK_FALSE(mods.host->config_value("missing", "zoom"));
}

TEST_CASE("an assets/ file replaces the disc file of its path while its mod runs") {
    Mods mods;
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.write("a/assets/KF/B0/SND0.SEQ", "SEQ bytes");
    mods.host->scan();
    mods.host->enable("a");
    REQUIRE(mods.info("a").state == ModState::Running);
    CHECK(text_of(mods.host->asset_override("KF/B0/SND0.SEQ")) == "SEQ bytes");
    CHECK_FALSE(mods.host->asset_override("KF/B0/OTHER.SEQ"));
    CHECK_FALSE(mods.host->asset_override("kf/b0/snd0.seq"));   // exact and case-sensitive
}

TEST_CASE("the last running mod in the listed order replaces the disc file") {
    Mods mods;
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.write("a/assets/KF/B0/SND0.SEQ", "first");
    mods.write("b/assets/KF/B0/SND0.SEQ", "second");
    mods.host->scan();
    mods.host->enable("a");
    mods.host->enable("b");
    CHECK(text_of(mods.host->asset_override("KF/B0/SND0.SEQ")) == "second");
    mods.host->disable("b");
    CHECK(text_of(mods.host->asset_override("KF/B0/SND0.SEQ")) == "first");
}

TEST_CASE("a held or failed mod replaces nothing, and a disabled mod replaces nothing") {
    Mods mods;
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.write("a/assets/KF/B0/SND0.SEQ", "a");
    mods.write("b/mod.json", R"({"enhancement": true})");
    mods.write("b/assets/KF/B0/SND0.SEQ", "b");
    mods.write("c/assets/KF/B0/SND0.SEQ", "c");
    mods.write("c/assets/KF/B0/nope.SEQ", "x");   // not on the disc: c fails
    mods.host->scan();
    mods.host->enable("a");
    mods.host->enable("b");
    mods.host->enable("c");
    REQUIRE(mods.info("c").state == ModState::Failed);
    CHECK(text_of(mods.host->asset_override("KF/B0/SND0.SEQ")) == "b");
    mods.host->hold_enhancements();
    CHECK(text_of(mods.host->asset_override("KF/B0/SND0.SEQ")) == "a");
    mods.host->disable("a");
    CHECK_FALSE(mods.host->asset_override("KF/B0/SND0.SEQ"));
    mods.host->release_enhancements();
    CHECK(text_of(mods.host->asset_override("KF/B0/SND0.SEQ")) == "b");
}

TEST_CASE("an assets/ file that fails its check fails the whole mod, and names the file") {
    Mods mods;
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.disc["KF/B0/SND1.TIM"] = 16;
    mods.write("a/assets/KF/B0/SND0.SEQ", "fine");
    mods.write("a/assets/KF/B0/SND1.TIM", "not a tim");
    mods.host->scan();
    mods.host->enable("a");
    const ModInfo info = mods.info("a");
    CHECK(info.state == ModState::Failed);
    CHECK(info.error.find("assets/KF/B0/SND1.TIM") != std::string::npos);
    CHECK(info.error.find("is not a TIM") != std::string::npos);
    CHECK_FALSE(mods.host->asset_override("KF/B0/SND0.SEQ"));
}

TEST_CASE("an assets/ file that is refused fails its mod, and names the file") {
    const std::string disc_name = "KF/B0/SND0.SEQ";
    Mods mods;
    mods.disc[disc_name] = 4;
    SUBCASE("a suffix no asset kind takes") {
        mods.disc["KF/readme.txt"] = 4;
        mods.write("a/assets/KF/readme.txt", "text");
        mods.host->scan();
        mods.host->enable("a");
        const ModInfo info = mods.info("a");
        CHECK(info.state == ModState::Failed);
        CHECK(info.error.find("assets/KF/readme.txt") != std::string::npos);
        CHECK(info.error.find("is not a kind of file a mod may replace") != std::string::npos);
    }
    SUBCASE("a file not on the disc") {
        mods.disc.clear();
        mods.write("a/assets/KF/B0/SND0.SEQ", "sn");
        mods.host->scan();
        mods.host->enable("a");
        CHECK(mods.info("a").state == ModState::Failed);
        CHECK(mods.info("a").error.find("assets/KF/B0/SND0.SEQ is not a file on the disc") != std::string::npos);
    }
    SUBCASE("a file larger than the disc's") {
        mods.write("a/assets/KF/B0/SND0.SEQ", "too large");
        mods.host->scan();
        mods.host->enable("a");
        CHECK(mods.info("a").state == ModState::Failed);
        CHECK(mods.info("a").error.find("assets/KF/B0/SND0.SEQ is larger than the disc's file (9 > 4 bytes)") !=
              std::string::npos);
    }
    SUBCASE("a file a link") {
        mods.write("outside.bin", "x");
        fs::create_directories(mods.dir / "a" / "assets" / "KF" / "B0");
        fs::create_symlink(mods.dir / "outside.bin", mods.dir / "a" / "assets" / "KF" / "B0" / "SND0.SEQ");
        mods.host->scan();
        mods.host->enable("a");
        CHECK(mods.info("a").state == ModState::Failed);
        CHECK(mods.info("a").error.find("assets/KF/B0/SND0.SEQ is a link") != std::string::npos);
    }
    SUBCASE("assets/ itself a link") {
        mods.write("outside/KF/B0/SND0.SEQ", "sn");
        fs::create_directories(mods.dir / "a");
        fs::create_directory_symlink(mods.dir / "outside", mods.dir / "a" / "assets");
        mods.host->scan();
        mods.host->enable("a");
        CHECK(mods.info("a").state == ModState::Failed);
        CHECK(mods.info("a").error.find("assets/ is a link") != std::string::npos);
        CHECK_FALSE(mods.host->asset_override(disc_name));
    }
}

TEST_CASE("assets/ fails the mod when the game takes no kind of file") {
    Mods mods(64u << 20, 1'000'000, {});
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.write("a/assets/KF/B0/SND0.SEQ", "sn");
    mods.host->scan();
    mods.host->enable("a");
    CHECK(mods.info("a").state == ModState::Failed);
    CHECK(mods.info("a").error.find("takes no kind of file") != std::string::npos);
}

TEST_CASE("a file exactly as large as the disc's file replaces it") {
    Mods mods;
    mods.disc["KF/B0/SND0.TIM"] = 8;
    mods.write("a/assets/KF/B0/SND0.TIM", "TIM12345");
    mods.host->scan();
    mods.host->enable("a");
    CHECK(mods.info("a").state == ModState::Running);
    CHECK(text_of(mods.host->asset_override("KF/B0/SND0.TIM")) == "TIM12345");
}

TEST_CASE("a replacement stays as it was loaded until the mod is rescanned") {
    Mods mods;
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.write("a/assets/KF/B0/SND0.SEQ", "before");
    mods.host->scan();
    mods.host->enable("a");
    const auto held = mods.host->asset_override("KF/B0/SND0.SEQ");
    mods.write("a/assets/KF/B0/SND0.SEQ", "after!");
    CHECK(text_of(mods.host->asset_override("KF/B0/SND0.SEQ")) == "before");
    CHECK(text_of(held) == "before");
    mods.host->scan();
    CHECK(text_of(mods.host->asset_override("KF/B0/SND0.SEQ")) == "after!");
    CHECK(text_of(held) == "before");
}

TEST_CASE("adding, changing or removing an assets/ file reloads the mod") {
    Mods mods;
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.disc["KF/B0/SND1.SEQ"] = 16;
    mods.write("a/assets/KF/B0/SND0.SEQ", "one");
    mods.host->scan();
    mods.host->enable("a");
    mods.write("a/assets/KF/B0/SND1.SEQ", "two");
    mods.host->scan();
    CHECK(text_of(mods.host->asset_override("KF/B0/SND1.SEQ")) == "two");
    mods.write("a/assets/KF/B0/SND1.SEQ", "three");
    mods.host->scan();
    CHECK(text_of(mods.host->asset_override("KF/B0/SND1.SEQ")) == "three");
    fs::remove(mods.dir / "a" / "assets" / "KF" / "B0" / "SND0.SEQ");
    mods.host->scan();
    CHECK_FALSE(mods.host->asset_override("KF/B0/SND0.SEQ"));
}

TEST_CASE("a mod with assets/ and no main.lua loads with no script, and a mod with neither does not") {
    Mods mods;
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.write("assets-only/assets/KF/B0/SND0.SEQ", "sn");
    mods.write("plain/mod.json", "{}");
    mods.host->scan();
    mods.host->enable("assets-only");
    mods.host->enable("plain");
    CHECK(mods.info("assets-only").state == ModState::Running);
    CHECK(mods.info("assets-only").error.empty());
    CHECK(text_of(mods.host->asset_override("KF/B0/SND0.SEQ")) == "sn");
    CHECK(mods.info("plain").state == ModState::Failed);
    CHECK(mods.info("plain").error.find("main.lua") != std::string::npos);
}

TEST_CASE("an empty assets/ file fails its mod, since the game cannot load one") {
    Mods mods;
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.write("a/assets/KF/B0/SND0.SEQ", "");
    mods.run("a", "");
    CHECK(mods.info("a").state == ModState::Failed);
    CHECK(mods.info("a").error.find("assets/KF/B0/SND0.SEQ is empty") != std::string::npos);
}

TEST_CASE("a mod with assets/ whose mod.json names a missing entry fails") {
    Mods mods;
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.write("a/assets/KF/B0/SND0.SEQ", "mine");
    mods.write("a/mod.json", R"({"entry": "typo.lua"})");
    mods.host->scan();
    mods.host->enable("a");
    CHECK(mods.info("a").state == ModState::Failed);
}

TEST_CASE("mod.assets.set replaces a disc file with the mod's file until the mod is discarded") {
    Mods mods;
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.write("a/replacement.SEQ", "mine");
    mods.run("a", "function on_load() mod.assets.set('KF/B0/SND0.SEQ', 'replacement.SEQ') end");
    CHECK(mods.info("a").state == ModState::Running);
    CHECK(text_of(mods.host->asset_override("KF/B0/SND0.SEQ")) == "mine");
    mods.host->disable("a");
    CHECK_FALSE(mods.host->asset_override("KF/B0/SND0.SEQ"));
    mods.host->enable("a");
    CHECK(text_of(mods.host->asset_override("KF/B0/SND0.SEQ")) == "mine");
}

TEST_CASE("mod.assets.set raises an error with the reason for each refused file, and the mod goes on") {
    Mods mods;
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.disc["KF/B0/BIG.SEQ"] = 2;
    mods.disc["KF/B0/BAD.TIM"] = 16;
    mods.write("a/ok.SEQ", "fine");
    mods.write("a/big.SEQ", "too big");
    mods.write("a/bad.TIM", "nope");
    fs::create_symlink(mods.dir / "a" / "ok.SEQ", mods.dir / "a" / "linked.SEQ");
    mods.run("a", R"(
        local function try(disc_path, file)
            local ok, err = pcall(mod.assets.set, disc_path, file)
            t.note(tostring(ok) .. ' ' .. tostring(err))
        end
        try('KF/B0/SND0.SEQ', 'ok.SEQ')
        try('KF/x/none.SEQ', 'ok.SEQ')
        try('KF/B0/BIG.SEQ', 'big.SEQ')
        try('KF/B0/BAD.TIM', 'bad.TIM')
        try('KF/B0/SND0.SEQ', 'linked.SEQ')
        try('KF/B0/SND0.SEQ', 'missing.SEQ')
    )");
    CHECK(notes == std::vector<std::string>{
                       "true nil",
                       "false mod.assets.set: 'KF/x/none.SEQ' is not a file on the disc",
                       "false mod.assets.set: 'KF/B0/BIG.SEQ' is larger than the disc's file (7 > 2 bytes)",
                       "false mod.assets.set: 'KF/B0/BAD.TIM' is not a TIM: it must begin with TIM",
                       "false mod.assets.set: 'KF/B0/SND0.SEQ' is a link; a mod's files are never followed",
                       "false mod.assets.set: 'KF/B0/SND0.SEQ' cannot be read"});
    CHECK(mods.info("a").state == ModState::Running);
    CHECK(text_of(mods.host->asset_override("KF/B0/SND0.SEQ")) == "fine");
}

TEST_CASE("an uncaught mod.assets.set error fails the mod") {
    Mods mods;
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.write("a/x.SEQ", "x");
    mods.run("a", "function on_load() mod.assets.set('KF/none.SEQ', 'x.SEQ') end");
    CHECK(mods.info("a").state == ModState::Failed);
    CHECK(mods.info("a").error.find("mod.assets.set") != std::string::npos);
}

TEST_CASE("mod.assets.set does not take a path outside the mod's folder") {
    Mods mods;
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.write("outside.SEQ", "secret");
    mods.run("a", R"(
        local ok, err = pcall(mod.assets.set, 'KF/B0/SND0.SEQ', '../outside.SEQ')
        t.note(tostring(ok) .. ' ' .. tostring(err))
    )");
    CHECK(notes == std::vector<std::string>{"false mod.assets.set: '../outside.SEQ' is outside the mod's folder"});
    CHECK_FALSE(mods.host->asset_override("KF/B0/SND0.SEQ"));
}

TEST_CASE("mod.assets.clear drops a replacement from assets/ until the next reload, and ignores a path not replaced") {
    Mods mods;
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.write("a/assets/KF/B0/SND0.SEQ", "from assets");
    mods.write("a/main.lua", "function on_update() mod.assets.clear('KF/B0/SND0.SEQ') mod.assets.clear('KF/B0/NONE.SEQ') end");
    mods.host->scan();
    mods.host->enable("a");
    CHECK(text_of(mods.host->asset_override("KF/B0/SND0.SEQ")) == "from assets");
    mods.host->call_each(ModCallback::Update);
    CHECK_FALSE(mods.host->asset_override("KF/B0/SND0.SEQ"));
    CHECK(mods.info("a").state == ModState::Running);
    mods.host->reload_all();
    CHECK(text_of(mods.host->asset_override("KF/B0/SND0.SEQ")) == "from assets");
}

TEST_CASE("mod.fs does not write under assets/, so the write cannot reload the mod") {
    Mods mods;
    mods.disc["KF/B0/SND0.SEQ"] = 16;
    mods.run("a", R"(
        local ok, why = mod.fs.write('assets/KF/B0/SND0.SEQ', 'x')
        t.note(tostring(ok) .. ' ' .. why)
        ok, why = mod.fs.write('./Assets/other.bin', 'x')
        t.note(tostring(ok) .. ' ' .. why)
    )");
    CHECK(notes == std::vector<std::string>{"nil a mod cannot write under its assets/ folder",
                                            "nil a mod cannot write under its assets/ folder"});
    CHECK_FALSE(fs::exists(mods.dir / "a" / "assets"));
}

TEST_CASE("mod_file_bytes reads a file inside the mod's folder up to the cap, and nothing else") {
    Mods mods;
    mods.write("a/data/seed.bin", "12345678");
    mods.write("a/big.bin", std::string(32, 'x'));
    mods.write("outside/secret.bin", "secret");
    fs::create_directories(mods.dir / "a");
    fs::create_directory_symlink(mods.dir / "outside", mods.dir / "a" / "link");
    mods.run("a", R"(
        t.note(tostring(t.size("data/seed.bin")))
        t.note(tostring(t.size("big.bin")))
        t.note(tostring(t.size("missing.bin")))
        t.note(tostring(t.size("data")))
        t.note(tostring(t.size("../outside/secret.bin")))
        t.note(tostring(t.size("link/secret.bin")))
    )");
    CHECK(notes == std::vector<std::string>{"8", "nil", "nil", "nil", "nil", "nil"});
}

// ---- other mods: exports, mods.get, mod.raise, mods.on ----

TEST_CASE("mods.get reads another mod's exports as copies") {
    Mods mods;
    mods.run("b", "exports.count = 3 exports.name = 'bee' exports.pi = 1.5 exports.on = true\n"
                  "exports.list = {10, 20, nested = {deep = 'x'}}\n"
                  "function on_update() t.note('b list ' .. exports.list[1]) end");
    mods.run("a", "function on_update()\n"
                  "  local b = mods.get('b')\n"
                  "  t.note(b.count .. b.name .. b.pi .. tostring(b.on) .. tostring(b.none))\n"
                  "  local list = b.list\n"
                  "  t.note(list[2] .. list.nested.deep)\n"
                  "  list[1] = 99\n"
                  "  t.note(math.type(b.count))\n"
                  "end");
    mods.host->call("a", ModCallback::Update);
    CHECK(noted("3bee1.5truenil"));
    CHECK(noted("20x"));
    CHECK(noted("integer"));
    mods.host->call("b", ModCallback::Update);
    CHECK(noted("b list 10"));
}

TEST_CASE("mods.get's table calls a function with copies and gets all it returns") {
    Mods mods;
    mods.run("b", "function exports.add(x, y) return x + y, 'sum' end\n"
                  "function exports.edit(list) list[1] = 2 return list end");
    mods.run("a", "function on_update()\n"
                  "  local sum, word = mods.get('b').add(2, 3)\n"
                  "  t.note(sum .. word)\n"
                  "  local mine = {1}\n"
                  "  local theirs = mods.get('b').edit(mine)\n"
                  "  t.note(mine[1] .. theirs[1])\n"
                  "end");
    mods.host->call("a", ModCallback::Update);
    CHECK(noted("5sum"));
    CHECK(noted("12"));
}

TEST_CASE("mods.get is nil for a mod not running, and its table follows the mod through reloads") {
    Mods mods;
    mods.write("b/main.lua", "exports.n = 1 function exports.f() return 'one' end");
    mods.host->scan();
    mods.run("a", "function on_update()\n"
                  "  t.note('unknown ' .. tostring(mods.get('nobody')))\n"
                  "  if not kept then\n"
                  "    t.note('off ' .. tostring(mods.get('b')))\n"
                  "    return\n"
                  "  end\n"
                  "  local value, why = call()\n"
                  "  t.note(tostring(kept.n) .. ' ' .. tostring(value) .. ' ' .. tostring(why))\n"
                  "end");
    mods.host->call("a", ModCallback::Update);
    CHECK(noted("unknown nil"));
    CHECK(noted("off nil"));

    mods.host->enable("b");
    mods.write("a/main.lua", "kept = nil call = nil\n"
                             "function on_update()\n"
                             "  kept = kept or mods.get('b')\n"
                             "  call = call or kept.f\n"
                             "  local value, why = call()\n"
                             "  t.note(tostring(kept.n) .. ' ' .. tostring(value) .. ' ' .. tostring(why))\n"
                             "end");
    mods.host->scan();
    mods.host->call("a", ModCallback::Update);
    CHECK(noted("1 one nil"));

    mods.write("b/main.lua", "exports.n = 2 function exports.f() return 'two' end");
    mods.host->scan();
    mods.host->call("a", ModCallback::Update);
    CHECK(noted("2 two nil"));

    mods.host->disable("b");
    mods.host->call("a", ModCallback::Update);
    CHECK(noted("nil nil mod 'b' is not running"));
    CHECK(mods.info("a").state == ModState::Running);
}

TEST_CASE("a mod's error in an exported function stops only that mod") {
    Mods mods;
    mods.run("b", "function exports.bad() error('broken') end");
    mods.run("a", "function on_update()\n"
                  "  local value, why = mods.get('b').bad()\n"
                  "  t.note(tostring(value))\n"
                  "  t.note(why:find('broken', 1, true) and 'said why' or 'silent')\n"
                  "end");
    mods.host->call("a", ModCallback::Update);
    CHECK(noted("nil"));
    CHECK(noted("said why"));
    CHECK(mods.info("a").state == ModState::Running);
    CHECK(mods.info("b").state == ModState::Failed);
    CHECK(mods.info("b").error.find("exports.bad") != std::string::npos);
}

TEST_CASE("a function that runs away stops its own mod, and a caller that keeps calling stops the caller") {
    SUBCASE("the function runs away") {
        Mods mods(64u << 20, 1'000'000);
        mods.run("b", "function exports.spin() while true do end end");
        mods.run("a", "function on_update() local _, why = mods.get('b').spin() t.note(why and 'told' or 'not') end");
        mods.host->call("a", ModCallback::Update);
        CHECK(noted("told"));
        CHECK(mods.info("a").state == ModState::Running);
        CHECK(mods.info("b").state == ModState::Failed);
    }
    SUBCASE("the caller keeps calling") {
        Mods mods(64u << 20, 1'000'000);
        mods.run("b", "function exports.work() for i = 1, 20000 do end end");
        mods.run("a", "function on_update() local b = mods.get('b') for i = 1, 100000 do b.work() end t.note('done') end");
        mods.host->call("a", ModCallback::Update);
        CHECK_FALSE(noted("done"));
        CHECK(mods.info("a").state == ModState::Failed);
        CHECK(mods.info("a").error.find("budget") != std::string::npos);
        CHECK(mods.info("b").state == ModState::Running);
    }
}

TEST_CASE("what cannot cross between mods is an error in the mod that holds it") {
    Mods mods;
    SUBCASE("a function as an argument stops the caller") {
        mods.run("b", "function exports.take() t.note('reached') end");
        mods.run("a", "function on_update() mods.get('b').take(print) end");
        mods.host->call("a", ModCallback::Update);
        CHECK_FALSE(noted("reached"));
        CHECK(mods.info("a").state == ModState::Failed);
        CHECK(mods.info("a").error.find("cannot cross") != std::string::npos);
        CHECK(mods.info("b").state == ModState::Running);
    }
    SUBCASE("a table holding itself stops the caller, and pcall can catch it first") {
        mods.run("b", "function exports.take() end");
        mods.run("a", "function on_update()\n"
                      "  local loop = {} loop.me = loop\n"
                      "  t.note(tostring(pcall(mods.get('b').take, loop)))\n"
                      "end");
        mods.host->call("a", ModCallback::Update);
        CHECK(noted("false"));
        CHECK(mods.info("a").state == ModState::Running);
    }
    SUBCASE("a table as a key stops the caller") {
        mods.run("b", "function exports.take() end");
        mods.run("a", "function on_update() mods.get('b').take({[{}] = 1}) end");
        mods.host->call("a", ModCallback::Update);
        CHECK(mods.info("a").state == ModState::Failed);
    }
    SUBCASE("a function inside an exported table stops the exporter, and reads as nil") {
        mods.run("b", "exports.bad = {inner = print}");
        mods.run("a", "function on_update() t.note(tostring(mods.get('b').bad)) end");
        mods.host->call("a", ModCallback::Update);
        CHECK(noted("nil"));
        CHECK(mods.info("a").state == ModState::Running);
        CHECK(mods.info("b").state == ModState::Failed);
    }
    SUBCASE("a function returned stops the exporter") {
        mods.run("b", "function exports.give() return print end");
        mods.run("a", "function on_update() local v, why = mods.get('b').give() t.note(tostring(v)) end");
        mods.host->call("a", ModCallback::Update);
        CHECK(noted("nil"));
        CHECK(mods.info("b").state == ModState::Failed);
    }
}

TEST_CASE("a table reached many times over costs its copier's budget, and stops at the budget") {
    Mods mods(64u << 20, 1'000'000);
    mods.run("b", "function exports.take() t.note('reached') end");
    mods.run("a", "function on_update()\n"
                  "  local wide = {}\n"
                  "  for level = 1, 12 do\n"
                  "    local next_level = {}\n"
                  "    for i = 1, 10 do next_level[i] = wide end\n"
                  "    wide = next_level\n"
                  "  end\n"
                  "  mods.get('b').take(wide)\n"
                  "end");
    mods.host->call("a", ModCallback::Update);
    CHECK_FALSE(noted("reached"));
    CHECK(mods.info("a").state == ModState::Failed);
    CHECK(mods.info("b").state == ModState::Running);
}

TEST_CASE("values over the memory cap do not cross, however many there are") {
    Mods mods(1u << 20, 100'000'000);
    mods.run("b", "local s = string.rep('x', 400000)\n"
                  "function exports.give() return s, s, s end");
    mods.run("a", "function on_update() local v, why = mods.get('b').give() t.note(tostring(v)) end");
    mods.host->call("a", ModCallback::Update);
    CHECK(noted("nil"));
    CHECK(mods.info("a").state == ModState::Running);
    CHECK(mods.info("b").state == ModState::Failed);
    CHECK(mods.info("b").error.find("too large") != std::string::npos);
}

TEST_CASE("mods calling each other without end are told it is too deep, and both run on") {
    Mods mods;
    mods.run("a", "depth = 0 deepest = 0\n"
                  "function exports.ping()\n"
                  "  depth = depth + 1 deepest = math.max(deepest, depth)\n"
                  "  local v, why = mods.get('b').pong()\n"
                  "  depth = depth - 1\n"
                  "  return v, why\n"
                  "end\n"
                  "function on_update()\n"
                  "  local v, why = exports.ping()\n"
                  "  t.note(why)\n"
                  "  t.note('deepest ' .. deepest)\n"
                  "end");
    mods.run("b", "function exports.pong() return mods.get('a').ping() end");
    mods.host->call("a", ModCallback::Update);
    CHECK(noted("calls between mods are nested too deep"));
    CHECK(noted("deepest 9"));
    CHECK(mods.info("a").state == ModState::Running);
    CHECK(mods.info("b").state == ModState::Running);
}

TEST_CASE("an error in a call that came back into the first mod stops it after it returns") {
    Mods mods;
    mods.run("a", "function exports.bad() error('inner') end\n"
                  "function on_update() mods.get('b').back() t.note('after') end");
    mods.run("b", "function exports.back() local _, why = mods.get('a').bad() t.note(why and 'b told' or 'b not') end");
    mods.host->call("a", ModCallback::Update);
    CHECK(noted("b told"));
    CHECK_FALSE(noted("after"));
    CHECK(mods.info("a").state == ModState::Failed);
    CHECK(mods.info("b").state == ModState::Running);
}

TEST_CASE("another mod's exports cannot be written or walked, but pcall may try") {
    Mods mods;
    mods.run("b", "exports.n = 1");
    mods.run("a", "function on_update()\n"
                  "  local b = mods.get('b')\n"
                  "  t.note('write ' .. tostring(pcall(function() b.n = 2 end)))\n"
                  "  t.note('walk ' .. tostring(pcall(function() for k in pairs(b) do end end)))\n"
                  "  t.note('meta ' .. tostring(getmetatable(b)))\n"
                  "  t.note('number key ' .. tostring(b[1]))\n"
                  "  t.note('n ' .. b.n)\n"
                  "end");
    mods.host->call("a", ModCallback::Update);
    CHECK(noted("write false"));
    CHECK(noted("walk false"));
    CHECK(noted("meta locked"));
    CHECK(noted("number key nil"));
    CHECK(noted("n 1"));
}

TEST_CASE("exports that are not a table read as nothing") {
    Mods mods;
    mods.run("b", "exports = 5");
    mods.run("a", "function on_update() local b = mods.get('b') t.note(tostring(b.x)) end");
    mods.host->call("a", ModCallback::Update);
    CHECK(noted("nil"));
    CHECK(mods.info("a").state == ModState::Running);
}

TEST_CASE("a mod held with the enhancements cannot be reached") {
    Mods mods;
    mods.write("b/mod.json", R"({"enhancement": true})");
    mods.run("b", "exports.n = 1 mods.on('a', 'ping', function() t.note('b heard') end)");
    mods.run("a", "function on_update() t.note(tostring(mods.get('b'))) mod.raise('ping') end");
    mods.host->hold_enhancements();
    mods.host->call("a", ModCallback::Update);
    CHECK(noted("nil"));
    CHECK_FALSE(noted("b heard"));
    mods.host->release_enhancements();
    mods.host->call("a", ModCallback::Update);
    CHECK(noted("b heard"));
}

TEST_CASE("a config a peer changes is saved when its call returns") {
    Mods mods;
    mods.run("b", "function exports.set() config.x = 7 mod.save_config() end");
    mods.run("a", "function on_update() mods.get('b').set() end");
    mods.host->call("a", ModCallback::Update);
    CHECK(mods.stored.at("b").config_json.find('7') != std::string::npos);
}

TEST_CASE("a finalizer in a closing mod reaches nobody") {
    Mods mods;
    mods.run("b", "exports.n = 1 function exports.f() t.note('reached') end");
    mods.run("a", "local b = mods.get('b')\n"
                  "local f = b.f\n"
                  "keep = setmetatable({}, {__gc = function()\n"
                  "  t.note('finalizer ' .. tostring(mods.get('b')) .. tostring(b.n))\n"
                  "  f()\n"
                  "  t.note('raised ' .. type(mod.raise('x')))\n"
                  "  mods.on('b', 'x', print)\n"
                  "end})");
    mods.host->disable("a");
    CHECK(noted("finalizer nilnil"));
    CHECK(noted("raised table"));
    CHECK_FALSE(noted("reached"));
    CHECK(mods.info("b").state == ModState::Running);
}

TEST_CASE("mod.raise reaches subscribers in order, each seeing what the last left, and returns it") {
    Mods mods;
    mods.run("sub1", "mods.on('src', 'hit', function(e) t.note('one ' .. e.amount) e.amount = e.amount + 1 e.list = {1, 2} end)");
    mods.run("sub2", "mods.on('src', 'hit', function(e) t.note('two ' .. e.amount .. ' ' .. #e.list) e.amount = e.amount * 2 end)");
    mods.run("sub3", "mods.on('other', 'hit', function(e) t.note('wrong source') end)\n"
                     "mods.on('src', 'miss', function(e) t.note('wrong event') end)");
    mods.run("src", "function on_update()\n"
                    "  local mine = {amount = 1}\n"
                    "  local out = mod.raise('hit', mine)\n"
                    "  t.note('out ' .. out.amount .. ' mine ' .. mine.amount)\n"
                    "  t.note('empty ' .. type(mod.raise('nobody')))\n"
                    "end");
    mods.host->call("src", ModCallback::Update);
    CHECK(noted("one 1"));
    CHECK(noted("two 2 2"));
    CHECK(noted("out 4 mine 1"));
    CHECK(noted("empty table"));
    CHECK_FALSE(noted("wrong source"));
    CHECK_FALSE(noted("wrong event"));
}

TEST_CASE("a subscriber that errs is stopped and changes nothing, and the others hear the event") {
    Mods mods;
    mods.run("sub1", "mods.on('src', 'hit', function(e) e.amount = 99 error('boom') end)");
    mods.run("sub2", "mods.on('src', 'hit', function(e) t.note('two ' .. e.amount) end)");
    mods.run("src", "function on_update() t.note('out ' .. mod.raise('hit', {amount = 1}).amount) end");
    mods.host->call("src", ModCallback::Update);
    CHECK(noted("two 1"));
    CHECK(noted("out 1"));
    CHECK(mods.info("sub1").state == ModState::Failed);
    CHECK(mods.info("sub1").error.find("event ") != std::string::npos);
    CHECK(mods.info("src").state == ModState::Running);
}

TEST_CASE("a subscription lasts through the raiser's reload, and goes with the subscriber's") {
    Mods mods;
    mods.run("sub", "count = 0\n"
                    "mods.on('src', 'hit', function() t.note('heard') end)\n"
                    "function exports.stop() mods.on('src', 'hit', nil) end");
    mods.run("src", "function on_update() mod.raise('hit') end");
    mods.host->call("src", ModCallback::Update);
    CHECK(noted("heard"));

    notes.clear();
    mods.write("src/main.lua", "function on_update() mod.raise('hit') t.note('new src') end");
    mods.host->scan();
    mods.host->call("src", ModCallback::Update);
    CHECK(noted("new src"));
    CHECK(noted("heard"));

    notes.clear();
    mods.write("sub/main.lua", "t.note('new sub')");
    mods.host->scan();
    mods.host->call("src", ModCallback::Update);
    CHECK(noted("new sub"));
    CHECK_FALSE(noted("heard"));
}

TEST_CASE("mods.on with nil removes a subscription, and a handler must be a function") {
    Mods mods;
    mods.run("sub", "mods.on('src', 'hit', function() t.note('heard') end)\n"
                    "function exports.stop() mods.on('src', 'hit', nil) end\n"
                    "t.note(tostring(pcall(mods.on, 'src', 'hit', 5)))");
    mods.run("src", "function on_update() mods.get('sub').stop() mod.raise('hit') end");
    CHECK(noted("false"));
    mods.host->call("src", ModCallback::Update);
    CHECK_FALSE(noted("heard"));
}

TEST_CASE("a subscriber's spending is charged to the raiser") {
    Mods mods(64u << 20, 1'000'000);
    mods.run("sub", "mods.on('src', 'hit', function() for i = 1, 20000 do end end)");
    mods.run("src", "function on_update() for i = 1, 100000 do mod.raise('hit') end t.note('done') end");
    mods.host->call("src", ModCallback::Update);
    CHECK_FALSE(noted("done"));
    CHECK(mods.info("src").state == ModState::Failed);
    CHECK(mods.info("sub").state == ModState::Running);
}

TEST_CASE("an event raised without end across mods is cut off at the depth, and everyone runs on") {
    Mods mods;
    mods.run("a", "mods.on('b', 'x', function() mod.raise('x') end) function exports.go() mod.raise('x') end");
    mods.run("b", "mods.on('a', 'x', function() mod.raise('x') t.note('b back') end)");
    mods.run("c", "function on_update() mods.get('a').go() t.note('c done') end");
    mods.host->call("c", ModCallback::Update);
    CHECK(noted("c done"));
    CHECK(noted("b back"));
    CHECK(mods.info("a").state == ModState::Running);
    CHECK(mods.info("b").state == ModState::Running);
}

TEST_CASE("the definitions file lists exports and the other mods' calls") {
    Mods mods;
    const std::string text = mods.host->definitions();
    CHECK(text.find("exports = {}") != std::string::npos);
    CHECK(text.find("---@class mods") != std::string::npos);
    CHECK(text.find("---@field raise") != std::string::npos);
}

TEST_CASE("a mod.json's api_version must fit the game's: the same major, no newer a minor") {
    const auto state_with = [](const std::string &wanted) {
        Mods mods;   // its API is 9.9
        mods.write("a/mod.json", R"({"api_version": ")" + wanted + R"("})");
        mods.run("a", "t.note('ran')");
        return std::pair{mods.info("a").state, mods.info("a").error};
    };
    CHECK(state_with("9.9").first == ModState::Running);
    CHECK(state_with("9.0").first == ModState::Running);
    CHECK(state_with("9").first == ModState::Running);
    CHECK(state_with("9.9.4").first == ModState::Running);
    CHECK_FALSE(noted("no such"));

    const auto newer = state_with("9.10");
    CHECK(newer.first == ModState::Failed);
    CHECK(newer.second.find("written for API 9.10, and this game has 9.9") != std::string::npos);
    CHECK(state_with("10.0").first == ModState::Failed);
    CHECK(state_with("8.9").first == ModState::Failed);
    for (const char *const bad : {"new", "1.x", "1.", ".1", "1.2x", "-1.0"}) {
        INFO(bad);
        const auto refused = state_with(bad);
        CHECK(refused.first == ModState::Failed);
        CHECK(refused.second.find("is not a version") != std::string::npos);
    }
}

TEST_CASE("a mod refused for its api_version runs when mod.json is fixed, and one that says nothing runs") {
    Mods mods;
    mods.write("a/mod.json", R"({"api_version": "10.0"})");
    mods.run("a", "t.note('ran')");
    CHECK_FALSE(noted("ran"));
    mods.write("a/mod.json", R"({"name": "A"})");
    mods.host->scan();
    CHECK(noted("ran"));
    CHECK(mods.info("a").state == ModState::Running);
}
