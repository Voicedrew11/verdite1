#include <verdite/mods.h>

#include <doctest/doctest.h>

#include <lauxlib.h>
#include <lua.h>

#include <unistd.h>

#include <algorithm>
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

// A mods folder of its own, and a host over it whose stored settings are a map.
struct Mods {
    fs::path dir;
    std::map<std::string, ModStored, std::less<>> stored;
    std::unique_ptr<ModHost> host;
    int writes = 0;

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
                                     {"changed", "fun()", "Marks config changed.", changed}},
                       .fields = {{"value", "integer", "A number.", get_value, set_value},
                                  {"fixed", "integer", "Seven.", get_fixed, nullptr}}}};
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
