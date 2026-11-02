#include <verdite/mods.h>
#include <verdite/raw_bind.h>
#include <verdite/raw_call_bind.h>
#include <verdite/raw_hook.h>
#include <verdite/raw_hook_bind.h>

#include <doctest/doctest.h>

#include <lauxlib.h>
#include <lua.h>

#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace verdite;
namespace fs = std::filesystem;

// A small game whose functions are routed through hook points, as the generated source does.
namespace hook_game {

struct Vector {
    std::int32_t x = 0, y = 0;
};
struct Actor {
    std::int32_t hp = 0;
    bool in_use = false;
};

std::array<Actor, 4> actors;
Actor outside_the_pool;
std::int32_t hp = 0, game_calls = 0, last_amount = 0, other_calls = 0;
const Vector *last_pointer = nullptr;
ModHost *host = nullptr;

void game_add_hp(std::int32_t amount) {
    hp += amount;
    last_amount = amount;
    ++game_calls;
}
std::int32_t game_scale(std::int32_t value) {
    ++game_calls;
    return value * 2;
}
std::int32_t game_unranged(std::int32_t value) {
    return value + 1;
}
void game_sum_into(const Vector *a, const Vector &b, Vector *out) {
    last_pointer = a;
    out->x = a->x + b.x;
    out->y = a->y + b.y;
}
std::int32_t game_double_in_place(Vector &value) {
    value.x *= 2;
    value.y *= 2;
    return value.x + value.y;
}
void game_hurt(Actor *actor, std::int32_t amount) {
    if (actor)
        actor->hp -= amount;
    ++game_calls;
}
Actor *game_first_in_use() {
    for (Actor &actor : actors)
        if (actor.in_use)
            return &actor;
    return nullptr;
}
// The same signature as first_in_use, but its callers take its result as never null, unless it is.
Actor *game_never_null() {
    return game_first_in_use();
}
std::int32_t frames_run = 0;
void (*during_frame)() = nullptr;
void game_run_frames(std::int32_t count) {
    for (std::int32_t i = 0; i < count; ++i) {
        host->frame_passed();
        host->call_each(ModCallback::Update);
        if (during_frame)
            during_frame();
        ++frames_run;
    }
}

} // namespace hook_game

template <> struct verdite::RawTypeOf<hook_game::Vector> {
    static const RawType &type() {
        static const std::array fields{
            raw_field<hook_game::Vector, [](hook_game::Vector &v) -> auto & { return v.x; }>("x", [] {
                RawLimits limits;
                limits.range = RawRange{-100, 100};
                return limits;
            }()),
            raw_field<hook_game::Vector, [](hook_game::Vector &v) -> auto & { return v.y; }>("y"),
        };
        static const RawType described = raw_struct_type("Vector", fields);
        return described;
    }
};
template <> struct verdite::RawPoolOf<hook_game::Actor> {
    static std::span<hook_game::Actor> pool() { return hook_game::actors; }
};

namespace {

enum Point : std::size_t { AddHp, Scale, Unranged, SumInto, DoubleInPlace, Hurt, FirstInUse, NeverNull, RunFrames, WatchOnly, PointCount };

std::array<std::atomic<bool>, PointCount> active{};
RawHookDispatch dispatch;

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
std::string_view actor_in_use(std::int64_t slot) {
    return hook_game::actors[static_cast<std::size_t>(slot)].in_use ? std::string_view{} : "that actor's slot is free";
}
RawParamListing actor_param(std::string_view name) {
    RawParamListing listing = param(name);
    listing.check = actor_in_use;
    listing.nil_passes_null = true;
    return listing;
}
RawParamListing actor_result() {
    RawParamListing listing;
    listing.check = actor_in_use;
    listing.nil_passes_null = true;
    return listing;
}
RawParamListing actor_result_not_nil() {
    RawParamListing listing;
    listing.check = actor_in_use;
    return listing;
}
RawFunctionListing runs_frames() {
    RawFunctionListing listing;
    listing.when = RawCallWhen::RunsFrames;
    return listing;
}

const RawHookLayer &test_hooks() {
    using namespace hook_game;
    using A0 = std::array<RawParamListing, 0>;
    static const std::array points{
        raw_hook_point<&game_add_hp>(raw_function<&game_add_hp>("add_hp", std::array{range_param("amount", -50, 50)}),
                                     RawHookReach::Replace),
        raw_hook_point<&game_scale>(raw_function<&game_scale>("scale", std::array{range_param("value", 0, 100)}),
                                    RawHookReach::Replace, range_param("", 0, 1000)),
        raw_hook_point<&game_unranged>(raw_function<&game_unranged>("unranged", std::array{range_param("value", 0, 9)}),
                                       RawHookReach::ChangeArguments),
        raw_hook_point<&game_sum_into>(raw_function<&game_sum_into>("sum_into", std::array{param("a"), param("b"),
                                                                                           role_param("out", RawParamRole::Out)}),
                                       RawHookReach::Replace),
        raw_hook_point<&game_double_in_place>(
            raw_function<&game_double_in_place>("double_in_place", std::array{role_param("value", RawParamRole::InOut)}),
            RawHookReach::Replace),
        raw_hook_point<&game_hurt>(raw_function<&game_hurt>("hurt", std::array{actor_param("actor"), param("amount")}),
                                   RawHookReach::Replace),
        raw_hook_point<&game_first_in_use>(raw_function<&game_first_in_use>("first_in_use", A0{}), RawHookReach::Replace,
                                           actor_result()),
        raw_hook_point<&game_never_null>(raw_function<&game_never_null>("never_null", A0{}), RawHookReach::Replace,
                                         actor_result_not_nil()),
        raw_hook_point<&game_run_frames>(raw_function<&game_run_frames>("run_frames", std::array{param("count")}, runs_frames()),
                                         RawHookReach::Replace),
        raw_hook_point<&game_add_hp>(raw_function<&game_add_hp>("watch_only", std::array{range_param("amount", -50, 50)}),
                                     RawHookReach::Watch),
    };
    static const RawHookLayer layer{"g.hook", "The test's hooks.", points, active, &dispatch, 1000};
    return layer;
}

} // namespace

// What the generated source writes for each function.
namespace hook_game {

void add_one();

template <auto function, typename... Given> decltype(auto) routed(std::size_t index, Given &&...given) {
    if (!active[index].load(std::memory_order_relaxed))
        return function(std::forward<Given>(given)...);
    return raw_hook_fire<function>(test_hooks(), index, std::forward<Given>(given)...);
}

void add_hp(std::int32_t amount) { routed<&game_add_hp>(AddHp, amount); }
std::int32_t scale(std::int32_t value) { return routed<&game_scale>(Scale, value); }
std::int32_t unranged(std::int32_t value) { return routed<&game_unranged>(Unranged, value); }
void sum_into(const Vector *a, const Vector &b, Vector *out) { routed<&game_sum_into>(SumInto, a, b, out); }
std::int32_t double_in_place(Vector &value) { return routed<&game_double_in_place>(DoubleInPlace, value); }
void hurt(Actor *actor, std::int32_t amount) { routed<&game_hurt>(Hurt, actor, amount); }
Actor *first_in_use() { return routed<&game_first_in_use>(FirstInUse); }
Actor *never_null() { return routed<&game_never_null>(NeverNull); }
void run_frames(std::int32_t count) { routed<&game_run_frames>(RunFrames, count); }
void watch_only(std::int32_t amount) { routed<&game_add_hp>(WatchOnly, amount); }
void add_one() { ++other_calls; }

} // namespace hook_game

namespace {

const RawCallLayer &test_calls() {
    static const std::array functions{
        raw_function<&hook_game::add_one>("add_one", std::array<RawParamListing, 0>{}),
    };
    static const RawCallLayer layer{"g.call", "", functions, 1000, nullptr, nullptr, 1000};
    return layer;
}

std::vector<std::string> notes;
int note(lua_State *L) {
    notes.emplace_back(luaL_tolstring(L, 1, nullptr));
    return 0;
}
// A native function of the game's API that reaches a hooked game function, as curated ones do.
int bound_add_hp(lua_State *L) {
    hook_game::add_hp(static_cast<std::int32_t>(luaL_checkinteger(L, 1)));
    return 0;
}

struct Mods {
    fs::path dir;
    std::unique_ptr<ModHost> host;

    explicit Mods(std::uint64_t budget = 1'000'000) {
        static int count;
        dir = fs::temp_directory_path() / ("verdite-hooks-" + std::to_string(::getpid()) + "-" + std::to_string(++count));
        fs::remove_all(dir);
        hook_game::actors = {};
        hook_game::hp = hook_game::game_calls = hook_game::last_amount = hook_game::frames_run = 0;
        hook_game::last_pointer = nullptr;
        notes.clear();
        ModHostConfig config;
        config.mods_dir = dir;
        config.api_version = "9.9";
        config.instructions_per_call = budget;
        config.hooks = test_hooks();
        config.calls = test_calls();
        config.read_stored = [](std::string_view) { return std::optional<ModStored>{}; };
        config.api = {ModTable{"t", "", {{"note", "fun(v: any)", "Notes v.", note}, {"add_hp", "fun(a: integer)", "Adds.", bound_add_hp}}, {}}};
        host = std::make_unique<ModHost>(std::move(config));
        hook_game::host = host.get();
    }
    ~Mods() {
        host.reset();
        hook_game::host = nullptr;
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
        INFO(error_of(id));
    }
    [[nodiscard]] std::string error_of(const std::string &id) const {
        for (const ModInfo &mod : host->mods())
            if (mod.id == id)
                return mod.error;
        return "no such mod";
    }
};

} // namespace

TEST_CASE("a function with no hook runs the game's own and fires nothing") {
    Mods mods;
    mods.run("a", "");
    CHECK_FALSE(active[AddHp].load());
    hook_game::add_hp(3);
    CHECK(hook_game::hp == 3);
}

TEST_CASE("before sees the arguments, and may change them") {
    Mods mods;
    mods.run("a", "g.hook.before.add_hp(function(amount) t.note(amount); return amount * 2 end)");
    CHECK(active[AddHp].load());
    hook_game::add_hp(5);
    CHECK(notes == std::vector<std::string>{"5"});
    CHECK(hook_game::hp == 10);
}

TEST_CASE("before returning nothing leaves the call as it was") {
    Mods mods;
    mods.run("a", "g.hook.before.add_hp(function(amount) end)");
    hook_game::add_hp(5);
    CHECK(hook_game::hp == 5);
    CHECK(mods.error_of("a").empty());
}

TEST_CASE("a changed argument is checked, and a mod that errs stops with the game's call still made") {
    Mods mods;
    mods.run("a", "g.hook.before.add_hp(function(amount) return 51 end)");
    hook_game::add_hp(5);
    CHECK(hook_game::hp == 5);
    CHECK(mods.error_of("a").find("add_hp") != std::string::npos);
    CHECK_FALSE(active[AddHp].load());
}

TEST_CASE("the game's own value outside the range passes back unchanged") {
    Mods mods;
    mods.run("a", "g.hook.before.add_hp(function(amount) return amount end)\n"
                  "g.hook.replace.add_hp(function(original, amount) original(amount) end)");
    hook_game::add_hp(500);
    CHECK(hook_game::hp == 500);
    CHECK(mods.error_of("a").empty());
}

TEST_CASE("after gets the results and the arguments, and cannot change them") {
    Mods mods;
    mods.run("a", "g.hook.after.scale(function(result, value) t.note(result .. ':' .. value); return 999 end)");
    CHECK(hook_game::scale(4) == 8);
    CHECK(notes == std::vector<std::string>{"8:4"});
    CHECK(mods.error_of("a").empty());
}

TEST_CASE("a replacement runs instead, and its result is the caller's") {
    Mods mods;
    mods.run("a", "g.hook.replace.scale(function(original, value) return 7 end)");
    CHECK(hook_game::scale(4) == 7);
    CHECK(hook_game::game_calls == 0);
}

TEST_CASE("a replacement may run the original") {
    Mods mods;
    mods.run("a", "g.hook.replace.scale(function(original, value) return original(value + 1) + 1 end)");
    CHECK(hook_game::scale(4) == 11);
    CHECK(hook_game::game_calls == 1);
}

TEST_CASE("a replacement's result is checked: on an error the mod stops and the game's function runs") {
    Mods mods;
    mods.run("a", "g.hook.replace.scale(function(original, value) return 1001 end)");
    CHECK(hook_game::scale(4) == 8);
    CHECK(hook_game::game_calls == 1);
    CHECK(mods.error_of("a").find("scale") != std::string::npos);
}

TEST_CASE("a replacement returning the wrong count stops, and the game's function runs") {
    for (const char *const script : {"g.hook.replace.scale(function(original, value) end)",
                                     "g.hook.replace.scale(function(original, value) return 1, 2 end)",
                                     "g.hook.replace.scale(function(original, value) return 'x' end)"}) {
        Mods mods;
        mods.run("a", script);
        CHECK(hook_game::scale(4) == 8);
        CHECK_FALSE(mods.error_of("a").empty());
    }
}

TEST_CASE("a replacement that errs after running the original keeps what happened") {
    Mods mods;
    mods.run("a", "g.hook.replace.add_hp(function(original, amount) original(amount); error('late') end)");
    hook_game::add_hp(5);
    CHECK(hook_game::hp == 5);
    CHECK(hook_game::game_calls == 1);
    CHECK(mods.error_of("a").find("late") != std::string::npos);
}

TEST_CASE("the original's own value may be handed on, though out of a replacement's limits") {
    Mods mods;
    mods.run("a", "g.hook.replace.scale(function(original, value) return original(value) end)");
    CHECK(hook_game::scale(800) == 1600);
    CHECK(mods.error_of("a").empty());
}

TEST_CASE("original cannot be kept past its replacement") {
    Mods mods;
    mods.run("a", "local kept\n"
                  "g.hook.replace.add_hp(function(original, amount) kept = original; original(amount) end)\n"
                  "function on_update() if kept then kept(1) end end");
    hook_game::add_hp(5);
    mods.host->call_each(ModCallback::Update);
    CHECK(hook_game::hp == 5);
    CHECK(mods.error_of("a").find("while its replacement runs") != std::string::npos);
}

TEST_CASE("original cannot run in a coroutine") {
    Mods mods;
    mods.run("a", "g.hook.replace.add_hp(function(original, amount)\n"
                  "  local co = coroutine.create(function() original(amount) end)\n"
                  "  local ok, why = coroutine.resume(co)\n"
                  "  t.note(tostring(ok))\n"
                  "  original(amount)\n"
                  "end)");
    hook_game::add_hp(5);
    CHECK(notes == std::vector<std::string>{"false"});
    CHECK(hook_game::hp == 5);
    CHECK(mods.error_of("a").empty());
}

TEST_CASE("a handler may not make a call that changes state, as an event's may not") {
    Mods mods;
    mods.run("a", "g.hook.before.add_hp(function(amount) g.call.add_one() end)\n"
                  "function on_between_updates() t.note('in callback'); g.call.add_one(); t.note('called') end");
    mods.host->call_each(ModCallback::BetweenUpdates);
    CHECK(notes == std::vector<std::string>{"in callback", "called"});
    hook_game::hp = 0;
    hook_game::add_hp(5);
    CHECK(hook_game::hp == 5);
    CHECK(mods.error_of("a").find("on_between_updates") != std::string::npos);

    Mods nested;
    nested.run("a", "g.hook.before.add_hp(function(amount) g.call.add_one() end)\n"
                    "function on_between_updates() t.add_hp(2) end");
    nested.host->call_each(ModCallback::BetweenUpdates);
    CHECK(nested.error_of("a").find("on_between_updates") != std::string::npos);
}

TEST_CASE("mods run in order: before first to last, replacements nested, after last to first") {
    Mods mods;
    const auto script = [](const std::string &name) {
        return "g.hook.before.add_hp(function(a) t.note('" + name + " before ' .. a); return a + 1 end)\n"
               "g.hook.replace.add_hp(function(original, a) t.note('" + name + " in ' .. a); original(a + 10); t.note('" + name + " out') end)\n"
               "g.hook.after.add_hp(function(a) t.note('" + name + " after ' .. a) end)\n";
    };
    mods.run("a", script("a"));
    mods.run("b", script("b"));
    hook_game::add_hp(1);
    CHECK(notes == std::vector<std::string>{"a before 1", "b before 2", "a in 3", "b in 13", "b out", "a out", "b after 3", "a after 3"});
    CHECK(hook_game::hp == 23);
}

TEST_CASE("a failing mod in a chain is passed over") {
    Mods mods;
    mods.run("a", "g.hook.replace.add_hp(function(original, a) error('no') end)");
    mods.run("b", "g.hook.replace.add_hp(function(original, a) t.note('b'); original(a * 3) end)");
    hook_game::add_hp(2);
    CHECK(notes == std::vector<std::string>{"b"});
    CHECK(hook_game::hp == 6);
    CHECK_FALSE(mods.error_of("a").empty());
    CHECK(mods.error_of("b").empty());
}

TEST_CASE("nil removes a mod's handler, and a mod's unload removes them all") {
    Mods mods;
    mods.run("a", "g.hook.before.add_hp(function(a) t.note(a) end)\n"
                  "function on_update() g.hook.before.add_hp(nil) end");
    CHECK(active[AddHp].load());
    mods.host->call_each(ModCallback::Update);
    CHECK_FALSE(active[AddHp].load());
    hook_game::add_hp(1);
    CHECK(notes.empty());

    mods.run("b", "g.hook.before.add_hp(function(a) t.note(a) end)");
    CHECK(active[AddHp].load());
    mods.host->disable("b");
    CHECK_FALSE(active[AddHp].load());
}

TEST_CASE("a mod enabled when first found has its hooks from the start") {
    Mods mods;
    const fs::path folder = mods.dir / "a";
    fs::create_directories(folder);
    std::ofstream(folder / "mod.json") << R"({"id":"a","name":"a","default_enabled":true})";
    std::ofstream(folder / "main.lua") << "g.hook.before.add_hp(function(a) t.note(a) end)";
    mods.host->scan();
    CHECK(active[AddHp].load());
    hook_game::add_hp(1);
    CHECK(notes.size() == 1);
}

TEST_CASE("a finalizer setting a hook as the state closes does nothing") {
    Mods mods;
    mods.run("a", "setmetatable({}, {__gc = function() g.hook.before.add_hp(function(a) end) end})\n"
                  "collectgarbage('stop')");
    mods.host->disable("a");
    CHECK_FALSE(active[AddHp].load());
    hook_game::add_hp(1);
    CHECK(hook_game::hp == 1);
}

TEST_CASE("a held mod's hooks do not fire until it is released") {
    Mods mods;
    const fs::path path = mods.dir / "a" / "mod.json";
    fs::create_directories(path.parent_path());
    std::ofstream(path) << R"({"id":"a","name":"a","enhancement":true})";
    mods.run("a", "g.hook.before.add_hp(function(a) t.note(a) end)");
    mods.host->hold_enhancements();
    CHECK_FALSE(active[AddHp].load());
    hook_game::add_hp(1);
    CHECK(notes.empty());
    mods.host->release_enhancements();
    CHECK(active[AddHp].load());
    hook_game::add_hp(1);
    CHECK(notes.size() == 1);
}

TEST_CASE("Watch: a before handler that changes the arguments stops") {
    Mods mods;
    mods.run("a", "g.hook.before.watch_only(function(a) return a + 1 end)");
    hook_game::watch_only(1);
    CHECK(hook_game::hp == 1);
    CHECK(mods.error_of("a").find("may only be watched") != std::string::npos);
}

TEST_CASE("Watch and ChangeArguments have no replace") {
    Mods mods;
    mods.run("a", "t.note(tostring(g.hook.replace.watch_only)); t.note(tostring(g.hook.replace.unranged))\n"
                  "g.hook.before.unranged(function(v) return v + 1 end)");
    CHECK(notes == std::vector<std::string>{"nil", "nil"});
    CHECK(hook_game::unranged(1) == 3);
}

TEST_CASE("structs by pointer: the caller's own runs unless changed, and an Out comes back") {
    Mods mods;
    mods.run("a", "g.hook.after.sum_into(function(out, a, b) t.note(out.x .. ',' .. out.y .. ',' .. a.x .. ',' .. b.y) end)");
    const hook_game::Vector a{1, 2}, b{3, 4};
    hook_game::Vector out;
    hook_game::sum_into(&a, b, &out);
    CHECK(out.x == 4);
    CHECK(out.y == 6);
    CHECK(hook_game::last_pointer == &a);
    CHECK(notes == std::vector<std::string>{"4,6,1,4"});
}

TEST_CASE("a replacement fills an Out and an InOut") {
    Mods mods;
    mods.run("a", "g.hook.replace.sum_into(function(original, a, b) return {x = 9, y = 8} end)\n"
                  "g.hook.replace.double_in_place(function(original, v) return 5, {x = v.x + 1, y = 0} end)");
    const hook_game::Vector a{1, 2}, b{3, 4};
    hook_game::Vector out;
    hook_game::sum_into(&a, b, &out);
    CHECK(out.x == 9);
    CHECK(out.y == 8);
    hook_game::Vector value{10, 20};
    CHECK(hook_game::double_in_place(value) == 5);
    CHECK(value.x == 11);
    CHECK(value.y == 0);
}

TEST_CASE("a changed struct argument is checked by field, and runs on a copy") {
    Mods mods;
    mods.run("a", "g.hook.before.sum_into(function(a, b) return {x = 50, y = 1}, b end)");
    const hook_game::Vector a{1, 2}, b{3, 4};
    hook_game::Vector out;
    hook_game::sum_into(&a, b, &out);
    CHECK(out.x == 53);
    CHECK(hook_game::last_pointer != &a);

    Mods other;
    other.run("a", "g.hook.before.sum_into(function(a, b) return {x = 101, y = 1}, b end)");
    hook_game::sum_into(&a, b, &out);
    CHECK(out.x == 4);
    CHECK(other.error_of("a").find('x') != std::string::npos);
}

TEST_CASE("a call the layer cannot describe skips the hooks") {
    Mods mods;
    mods.run("a", "g.hook.before.hurt(function(a, b) t.note('fired') end)\n"
                  "g.hook.before.sum_into(function(a, b) t.note('fired') end)");
    hook_game::outside_the_pool.hp = 10;
    hook_game::hurt(&hook_game::outside_the_pool, 3);
    CHECK(hook_game::outside_the_pool.hp == 7);
    CHECK(notes.empty());
}

TEST_CASE("pool elements cross as slots; null is nil; a free slot in a change is refused") {
    Mods mods;
    hook_game::actors[1] = {10, true};
    hook_game::actors[2] = {10, true};
    mods.run("a", "g.hook.before.hurt(function(actor, amount) t.note(tostring(actor)); if actor then return 2, amount end end)");
    hook_game::hurt(&hook_game::actors[1], 3);
    CHECK(hook_game::actors[1].hp == 10);
    CHECK(hook_game::actors[2].hp == 7);
    hook_game::hurt(nullptr, 3);
    CHECK(notes == std::vector<std::string>{"1", "nil"});
    CHECK(mods.error_of("a").empty());

    Mods other;
    other.run("a", "g.hook.before.hurt(function(actor, amount) return 3, amount end)");
    hook_game::actors[3] = {};
    hook_game::actors[1] = {10, true};
    hook_game::hurt(&hook_game::actors[1], 3);
    CHECK(hook_game::actors[1].hp == 7);
    CHECK(other.error_of("a").find("free") != std::string::npos);
}

TEST_CASE("a replacement returning a pool's element: a slot in use or nil") {
    {
        Mods mods;
        hook_game::actors[2] = {10, true};
        mods.run("a", "g.hook.replace.first_in_use(function(original) return 2 end)");
        CHECK(hook_game::first_in_use() == &hook_game::actors[2]);
    }
    {
        Mods mods;
        hook_game::actors[2] = {10, true};
        mods.run("a", "g.hook.replace.first_in_use(function(original) return nil end)");
        CHECK(hook_game::first_in_use() == nullptr);
    }
    {
        Mods mods;
        hook_game::actors[2] = {10, true};
        mods.run("a", "g.hook.replace.first_in_use(function(original) return 3 end)");
        CHECK(hook_game::first_in_use() == &hook_game::actors[2]);
        CHECK(mods.error_of("a").find("free") != std::string::npos);
    }
    {
        Mods mods;
        hook_game::actors[2] = {10, true};
        mods.run("a", "g.hook.replace.first_in_use(function(original) return 99 end)");
        CHECK(hook_game::first_in_use() == &hook_game::actors[2]);
        CHECK_FALSE(mods.error_of("a").empty());
    }
}

TEST_CASE("a replacement may return null only where it is listed, or where the original gave it") {
    {
        Mods mods;
        hook_game::actors[2] = {10, true};
        mods.run("a", "g.hook.replace.never_null(function(original) return nil end)");
        CHECK(hook_game::never_null() == &hook_game::actors[2]);
        CHECK_FALSE(mods.error_of("a").empty());
    }
    {
        Mods mods;
        mods.run("a", "g.hook.replace.never_null(function(original) return original() end)");
        CHECK(hook_game::never_null() == nullptr);
        CHECK(mods.error_of("a").empty());
    }
}

TEST_CASE("a hook firing from a bound function inside a coroutine works") {
    Mods mods;
    mods.run("a", "g.hook.before.add_hp(function(a) t.note('hook ' .. a) end)\n"
                  "function on_update()\n"
                  "  local co = coroutine.wrap(function() t.add_hp(2); coroutine.yield(); t.add_hp(3) end)\n"
                  "  co(); co()\n"
                  "end");
    mods.host->call_each(ModCallback::Update);
    CHECK(notes == std::vector<std::string>{"hook 2", "hook 3"});
    CHECK(hook_game::hp == 5);
    CHECK(mods.error_of("a").empty());
}

TEST_CASE("a mod's error in a handler below its own callback stops it once the callback returns") {
    Mods mods;
    mods.run("a", "g.hook.before.add_hp(function(a) error('below') end)\n"
                  "function on_update() t.add_hp(2); t.note('after the call') end");
    mods.host->call_each(ModCallback::Update);
    CHECK(hook_game::hp == 2);
    CHECK(notes.empty());
    CHECK(mods.error_of("a").find("below") != std::string::npos);
}

TEST_CASE("handlers share a budget a frame, so a hot hook cannot stall the game for long") {
    Mods mods(100'000);
    mods.run("a", "g.hook.before.add_hp(function(a) local x = 0; for i = 1, 10000 do x = x + i end end)");
    for (int i = 0; i < 1000 && mods.error_of("a").empty(); ++i)
        hook_game::add_hp(1);
    CHECK(mods.error_of("a").find("budget") != std::string::npos);

    Mods fresh(100'000);
    fresh.run("a", "g.hook.before.add_hp(function(a) local x = 0; for i = 1, 3000 do x = x + i end end)");
    for (int i = 0; i < 50; ++i) {
        hook_game::add_hp(1);
        fresh.host->frame_passed();
    }
    CHECK(fresh.error_of("a").empty());
}

TEST_CASE("a replacement running frames keeps the state open, and scan waits") {
    Mods mods;
    mods.run("a", "local seen = 0\n"
                  "g.hook.replace.run_frames(function(original, count) original(count); t.note('seen ' .. seen) end)\n"
                  "function on_update() seen = seen + 1 end");
    hook_game::run_frames(3);
    CHECK(hook_game::frames_run == 3);
    CHECK(notes == std::vector<std::string>{"seen 3"});
    CHECK(mods.error_of("a").empty());
}

TEST_CASE("a watched function running frames leaves the host free to reload mods in them") {
    Mods mods;
    mods.run("a", "g.hook.before.run_frames(function(count) t.note('before') end)\n"
                  "g.hook.after.run_frames(function(count) t.note('after') end)");
    static Mods *in_frames;
    in_frames = &mods;
    hook_game::during_frame = [] {
        in_frames->host->scan();
        in_frames->run("b", "t.note('b loaded')");
        in_frames->host->disable("a");
    };
    hook_game::run_frames(1);
    hook_game::during_frame = nullptr;
    CHECK(notes == std::vector<std::string>{"before", "b loaded"});
    CHECK_FALSE(active[RunFrames].load());
}

TEST_CASE("a mod failing in a frame under its own replacement stops when the call returns") {
    Mods mods;
    mods.run("a", "g.hook.replace.run_frames(function(original, count) original(count); t.note('after') end)\n"
                  "function on_update() error('in a frame') end");
    hook_game::run_frames(2);
    CHECK(hook_game::frames_run >= 1);
    CHECK(notes.empty());
    CHECK(mods.error_of("a").find("in a frame") != std::string::npos);
    CHECK_FALSE(active[RunFrames].load());
}

TEST_CASE("a hook does not fire off the host's thread") {
    Mods mods;
    mods.run("a", "g.hook.before.add_hp(function(a) t.note(a) end)");
    std::thread([] { hook_game::add_hp(4); }).join();
    CHECK(notes.empty());
    CHECK(hook_game::hp == 4);
}

TEST_CASE("the wrong handler type is an error") {
    Mods mods;
    mods.run("a", "g.hook.before.add_hp(1)");
    CHECK(mods.error_of("a").find("one function") != std::string::npos);
}

TEST_CASE("the definitions file lists each kind a point's reach allows") {
    Mods mods;
    const std::string text = mods.host->definitions();
    CHECK(text.find("function g.hook.before.watch_only(handler) end") != std::string::npos);
    CHECK(text.find("function g.hook.replace.watch_only(handler) end") == std::string::npos);
    CHECK(text.find("function g.hook.replace.scale(handler) end") != std::string::npos);
    CHECK(text.find("fun(original: fun(value: integer): integer, value: integer): integer") != std::string::npos);
}
