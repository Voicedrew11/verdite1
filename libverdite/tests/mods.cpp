#include <verdite/mods.h>

#include <doctest/doctest.h>

#include <lauxlib.h>
#include <lua.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
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

// The host the nested-raise test raises through, from inside a handler.
ModHost *nested_host = nullptr;
int again(lua_State *) {
    std::array<ModEventValue, 5> values{std::int64_t{1}, std::int64_t{2}, false, std::string_view("n"), false};
    nested_host->raise("on_test", values);
    return 0;
}

// A mods folder of its own, and a host over it whose stored settings are a map.
struct Mods {
    fs::path dir;
    std::map<std::string, ModStored, std::less<>> stored;
    std::unique_ptr<ModHost> host;
    int writes = 0;
    std::vector<std::string> unloaded_ids;   // each discard of a mod's Lua state, in order

    explicit Mods(std::size_t memory_cap_bytes = 64u << 20, std::uint64_t instructions = 1'000'000) {
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
                                     {"again", "fun()", "Raises on_test from inside a handler.", again}},
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
