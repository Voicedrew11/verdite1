#include <verdite/mods.h>
#include <verdite/raw_bind.h>
#include <verdite/raw_call.h>
#include <verdite/raw_call_bind.h>

#include <doctest/doctest.h>

#include <lauxlib.h>
#include <lua.h>

#include <unistd.h>

#include <array>
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

// A small game whose functions take each kind the call layer describes.
namespace call_game {

enum class Mode : std::uint8_t { Off = 0, On = 1 };
constexpr std::array<RawEnumValue, 2> mode_values{{{"MODE_OFF", 0}, {"MODE_ON", 1}}};

struct Vector {
    std::int32_t x = 0, y = 0;
};
struct Actor {
    std::int32_t hp = 0;
    bool in_use = false;
};

std::array<Actor, 4> actors;
std::int32_t hp = 0, last_scale = 0, calls = 0;
Mode mode = Mode::Off;
Vector position;
ModHost *host_to_raise = nullptr;
std::int32_t frames_run = 0;
void (*during_frame)() = nullptr;

void add_hp(std::int16_t delta) {
    hp += delta;
    ++calls;
}
std::int32_t twice(std::int32_t value) {
    return value * 2;
}
void set_mode(Mode value) {
    mode = value;
}
void scaled(float scale) {
    last_scale = static_cast<std::int32_t>(scale);
}
void move_to(const Vector &to) {
    position = to;
}
Vector position_now() {
    return position;
}
const Vector &position_ref() {
    return position;
}
void sum_into(const Vector *a, const Vector &b, Vector *out) {
    out->x = a->x + b.x;
    out->y = a->y + b.y;
}
std::int32_t double_in_place(Vector &value) {
    value.x *= 2;
    value.y *= 2;
    return value.x + value.y;
}
void hurt(Actor *actor, std::int32_t amount) {
    actor->hp -= amount;
}
void hurt_ref(Actor &actor) {
    actor.hp -= 1;
}
bool hp_of_or_none(const Actor *actor, std::int32_t wanted) {
    return actor ? actor->hp == wanted : wanted == -1;
}
Actor *first_in_use() {
    for (Actor &actor : actors)
        if (actor.in_use)
            return &actor;
    return nullptr;
}
Actor &first_slot() {
    return actors[0];
}
void raise_test() {
    std::array<ModEventValue, 1> values{std::int64_t{1}};
    host_to_raise->raise("on_test", values);
}
// A game function that presents frames: each runs the mods' on_update, as the game's frames do.
void run_frames(std::int32_t count) {
    frames_run = 0;
    for (std::int32_t i = 0; i < count; ++i) {
        host_to_raise->frame_passed();
        host_to_raise->call_each(ModCallback::Update);
        if (during_frame)
            during_frame();
        ++frames_run;
    }
}
void add_then_subtract(std::int32_t first, std::int32_t second) {
    hp += first - second;
}
std::array<std::int16_t, 2> pair_now() {
    return {3, 4};
}
void take_pair(const std::array<std::int16_t, 2> &pair) {
    hp = pair[0] + pair[1];
}

std::string_view actor_in_use(std::int64_t slot) {
    return actors[static_cast<std::size_t>(slot)].in_use ? std::string_view{} : "that actor's slot is free";
}
std::string_view first_is_larger(std::span<const RawValue> arguments) {
    return std::get<std::int64_t>(arguments[0]) > std::get<std::int64_t>(arguments[1]) ? std::string_view{}
                                                                                       : "first must be larger";
}

} // namespace call_game

template <> struct verdite::RawTypeOf<call_game::Mode> {
    static const RawType &type() {
        static const RawType described = raw_enum_type<call_game::Mode>("Mode", call_game::mode_values);
        return described;
    }
};
template <> struct verdite::RawTypeOf<call_game::Vector> {
    static const RawType &type() {
        static const std::array fields{
            raw_field<call_game::Vector, [](call_game::Vector &v) -> auto & { return v.x; }>("x", [] {
                RawLimits limits;
                limits.range = RawRange{-100, 100};
                return limits;
            }()),
            raw_field<call_game::Vector, [](call_game::Vector &v) -> auto & { return v.y; }>("y"),
        };
        static const RawType described = raw_struct_type("Vector", fields);
        return described;
    }
};
template <> struct verdite::RawPoolOf<call_game::Actor> {
    static std::span<call_game::Actor> pool() { return call_game::actors; }
};

namespace {

RawParamListing param(std::string_view name) {
    RawParamListing listing;
    listing.name = name;
    return listing;
}
RawParamListing range_param(std::string_view name, std::int64_t min, std::int64_t max) {
    RawParamListing listing = param(name);
    listing.limits.range = RawRange{min, max};
    return listing;
}
RawParamListing role_param(std::string_view name, RawParamRole role) {
    RawParamListing listing = param(name);
    listing.role = role;
    return listing;
}
RawParamListing checked_param(std::string_view name, RawArgumentCheck check) {
    RawParamListing listing = param(name);
    listing.check = check;
    return listing;
}
RawParamListing nullable_param(std::string_view name) {
    RawParamListing listing = param(name);
    listing.nil_passes_null = true;
    return listing;
}
RawFunctionListing any_time() {
    RawFunctionListing listing;
    listing.when = RawCallWhen::AnyTime;
    return listing;
}
RawFunctionListing with_doc(std::string_view doc) {
    RawFunctionListing listing;
    listing.doc = doc;
    return listing;
}
RawFunctionListing runs_frames() {
    RawFunctionListing listing;
    listing.when = RawCallWhen::RunsFrames;
    return listing;
}
RawFunctionListing with_check(RawCallCheck check) {
    RawFunctionListing listing;
    listing.check = check;
    return listing;
}

int began = 0, ended = 0;

const RawCallLayer &test_layer() {
    using A0 = std::array<RawParamListing, 0>;
    static const std::array functions{
        raw_function<&call_game::add_hp>("add_hp", std::array{range_param("delta", -50, 50)},
                                    with_doc("Adds to the HP.")),
        raw_function<&call_game::twice>("twice", std::array{param("value")}, any_time()),
        raw_function<&call_game::set_mode>("set_mode", std::array{param("value")}),
        raw_function<&call_game::scaled>("scaled", std::array{param("scale")}),
        raw_function<&call_game::move_to>("move_to", std::array{param("to")}),
        raw_function<&call_game::position_now>("position_now", A0{}, any_time()),
        raw_function<&call_game::position_ref>("position_ref", A0{}, any_time()),
        raw_function<&call_game::sum_into>(
            "sum_into", std::array{param("a"), param("b"), role_param("out", RawParamRole::Out)}),
        raw_function<&call_game::double_in_place>("double_in_place", std::array{role_param("value", RawParamRole::InOut)}),
        raw_function<&call_game::hurt>("hurt",
                                  std::array{checked_param("actor", call_game::actor_in_use),
                                             param("amount")}),
        raw_function<&call_game::hurt_ref>("hurt_ref", std::array{param("actor")}),
        raw_function<&call_game::hp_of_or_none>("hp_of_or_none",
                                           std::array{nullable_param("actor"),
                                                      param("wanted")}),
        raw_function<&call_game::first_in_use>("first_in_use", A0{}),
        raw_function<&call_game::first_slot>("first_slot", A0{}),
        raw_function<&call_game::raise_test>("raise_test", A0{}),
        raw_function<&call_game::pair_now>("pair_now", A0{}),
        raw_function<&call_game::take_pair>("take_pair", std::array{param("pair")}),
        raw_function<&call_game::run_frames>("run_frames", std::array{param("count")}, runs_frames()),
        raw_function<&call_game::add_then_subtract>("add_then_subtract", std::array{param("first"), param("second")},
                                                    with_check(call_game::first_is_larger)),
    };
    static const RawCallLayer layer{"g.call",
                                    "The test's functions.",
                                    functions,
                                    100'000,
                                    [] { ++began; },
                                    [] { ++ended; },
                                    400'000};
    return layer;
}

void reset_game() {
    call_game::actors = {};
    call_game::hp = call_game::last_scale = call_game::calls = 0;
    call_game::mode = call_game::Mode::Off;
    call_game::position = {};
    call_game::frames_run = 0;
    call_game::during_frame = nullptr;
    began = ended = 0;
}

std::vector<std::string> notes;
int note(lua_State *L) {
    notes.emplace_back(luaL_tolstring(L, 1, nullptr));
    return 0;
}

// A mods folder and a host with the layer, on a budget of 1,000,000.
struct Mods {
    fs::path dir;
    std::unique_ptr<ModHost> host;

    Mods() {
        static int count;
        dir = fs::temp_directory_path() / ("verdite-calls-" + std::to_string(::getpid()) + "-" + std::to_string(++count));
        fs::remove_all(dir);
        reset_game();
        notes.clear();
        ModHostConfig config;
        config.mods_dir = dir;
        config.api_version = "9.9";
        config.instructions_per_call = 1'000'000;
        config.calls = test_layer();
        config.api = {ModTable{"t", "", {{"note", "fun(v: any)", "Notes v.", note}}, {}}};
        config.events = {ModEvent{"on_test", "", {{"amount", "integer", "", false, 0, 0}}}};
        host = std::make_unique<ModHost>(std::move(config));
        call_game::host_to_raise = host.get();
    }
    ~Mods() {
        host.reset();
        call_game::host_to_raise = nullptr;
        std::error_code error;
        fs::remove_all(dir, error);
    }
    Mods(const Mods &) = delete;
    Mods &operator=(const Mods &) = delete;

    void run(const std::string &id, const std::string &script) {
        const fs::path path = dir / id / "main.lua";
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << script;
        host->scan();
        host->enable(id);
    }
    // Runs body as on_between_updates once; the mod's state afterwards.
    ModInfo between(const std::string &body) {
        run("a", "function on_between_updates()\n" + body + "\nend");
        host->call_each(ModCallback::BetweenUpdates);
        return host->mods().front();
    }
};

bool has(const std::string &text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("raw_call runs a function and returns its value") {
    reset_game();
    const std::array<RawValue, 1> arguments{std::int64_t{21}};
    const RawCallResult result = raw_call(test_layer(), "twice", arguments);
    CHECK(result.error.empty());
    REQUIRE(result.values.size() == 1);
    CHECK(std::get<std::int64_t>(result.values[0]) == 42);
    CHECK(began == 1);
    CHECK(ended == 1);
}

TEST_CASE("raw_call refuses a bad argument before any game code runs") {
    reset_game();
    const RawCallLayer &layer = test_layer();
    const auto refused = [&](std::string_view name, const std::vector<RawValue> &arguments, std::string_view says) {
        const RawCallResult result = raw_call(layer, name, arguments);
        INFO(result.error);
        CHECK(has(result.error, says));
        CHECK(result.values.empty());
    };
    refused("nothing", {}, "no function named");
    refused("add_hp", {}, "takes 1 values, not 0");
    refused("add_hp", {std::int64_t{1}, std::int64_t{2}}, "takes 1 values, not 2");
    refused("add_hp", {std::int64_t{51}}, "out of range for add_hp(delta) (-50 to 50)");
    refused("add_hp", {std::string("x")}, "needs an integer");
    refused("add_hp", {true}, "needs an integer");
    refused("set_mode", {std::int64_t{5}}, "is not a value of set_mode(value)");
    refused("scaled", {std::string("x")}, "needs a number");
    refused("move_to", {std::int64_t{101}, std::int64_t{0}}, "out of range for move_to(to).x");
    refused("move_to", {std::int64_t{1}}, "takes 2 values, not 1");
    refused("hurt", {std::int64_t{4}, std::int64_t{1}}, "out of range for hurt(actor) (0 to 3)");
    refused("hurt", {std::int64_t{-1}, std::int64_t{1}}, "out of range for hurt(actor)");
    refused("hurt", {std::int64_t{0}, std::int64_t{1}}, "that actor's slot is free");
    refused("take_pair", {std::int64_t{1}}, "takes 2 values, not 1");
    CHECK(call_game::calls == 0);
    CHECK(began == 0);
}

TEST_CASE("raw_call passes a whole number to a float, and structs flattened") {
    reset_game();
    const RawCallLayer &layer = test_layer();
    CHECK(raw_call(layer, "scaled", std::array<RawValue, 1>{std::int64_t{7}}).error.empty());
    CHECK(call_game::last_scale == 7);
    CHECK(raw_call(layer, "move_to", std::array<RawValue, 2>{std::int64_t{3}, std::int64_t{4}}).error.empty());
    CHECK(call_game::position.x == 3);
    CHECK(call_game::position.y == 4);
    const RawCallResult now = raw_call(layer, "position_now", {});
    REQUIRE(now.values.size() == 2);
    CHECK(std::get<std::int64_t>(now.values[1]) == 4);
    CHECK(raw_call(layer, "position_ref", {}).values.size() == 2);
}

TEST_CASE("raw_call returns an Out or InOut parameter after the return value") {
    reset_game();
    const RawCallLayer &layer = test_layer();
    const std::array<RawValue, 4> sum{std::int64_t{1}, std::int64_t{2}, std::int64_t{10}, std::int64_t{20}};
    const RawCallResult out = raw_call(layer, "sum_into", sum);
    CHECK(out.error.empty());
    REQUIRE(out.values.size() == 2);
    CHECK(std::get<std::int64_t>(out.values[0]) == 11);
    CHECK(std::get<std::int64_t>(out.values[1]) == 22);
    const RawCallResult inout =
        raw_call(layer, "double_in_place", std::array<RawValue, 2>{std::int64_t{2}, std::int64_t{3}});
    REQUIRE(inout.values.size() == 3);
    CHECK(std::get<std::int64_t>(inout.values[0]) == 10);
    CHECK(std::get<std::int64_t>(inout.values[1]) == 4);
    CHECK(std::get<std::int64_t>(inout.values[2]) == 6);
}

TEST_CASE("a pool's element crosses as its slot, and -1 as null only where listed") {
    reset_game();
    const RawCallLayer &layer = test_layer();
    call_game::actors[2].in_use = true;
    call_game::actors[2].hp = 10;
    CHECK(raw_call(layer, "hurt", std::array<RawValue, 2>{std::int64_t{2}, std::int64_t{3}}).error.empty());
    CHECK(call_game::actors[2].hp == 7);
    CHECK(raw_call(layer, "hurt_ref", std::array<RawValue, 1>{std::int64_t{2}}).error.empty());
    CHECK(call_game::actors[2].hp == 6);
    CHECK(std::get<bool>(raw_call(layer, "hp_of_or_none", std::array<RawValue, 2>{std::int64_t{-1}, std::int64_t{-1}}).values[0]));
    CHECK(std::get<std::int64_t>(raw_call(layer, "first_in_use", {}).values[0]) == 2);
    CHECK(std::get<std::int64_t>(raw_call(layer, "first_slot", {}).values[0]) == 0);
    call_game::actors[2].in_use = false;
    CHECK(std::get<std::int64_t>(raw_call(layer, "first_in_use", {}).values[0]) == -1);
}

TEST_CASE("Lua calls a function in on_between_updates and gets its results") {
    Mods mods;
    const ModInfo info = mods.between("g.call.add_hp(5) t.note(g.call.twice(21))\n"
                                      "g.call.move_to({x = 1, y = 2})\n"
                                      "t.note(g.call.position_now().y)\n"
                                      "local out = g.call.sum_into({x = 1, y = 2}, {x = 10, y = 20})\n"
                                      "t.note(out.x)\n"
                                      "local sum, doubled = g.call.double_in_place({x = 2, y = 3})\n"
                                      "t.note(sum) t.note(doubled.y)\n"
                                      "local pair = g.call.pair_now()\n"
                                      "t.note(pair[0]) t.note(#pair)\n"
                                      "g.call.take_pair(pair)\n");
    CHECK(info.error.empty());
    CHECK(info.state == ModState::Running);
    CHECK(notes == std::vector<std::string>{"42", "2", "11", "10", "6", "3", "1"});
    CHECK(call_game::hp == 7);
    CHECK(call_game::position.y == 2);
}

TEST_CASE("Lua sees a pool's element as its slot, and null as nil") {
    Mods mods;
    call_game::actors[1].in_use = true;
    const ModInfo info = mods.between("t.note(g.call.first_in_use())\n"
                                      "t.note(g.call.hp_of_or_none(nil, -1))\n"
                                      "g.call.hurt(1, 2)\n");
    CHECK(info.error.empty());
    CHECK(notes == std::vector<std::string>{"1", "true"});
    CHECK(call_game::actors[1].hp == -2);
    call_game::actors[1].in_use = false;
    mods.host->call_each(ModCallback::BetweenUpdates);
    CHECK(has(mods.host->mods().front().error, "that actor's slot is free"));
}

TEST_CASE("Lua's bad arguments stop the mod and name what was wrong") {
    const auto says = [](const std::string &body, std::string_view text) {
        Mods mods;
        const ModInfo info = mods.between(body);
        INFO(info.error);
        CHECK(info.state == ModState::Failed);
        CHECK(has(info.error, text));
        CHECK(call_game::calls == 0);
    };
    says("g.call.add_hp()", "takes 1 arguments, not 0");
    says("g.call.add_hp(1, 2)", "takes 1 arguments, not 2");
    says("g.call.add_hp(nil)", "needs an integer, not nil");
    says("g.call.add_hp({})", "needs an integer, not table");
    says("g.call.add_hp(99)", "out of range");
    says("g.call.add_hp(1.5)", "needs an integer");
    says("g.call.move_to(1)", "needs a table");
    says("g.call.move_to({x = 1})", "missing a part");
    says("g.call.move_to({x = 1000, y = 1})", "move_to(to).x");
    says("g.call.hurt(nil, 1)", "needs an integer, not nil");
    says("g.call.hurt(9, 1)", "out of range");
    says("g.call.take_pair({[0] = 1})", "missing a part");
}

TEST_CASE("another mod's function may change the game when called directly from on_between_updates") {
    const auto peer = [](const std::string &caller, ModCallback callback) {
        Mods mods;
        mods.run("b", "function exports.heal() g.call.add_hp(1) end");
        mods.run("a", caller);
        mods.host->call("a", callback);
        return mods.host->mods().back();
    };
    CHECK(peer("function on_between_updates() mods.get('b').heal() end", ModCallback::BetweenUpdates).state == ModState::Running);
    CHECK(call_game::calls == 1);

    const ModInfo from_update = peer("function on_update() mods.get('b').heal() end", ModCallback::Update);
    CHECK(from_update.state == ModState::Failed);
    CHECK(has(from_update.error, "only directly in on_between_updates"));
    CHECK(call_game::calls == 0);
    const ModInfo from_coroutine = peer("function on_between_updates() coroutine.wrap(function() mods.get('b').heal() end)() end",
                                        ModCallback::BetweenUpdates);
    CHECK(from_coroutine.state == ModState::Failed);
    CHECK(call_game::calls == 0);
}

TEST_CASE("a call that changes the game runs only directly in on_between_updates") {
    const auto refused = [](const std::string &script, ModCallback callback) {
        Mods mods;
        mods.run("a", script);
        mods.host->call_each(callback);
        const ModInfo info = mods.host->mods().front();
        INFO(info.error);
        CHECK(info.state == ModState::Failed);
        CHECK(has(info.error, "only directly in on_between_updates"));
        CHECK(call_game::calls == 0);
    };
    refused("function on_update() g.call.add_hp(1) end", ModCallback::Update);
    refused("function on_draw() g.call.add_hp(1) end", ModCallback::Draw);
    refused("function on_settings() g.call.add_hp(1) end", ModCallback::Settings);
    refused("function on_between_updates() coroutine.wrap(function() g.call.add_hp(1) end)() end",
            ModCallback::BetweenUpdates);

    Mods mods;
    mods.run("a", "g.call.add_hp(1)");
    CHECK(mods.host->mods().front().state == ModState::Failed);
    CHECK(call_game::calls == 0);
}

TEST_CASE("an event handler raised inside a call cannot make a call that changes the game") {
    Mods mods;
    mods.run("a", "function on_test(e) g.call.add_hp(1) end\n"
                  "function on_between_updates() g.call.raise_test() t.note('after') end");
    mods.host->call_each(ModCallback::BetweenUpdates);
    const ModInfo info = mods.host->mods().front();
    CHECK(info.state == ModState::Failed);
    CHECK(has(info.error, "only directly in on_between_updates"));
    CHECK(call_game::calls == 0);
    CHECK(notes.empty());
}

TEST_CASE("an AnyTime function may be called anywhere, even in a coroutine or a handler") {
    Mods mods;
    mods.run("a", "function on_test(e) t.note(g.call.twice(1)) end\n"
                  "function on_update() t.note(g.call.twice(2)) end\n"
                  "function on_draw() t.note(coroutine.wrap(function() return g.call.twice(3) end)()) end\n"
                  "function on_between_updates() g.call.raise_test() end\n"
                  "t.note(g.call.twice(4))");
    mods.host->call_each(ModCallback::Update);
    mods.host->call_each(ModCallback::Draw);
    mods.host->call_each(ModCallback::BetweenUpdates);
    CHECK(mods.host->mods().front().error.empty());
    CHECK(notes == std::vector<std::string>{"8", "4", "6", "2"});
}

TEST_CASE("each call is charged to the callback's budget") {
    Mods mods;   // 1,000,000 a callback, 100,000 a call
    const ModInfo info = mods.between("while true do pcall(g.call.twice, 1) end");
    CHECK(info.state == ModState::Failed);
    CHECK(has(info.error, "instruction budget"));
}

TEST_CASE("the budget lets a callback make its calls, and is whole for the next") {
    Mods mods;
    mods.between("for i = 1, 5 do g.call.add_hp(1) end");
    for (int i = 0; i < 5; ++i)
        mods.host->call_each(ModCallback::BetweenUpdates);
    CHECK(mods.host->mods().front().state == ModState::Running);
    CHECK(call_game::hp == 30);
}

TEST_CASE("a call whose event handler errs stops the mod after the call, with the call's effect kept") {
    Mods mods;
    mods.run("a", "function on_test(e) error('boom') end\n"
                  "function on_between_updates() g.call.raise_test() t.note('after') end");
    mods.host->call_each(ModCallback::BetweenUpdates);
    const ModInfo info = mods.host->mods().front();
    CHECK(info.state == ModState::Failed);
    CHECK(has(info.error, "boom"));
    CHECK(notes.empty());
    CHECK(ended == 1);
}

TEST_CASE("a function's own check sees its arguments together, before any game code runs") {
    reset_game();
    const std::array good{RawValue{std::int64_t{5}}, RawValue{std::int64_t{2}}};
    CHECK(raw_call(test_layer(), "add_then_subtract", good).error.empty());
    CHECK(call_game::hp == 3);
    const std::array bad{RawValue{std::int64_t{2}}, RawValue{std::int64_t{5}}};
    CHECK(has(raw_call(test_layer(), "add_then_subtract", bad).error, "first must be larger"));
    CHECK(call_game::hp == 3);
}

TEST_CASE("a call that runs frames runs each mod's on_update, which cannot change the game") {
    Mods mods;
    mods.run("a", "function on_update() t.note('a') end\n"
                  "function on_between_updates() g.call.run_frames(2) t.note('after') end");
    mods.run("b", "function on_update() t.note('b') end");
    mods.host->call_each(ModCallback::BetweenUpdates);
    CHECK(notes == std::vector<std::string>{"a", "b", "a", "b", "after"});
    CHECK(began == 0);   // the game's "no frame may be reached" is for other calls

    mods.run("a", "function on_update() g.call.add_hp(1) end\n"
                  "function on_between_updates() g.call.run_frames(1) end");
    mods.host->call_each(ModCallback::BetweenUpdates);
    CHECK(mods.host->mods().front().state == ModState::Failed);
    CHECK(has(mods.host->mods().front().error, "only directly in on_between_updates"));
    CHECK(call_game::hp == 0);
}

TEST_CASE("a call that runs frames is refused where one that changes the game is") {
    Mods mods;
    mods.run("a", "function on_update() g.call.run_frames(1) end");
    mods.host->call_each(ModCallback::Update);
    CHECK(mods.host->mods().front().state == ModState::Failed);
    CHECK(call_game::frames_run == 0);
}

TEST_CASE("each frame gives a whole budget, and the callback's own comes back after the call") {
    Mods mods;
    // Each frame's on_update spends most of the 1,000,000: without the refill the third would fail.
    mods.run("a", "function on_update() for i = 1, 150000 do end end\n"
                  "function on_between_updates() g.call.run_frames(20) t.note('done') end");
    mods.host->call_each(ModCallback::BetweenUpdates);
    CHECK(mods.host->mods().front().state == ModState::Running);
    CHECK(notes == std::vector<std::string>{"done"});

    // The callback's own loop is not refilled: it is charged for each call and runs out.
    mods.run("a", "function on_between_updates() while true do g.call.run_frames(1) end end");
    mods.host->call_each(ModCallback::BetweenUpdates);
    CHECK(mods.host->mods().front().state == ModState::Failed);
    CHECK(has(mods.host->mods().front().error, "over its instruction budget"));
    CHECK(call_game::frames_run == 1);
}

TEST_CASE("a mod that errs in its on_update during its call stops, and its frames go on") {
    Mods mods;
    mods.run("a", "function on_update() error('boom') end\n"
                  "function on_between_updates() g.call.run_frames(3) t.note('after') end");
    mods.host->call_each(ModCallback::BetweenUpdates);
    CHECK(call_game::frames_run == 3);
    CHECK(notes.empty());
    const ModInfo info = mods.host->mods().front();
    CHECK(info.state == ModState::Failed);
    CHECK(has(info.error, "boom"));
}

TEST_CASE("a mod that fails in a frame of another's call stops at once") {
    Mods mods;
    mods.run("a", "function on_between_updates() g.call.run_frames(2) t.note('after') end");
    mods.run("b", "function on_update() t.note('b') error('boom') end");
    mods.host->call_each(ModCallback::BetweenUpdates);
    CHECK(notes == std::vector<std::string>{"b", "after"});
    CHECK(mods.host->mods()[0].state == ModState::Running);
    CHECK(mods.host->mods()[1].state == ModState::Failed);
}

// During run_frames, asks the host for what the Mods panel and the scan can ask for.
static ModHost *asked = nullptr;

TEST_CASE("nothing unloads a mod during a call that runs frames, and what was asked happens after") {
    Mods mods;
    asked = mods.host.get();
    mods.run("a", "function on_update() t.note('update') end\n"
                  "function on_unload() t.note('unload') end\n"
                  "function on_between_updates() g.call.run_frames(1) t.note('after') end");
    call_game::during_frame = [] {
        asked->disable("a");
        asked->reload_all();
        asked->scan();
    };
    fs::remove_all(mods.dir / "a");   // the scan would remove it, closing the state in use
    mods.host->call_each(ModCallback::BetweenUpdates);
    CHECK(notes == std::vector<std::string>{"update", "after", "unload"});
    CHECK(mods.host->mods().front().state == ModState::Disabled);
    CHECK_FALSE(mods.host->mods().front().enabled);
    call_game::during_frame = nullptr;
    asked = nullptr;
}

TEST_CASE("the definitions file lists each function with its parameters, results and when") {
    const std::string text = raw_call_definitions(test_layer());
    CHECK(has(text, "---The test's functions."));
    CHECK(has(text, "---Adds to the HP.\n---Only directly in on_between_updates.\n---@param delta integer (-50 to 50)\nfunction g.call.add_hp(delta) end"));
    CHECK(has(text, "---May be called anywhere.\n---@param value integer"));
    CHECK(has(text, "---Only directly in on_between_updates. Runs frames until it is done.\n---@param count integer"));
    CHECK(has(text, "---@return integer\nfunction g.call.twice(value) end"));
    CHECK(has(text, "---@param a g.call.Vector\n---@param b g.call.Vector\n---@return g.call.Vector\nfunction g.call.sum_into(a, b) end"));
    CHECK(has(text, "---@return integer, g.call.Vector\nfunction g.call.double_in_place(value) end"));
    CHECK(has(text, "---@param actor integer? (0 to 3) nil for none"));
    CHECK(has(text, "---@return integer?\nfunction g.call.first_in_use() end"));
    CHECK(has(text, "---@class g.call.Vector\n---@field x integer (-100 to 100)"));
    CHECK(has(text, "---MODE_ON = 1\n---@alias g.call.Mode integer"));

    Mods mods;
    CHECK(has(mods.host->definitions(), "function g.call.add_hp(delta) end"));
}
