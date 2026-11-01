#include <verdite/mods.h>
#include <verdite/raw.h>
#include <verdite/raw_bind.h>

#include <doctest/doctest.h>

#include <lauxlib.h>
#include <lua.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace verdite;
namespace fs = std::filesystem;

namespace {

// Limits for the listed file's lines: a range, or a read-only reason with its doc.
RawLimits range_limit(std::int64_t min, std::int64_t max) {
    RawLimits limits;
    limits.range = RawRange{min, max};
    return limits;
}
RawLimits read_only_limit(std::string_view why, std::string_view doc) {
    RawLimits limits;
    limits.read_only = why;
    limits.doc = doc;
    return limits;
}

} // namespace

// A small game with every kind the raw layer describes. Its globals are the game.
namespace game {

enum class Mode : std::uint8_t { Off = 0, On = 1, Auto = 2 };
constexpr std::array<RawEnumValue, 3> mode_values{
    {{"MODE_OFF", 0}, {"MODE_ON", 1}, {"MODE_AUTO", 2}}};

enum class Bits : std::uint16_t { Lit = 1, Open = 2, Hidden = 8 };
constexpr std::array<RawEnumValue, 3> bits_values{
    {{"BIT_LIT", 1}, {"BIT_OPEN", 2}, {"BIT_HIDDEN", 8}}};

// An enum kept as a byte behind a wrapper, as KF1's KfEnumStorage keeps its enums.
class ModeStorage {
  public:
    ModeStorage() = default;
    ModeStorage(Mode mode) : value_(static_cast<std::uint8_t>(mode)) {}
    operator Mode() const {
        return static_cast<Mode>(value_);
    }
    ModeStorage &operator=(Mode mode) {
        value_ = static_cast<std::uint8_t>(mode);
        return *this;
    }

  private:
    std::uint8_t value_ = 0;
};

struct Vitals {
    std::int16_t current_hp = 0;
    std::uint16_t max_hp = 0;
};

struct Item {
    std::uint8_t id = 0;
    std::uint8_t count = 0;
};

struct Player {
    std::int8_t level = 0;
    std::uint8_t tag = 0;
    std::int32_t gold = 0;
    std::uint32_t xp = 0;
    std::int64_t big = 0;
    std::uint64_t huge = 0;
    bool alive = false;
    float speed = 0;
    double ratio = 0;
    Mode mode = Mode::Off;
    ModeStorage stored_mode;
    Bits flags{};
    std::array<char, 8> name{};
    Vitals vitals;
    std::array<Item, 3> items{};
    int slots[4] = {};
    std::array<std::array<std::uint8_t, 3>, 2> grid{};
    std::array<std::uint8_t, 4> tally{}; // limited to 0 to 9 by the listed file
    int capped = 0;                      // limited to 0 to 10
    int locked = 0;                      // read-only: held by the game
    const int version = 3;               // const in the decomp
};

Player player_state;
std::array<std::array<std::uint8_t, 6>, 3> item_stock{};
int ranged_level = 1; // limited to 1 to 5 by the listed file

} // namespace game

namespace verdite {

template <> struct RawTypeOf<game::Mode> {
    static const RawType &type() {
        static const RawType described = raw_enum_type<game::Mode>("Mode", game::mode_values);
        return described;
    }
};

// The same enum as a field stores it: through the wrapper.
template <> struct RawTypeOf<game::ModeStorage> {
    static const RawType &type() {
        static const RawType described =
            raw_enum_type<game::Mode, game::ModeStorage>("Mode", game::mode_values);
        return described;
    }
};

template <> struct RawTypeOf<game::Bits> {
    static const RawType &type() {
        static const RawType described = raw_flags_type<game::Bits>("Bits", game::bits_values);
        return described;
    }
};

template <> struct RawTypeOf<game::Vitals> {
    static const RawType &type() {
        static const std::array<RawField, 2> fields = {
            raw_field<game::Vitals, [](game::Vitals &v) -> auto & { return v.current_hp; }>(
                "current_hp"),
            raw_field<game::Vitals, [](game::Vitals &v) -> auto & { return v.max_hp; }>("max_hp")};
        static const RawType described = raw_struct_type("Vitals", fields);
        return described;
    }
};

template <> struct RawTypeOf<game::Item> {
    static const RawType &type() {
        static const std::array<RawField, 2> fields = {
            raw_field<game::Item, [](game::Item &i) -> auto & { return i.id; }>("id"),
            raw_field<game::Item, [](game::Item &i) -> auto & { return i.count; }>("count")};
        static const RawType described = raw_struct_type("Item", fields);
        return described;
    }
};

template <> struct RawTypeOf<game::Player> {
    static const RawType &type() {
        static const std::array<RawField, 21> fields = {
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.level; }>("level"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.tag; }>("tag"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.gold; }>("gold"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.xp; }>("xp"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.big; }>("big"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.huge; }>("huge"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.alive; }>("alive"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.speed; }>("speed"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.ratio; }>("ratio"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.mode; }>("mode"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.stored_mode; }>(
                "stored_mode"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.flags; }>("flags"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.name; }>("name"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.vitals; }>("vitals"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.items; }>("items"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.slots; }>("slots"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.grid; }>("grid"),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.tally; }>(
                "tally", range_limit(0, 9)),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.capped; }>(
                "capped", range_limit(0, 10)),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.locked; }>(
                "locked", read_only_limit("held by the game", "The stored score.")),
            raw_field<game::Player, [](game::Player &p) -> auto & { return p.version; }>("version"),
        };
        static const RawType described = raw_struct_type("Player", fields);
        return described;
    }
};

} // namespace verdite

namespace {

const std::array<RawGlobal, 3> game_globals = {
    raw_global<[]() -> auto & { return game::player_state; }>("player_state"),
    raw_global<[]() -> auto & { return game::item_stock; }>("item_stock"),
    raw_global<[]() -> auto & { return game::ranged_level; }>("ranged_level", range_limit(1, 5)),
};
const RawLayer game_layer{
    .path = "t.raw", .doc = "The test game's raw layer.", .globals = game_globals};

// A layer whose global clashes with the layer's own copy function.
const std::array<RawGlobal, 1> clashing_globals = {
    raw_global<[]() -> auto & { return game::ranged_level; }>("copy"),
};
const RawLayer clashing_layer{.path = "t.raw", .doc = "", .globals = clashing_globals};

// A flags value from a number at run time: a literal cast would be range-checked as an enum.
game::Bits bits_of(std::uint16_t value) {
    return std::bit_cast<game::Bits>(value);
}

// Every writable field to a known value, the way a game starts a test.
void reset_game() {
    game::Player &p = game::player_state;
    p.level = 5;
    p.tag = 7;
    p.gold = -1000;
    p.xp = 4000000000u;
    p.big = -5;
    p.huge = 6;
    p.alive = true;
    p.speed = 1.5f;
    p.ratio = 0.25;
    p.mode = game::Mode::On;
    p.stored_mode = game::Mode::Auto;
    p.flags = bits_of(3);
    p.name = {'a', 'b', 'c'};
    p.vitals.current_hp = 40;
    p.vitals.max_hp = 60;
    p.items[1] = {2, 9};
    p.slots[2] = 22;
    p.grid[1][2] = 7;
    p.tally = {1, 2, 3, 4};
    p.capped = 3;
    p.locked = 8;
    game::item_stock[2][5] = 5;
    game::ranged_level = 3;
}

std::string write_game(std::string_view path, const RawValue &value) {
    return raw_write(game_layer, path, value);
}
RawRead read_game(std::string_view path) {
    return raw_read(game_layer, path);
}

// The value at path; a missing one reads as its error, so a failing CHECK shows it.
RawValue value_at(std::string_view path) {
    const RawRead read = read_game(path);
    if (!read.value)
        return RawValue{read.error};
    return *read.value;
}

// The integer at path, or nothing if it is not one.
std::optional<std::int64_t> read_int(std::string_view path) {
    const RawValue value = value_at(path);
    if (const auto *integer = std::get_if<std::int64_t>(&value))
        return *integer;
    return std::nullopt;
}
std::string write_int(std::string_view path, std::int64_t value) {
    return write_game(path, RawValue{value});
}

// The mods' notes: what their test function t.note was given, in order.
std::vector<std::string> notes;

int note(lua_State *L) {
    notes.emplace_back(luaL_checkstring(L, 1));
    return 0;
}

// A mods folder and a host over it, with the test layer at t.raw (or another layer).
struct RawMods {
    fs::path dir;
    std::map<std::string, ModStored, std::less<>> stored;
    std::unique_ptr<ModHost> host;

    explicit RawMods(const RawLayer &layer = game_layer) {
        static int count;
        dir = fs::temp_directory_path() /
              ("verdite-raw-" + std::to_string(::getpid()) + "-" + std::to_string(++count));
        fs::remove_all(dir);
        notes.clear();
        ModHostConfig config;
        config.mods_dir = dir;
        config.api_version = "9.9";
        config.instructions_per_call = 1'000'000;
        config.api = {{.path = "t",
                       .doc = "The test's table.",
                       .functions = {{"note", "fun(text: string)", "Notes text.", note}},
                       .fields = {}}};
        config.raw = layer;
        config.read_stored = [this](std::string_view id) -> std::optional<ModStored> {
            const auto found = stored.find(id);
            if (found == stored.end())
                return std::nullopt;
            return found->second;
        };
        config.write_stored = [this](std::string_view id, const ModStored &now) {
            stored[std::string(id)] = now;
        };
        host = std::make_unique<ModHost>(std::move(config));
    }
    ~RawMods() {
        host.reset();
        std::error_code error;
        fs::remove_all(dir, error);
    }
    RawMods(const RawMods &) = delete;
    RawMods &operator=(const RawMods &) = delete;

    // Writes a file dated later than anything before it, so a scan sees the change.
    void write(const std::string &relative, const std::string &text) {
        static int seconds;
        const fs::path path = dir / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        fs::last_write_time(path,
                            fs::file_time_type::clock::now() + std::chrono::seconds(++seconds));
    }

    // A mod named id with main.lua holding script, found and enabled.
    void run(const std::string &id, const std::string &script) {
        write(id + "/main.lua", script);
        host->scan();
        host->enable(id);
    }

    [[nodiscard]] ModInfo info(std::string_view id) const {
        const auto list = host->mods();
        const auto found = std::find_if(list.begin(), list.end(),
                                        [&](const ModInfo &mod) { return mod.id == id; });
        REQUIRE(found != list.end());
        return *found;
    }
};

} // namespace

TEST_CASE("every integer width reads and writes") {
    reset_game();
    CHECK(write_int("player_state.level", -128).empty());
    CHECK(game::player_state.level == -128);
    CHECK(read_int("player_state.level") == -128);
    CHECK(write_int("player_state.tag", 255).empty());
    CHECK(game::player_state.tag == 255);
    CHECK(write_int("player_state.gold", std::numeric_limits<std::int32_t>::min()).empty());
    CHECK(read_int("player_state.gold") == std::numeric_limits<std::int32_t>::min());
    CHECK(write_int("player_state.xp", 4294967295).empty());
    CHECK(read_int("player_state.xp") == 4294967295);
    CHECK(write_int("player_state.big", std::numeric_limits<std::int64_t>::min()).empty());
    CHECK(read_int("player_state.big") == std::numeric_limits<std::int64_t>::min());
    CHECK(write_int("player_state.huge", 1234).empty());
    CHECK(read_int("player_state.huge") == 1234);
    CHECK(write_int("player_state.vitals.current_hp", -300).empty());
    CHECK(game::player_state.vitals.current_hp == -300);
    CHECK(write_int("player_state.vitals.max_hp", 65535).empty());
    CHECK(read_int("player_state.vitals.max_hp") == 65535);
    CHECK(write_int("player_state.slots[2]", -7).empty());
    CHECK(game::player_state.slots[2] == -7);
}

TEST_CASE("booleans, floats, doubles, enums, flags and strings read and write") {
    reset_game();
    CHECK(write_game("player_state.alive", RawValue{false}).empty());
    CHECK_FALSE(game::player_state.alive);
    CHECK(std::get<bool>(value_at("player_state.alive")) == false);
    CHECK(write_game("player_state.speed", RawValue{2.5}).empty());
    CHECK(std::get<double>(value_at("player_state.speed")) == 2.5);
    CHECK(write_int("player_state.speed", 2).empty()); // a whole number is a float too
    CHECK(game::player_state.speed == 2.0f);
    CHECK(write_game("player_state.ratio", RawValue{0.125}).empty());
    CHECK(game::player_state.ratio == 0.125);
    CHECK(write_int("player_state.mode", 2).empty());
    CHECK(game::player_state.mode == game::Mode::Auto);
    CHECK(read_int("player_state.mode") == 2);
    CHECK(write_int("player_state.stored_mode", 0).empty());
    CHECK(game::player_state.stored_mode == game::Mode::Off);
    CHECK(read_int("player_state.stored_mode") == 0);
    CHECK(write_int("player_state.flags", 10).empty());
    CHECK(game::player_state.flags == bits_of(10));
    CHECK(write_game("player_state.name", RawValue{std::string("hi")}).empty());
    CHECK(game::player_state.name[0] == 'h');
    CHECK(game::player_state.name[2] == '\0');
    CHECK(std::get<std::string>(value_at("player_state.name")) == "hi");
}

TEST_CASE("nested structs, arrays of structs, C arrays and nested arrays read and write") {
    reset_game();
    CHECK(write_int("player_state.items[1].count", 200).empty());
    CHECK(game::player_state.items[1].count == 200);
    CHECK(read_int("player_state.items[1].id") == 2);
    CHECK(write_int("player_state.grid[1][2]", 9).empty());
    CHECK(game::player_state.grid[1][2] == 9);
    CHECK(write_int("item_stock[1][5]", 9).empty());
    CHECK(game::item_stock[1][5] == 9);
    CHECK(read_int("item_stock[2][5]") == 5);
    CHECK(read_int("player_state.slots[2]") == 22);
}

TEST_CASE("a global with a range reads and writes within it") {
    reset_game();
    CHECK(write_int("ranged_level", 5).empty());
    CHECK(game::ranged_level == 5);
    CHECK(read_int("ranged_level") == 5);
}

TEST_CASE("an integer out of its range is refused, and nothing is written") {
    reset_game();
    CHECK(write_int("player_state.level", 200) ==
          "200 is out of range for player_state.level (-128 to 127)");
    CHECK(game::player_state.level == 5);
    CHECK(write_int("player_state.capped", 11) ==
          "11 is out of range for player_state.capped (0 to 10)");
    CHECK(game::player_state.capped == 3);
    CHECK(write_int("ranged_level", 6) == "6 is out of range for ranged_level (1 to 5)");
    CHECK(game::ranged_level == 3);
    CHECK(write_int("player_state.xp", -1) ==
          "-1 is out of range for player_state.xp (0 to 4294967295)");
}

TEST_CASE("a value of the wrong kind is refused") {
    reset_game();
    CHECK(write_game("player_state.gold", RawValue{std::string("x")}) ==
          "player_state.gold needs an integer, not a string");
    CHECK(write_int("player_state.alive", 1) ==
          "player_state.alive needs a boolean, not an integer");
    CHECK(write_game("player_state.speed", RawValue{std::string("x")}) ==
          "player_state.speed needs a number, not a string");
    CHECK(write_int("player_state.name", 1) == "player_state.name needs a string, not an integer");
    CHECK(write_game("player_state.gold", RawValue{true}) ==
          "player_state.gold needs an integer, not a boolean");
    CHECK(game::player_state.gold == -1000);
}

TEST_CASE("an enum value not declared, and a flag bit not declared, are refused") {
    reset_game();
    CHECK(write_int("player_state.mode", 7) == "7 is not a value of player_state.mode");
    CHECK(game::player_state.mode == game::Mode::On);
    CHECK(write_int("player_state.stored_mode", 3) ==
          "3 is not a value of player_state.stored_mode");
    CHECK(game::player_state.stored_mode == game::Mode::Auto);
    CHECK(write_int("player_state.flags", 16) == "16 is not a flag of player_state.flags");
    CHECK(game::player_state.flags == bits_of(3));
    CHECK(write_int("player_state.flags", 11).empty()); // 1, 2 and 8 together
    CHECK(game::player_state.flags == bits_of(11));
}

TEST_CASE("a string too long, or with a NUL byte, is refused") {
    reset_game();
    CHECK(write_game("player_state.name", RawValue{std::string("abcdefgh")}) ==
          "player_state.name is too long (at most 7 bytes)");
    CHECK(write_game("player_state.name", RawValue{std::string("abcdefg")}).empty());
    CHECK(write_game("player_state.name", RawValue{std::string("a\0b", 3)}) ==
          "player_state.name contains a NUL byte");
    CHECK(std::get<std::string>(value_at("player_state.name")) == "abcdefg");
}

TEST_CASE("a float that is not finite, or too big for a float, is refused") {
    reset_game();
    CHECK(write_game("player_state.speed", RawValue{std::numeric_limits<double>::quiet_NaN()}) ==
          "player_state.speed is not a finite number");
    CHECK(write_game("player_state.speed", RawValue{std::numeric_limits<double>::infinity()}) ==
          "player_state.speed is not a finite number");
    CHECK(write_game("player_state.speed", RawValue{1e39}) ==
          "player_state.speed is out of range for a float");
    CHECK(game::player_state.speed == 1.5f);
    CHECK(write_game("player_state.speed", RawValue{3e38}).empty());
}

TEST_CASE("an array's last element is reachable, and one past it is refused") {
    reset_game();
    CHECK(write_int("item_stock[2][5]", 1).empty());
    CHECK(game::item_stock[2][5] == 1);
    CHECK(write_int("item_stock[2][6]", 1) == "item_stock[2][6] is past the end (6 elements)");
    CHECK(write_int("item_stock[3]", 1) == "item_stock[3] is past the end (3 elements)");
    CHECK(write_int("player_state.items[3].id", 1) ==
          "player_state.items[3] is past the end (3 elements)");
    CHECK(read_game("item_stock[3][0]").error == "item_stock[3] is past the end (3 elements)");
}

TEST_CASE("an unknown name or a malformed path is refused") {
    reset_game();
    CHECK(read_game("player_state.foo").error == "player_state has no field 'foo'");
    CHECK(read_game("nope.x").error == "no global 'nope'");
    CHECK(read_game("nope.").error == "bad path 'nope.'");
    CHECK(read_game("player_state.").error == "bad path 'player_state.'");
    CHECK(read_game("player_state[x]").error == "bad path 'player_state[x]'");
    CHECK(read_game("").error == "empty path");
    CHECK(read_game("player_state[0]").error == "player_state is not an array");
    CHECK(read_game("player_state.gold.x").error == "player_state.gold is not a struct");
}

TEST_CASE("a read-only field is refused with its reason, and still reads") {
    reset_game();
    CHECK(write_int("player_state.locked", 1) ==
          "player_state.locked is read-only: held by the game");
    CHECK(game::player_state.locked == 8);
    CHECK(read_int("player_state.locked") == 8);
    CHECK(write_int("player_state.version", 4) ==
          "player_state.version is read-only: const in the decomp");
    CHECK(read_int("player_state.version") == 3);
}

TEST_CASE("a read-only global is named by its own path however deep the write goes") {
    static const std::array<RawGlobal, 1> frozen_globals = {
        raw_global<[]() -> auto & { return game::player_state; }>("player_state",
                                                                  read_only_limit("frozen", ""))};
    static const RawLayer frozen_layer{.path = "t.raw", .doc = "", .globals = frozen_globals};
    reset_game();
    CHECK(raw_write(frozen_layer, "player_state.vitals.current_hp", RawValue{std::int64_t{1}}) ==
          "player_state is read-only: frozen");
    CHECK(game::player_state.vitals.current_hp == 40);
}

TEST_CASE("a struct or an array is not a value") {
    reset_game();
    CHECK(read_game("player_state").error == "player_state is a struct, not a value");
    CHECK(read_game("item_stock").error == "item_stock is an array, not a value");
    CHECK(write_int("player_state.vitals", 1) == "player_state.vitals is a struct, not a value");
    CHECK(write_int("item_stock", 1) == "item_stock is an array, not a value");
}

TEST_CASE("a limit on an array narrows its elements") {
    reset_game();
    CHECK(write_int("player_state.tally[0]", 10) ==
          "10 is out of range for player_state.tally[0] (0 to 9)");
    CHECK(write_int("player_state.tally[0]", 9).empty());
    CHECK(game::player_state.tally[0] == 9);
    CHECK(game::player_state.tally[1] == 2);
}

TEST_CASE("the definitions name each class, alias and field") {
    const std::string text = raw_definitions(game_layer);
    CHECK(text == raw_definitions(game_layer)); // deterministic
    CHECK(text.find("---@class t.raw\n") != std::string::npos);
    CHECK(text.find("---@field player_state t.raw.Player\n") != std::string::npos);
    CHECK(text.find("---@field ranged_level integer (1 to 5)\n") != std::string::npos);
    CHECK(text.find("---@field copy fun(proxy: any): table") != std::string::npos);
    CHECK(text.find("t.raw = {}\n") != std::string::npos);
    CHECK(text.find("---@class t.raw.Player\n") != std::string::npos);
    CHECK(text.find("---@field level integer (-128 to 127)\n") != std::string::npos);
    CHECK(text.find("---@field gold integer (-2147483648 to 2147483647)\n") != std::string::npos);
    CHECK(text.find("---@field speed number\n") != std::string::npos);
    CHECK(text.find("---@field alive boolean\n") != std::string::npos);
    CHECK(text.find("---@field stored_mode t.raw.Mode\n") != std::string::npos);
    CHECK(text.find("---@field flags t.raw.Bits\n") != std::string::npos);
    CHECK(text.find("---@field name string (at most 7 bytes)\n") != std::string::npos);
    CHECK(text.find("---@field vitals t.raw.Vitals\n") != std::string::npos);
    CHECK(text.find("---@field items {[integer]: t.raw.Item} (3 elements, from 0)\n") !=
          std::string::npos);
    CHECK(text.find("---@field tally {[integer]: integer} (4 elements, from 0) (0 to 9)\n") !=
          std::string::npos);
    CHECK(text.find("---@field version integer (-2147483648 to 2147483647) (read-only: const in "
                    "the decomp)\n") != std::string::npos);
    CHECK(text.find("---@field locked integer (-2147483648 to 2147483647) (read-only: held by the "
                    "game) The "
                    "stored score.\n") != std::string::npos);
    CHECK(text.find("---@class t.raw.Vitals\n") != std::string::npos);
    CHECK(text.find("---@class t.raw.Item\n") != std::string::npos);
    CHECK(text.find("---MODE_AUTO = 2\n---@alias t.raw.Mode integer\n") != std::string::npos);
    CHECK(
        text.find(
            "---bits: BIT_LIT = 1, BIT_OPEN = 2, BIT_HIDDEN = 8\n---@alias t.raw.Bits integer\n") !=
        std::string::npos);
}

TEST_CASE("a mod reads and writes a nested field through kf.raw") {
    reset_game();
    RawMods mods;
    mods.run("a", "t.raw.player_state.vitals.current_hp = 5\n"
                  "t.note(tostring(t.raw.player_state.vitals.current_hp))\n"
                  "t.raw.ranged_level = 4\n"
                  "t.raw.player_state.gold = 3.0\n");
    CHECK(notes == std::vector<std::string>{"5"});
    CHECK(game::player_state.vitals.current_hp == 5);
    CHECK(game::ranged_level == 4);
    CHECK(game::player_state.gold == 3);
    CHECK(mods.info("a").state == ModState::Running);
}

TEST_CASE("a kept proxy resolves again from its global") {
    reset_game();
    RawMods mods;
    mods.run("a", "kept = t.raw.player_state\n"
                  "function on_update() t.note(tostring(kept.gold)) end\n");
    game::player_state.gold = 77;
    mods.host->call_each(ModCallback::Update);
    CHECK(notes == std::vector<std::string>{"77"});
}

TEST_CASE("# and pairs follow the array's length and the struct's fields") {
    reset_game();
    RawMods mods;
    mods.run("a",
             "for k, v in pairs(t.raw.player_state.vitals) do t.note(k .. '=' .. tostring(v)) end\n"
             "t.note(tostring(#t.raw.item_stock))\n"
             "for i in pairs(t.raw.item_stock[0]) do t.note(tostring(i)) end\n");
    CHECK(notes == std::vector<std::string>{"current_hp=40", "max_hp=60", "3", "0", "1", "2", "3",
                                            "4", "5"});
}

TEST_CASE("copy makes a plain table, with arrays keyed from 0") {
    reset_game();
    RawMods mods;
    mods.run("a", "local c = t.raw.copy(t.raw.item_stock[2])\n"
                  "t.note(tostring(c[0]) .. ',' .. tostring(c[5]) .. ',' .. tostring(c[6]))\n"
                  "local v = t.raw.copy(t.raw.player_state.vitals)\n"
                  "t.note(tostring(v.current_hp) .. '/' .. tostring(v.max_hp))\n"
                  "local p = t.raw.copy(t.raw.player_state)\n"
                  "t.note(tostring(p.vitals.max_hp) .. ',' .. tostring(p.items[1].count))\n");
    CHECK(notes == std::vector<std::string>{"0,5,nil", "40/60", "60,9"});
}

TEST_CASE("path and tostring name the place") {
    reset_game();
    RawMods mods;
    mods.run("a", "t.note(t.raw.path(t.raw.player_state.items[1]))\n"
                  "t.note(tostring(t.raw.item_stock[1]))\n"
                  "t.note(tostring(t.raw.player_state))\n");
    CHECK(notes ==
          std::vector<std::string>{"player_state.items[1]", "item_stock[1]", "player_state"});
}

TEST_CASE("each bad access makes the mod Failed with its message, and writes nothing") {
    reset_game();
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"t.raw.player_state.level = 200",
         "200 is out of range for player_state.level (-128 to 127)"},
        {"t.raw.player_state.locked = 1", "player_state.locked is read-only: held by the game"},
        {"t.raw.player_state.name = 'abcdefgh'", "player_state.name is too long (at most 7 bytes)"},
        {"t.raw.player_state.vitals = 1", "player_state.vitals is a struct, not a value"},
        {"t.raw.ranged_level = 9", "9 is out of range for ranged_level (1 to 5)"},
        {"t.raw.player_state.gold = 'x'", "player_state.gold needs an integer, not a string"},
        {"t.raw.player_state.gold = 3.5", "player_state.gold needs an integer, not a number"},
        {"t.raw.player_state.foo = 1", "player_state has no field 'foo'"},
        {"local x = t.raw.nope", "t.raw has no global 'nope'"},
        {"t.raw.player_state.speed = 0/0", "player_state.speed is not a finite number"},
        {"local x = #t.raw.player_state", "player_state is a struct, not an array"},
        {"t.raw.copy(1)", "expected a raw proxy, got number"},
        {"t.raw.player_state.tally[0] = 10",
         "10 is out of range for player_state.tally[0] (0 to 9)"},
        {"t.raw.player_state.mode = 7", "7 is not a value of player_state.mode"},
        {"t.raw.player_state.items[3].id = 1",
         "player_state.items[3] is past the end (3 elements)"},
        {"local x = t.raw.player_state.items[-1]",
         "raw arrays index from 0, and -1 is not an index"},
    };
    RawMods mods;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const std::string id = "bad" + std::to_string(i);
        mods.run(id, cases[i].first + "\n");
        const ModInfo info = mods.info(id);
        CHECK(info.state == ModState::Failed);
        CHECK(info.error.find(cases[i].second) != std::string::npos);
    }
    CHECK(game::player_state.level == 5);
    CHECK(game::player_state.gold == -1000);
    CHECK(game::ranged_level == 3);
    CHECK(game::player_state.mode == game::Mode::On);
}

TEST_CASE("a layer global named copy fails the mod that loads it") {
    RawMods mods(clashing_layer);
    mods.run("a", "t.note('loaded')\n");
    const ModInfo info = mods.info("a");
    CHECK(info.state == ModState::Failed);
    CHECK(info.error.find("may not be named 'copy'") != std::string::npos);
    CHECK(notes.empty());
}

TEST_CASE("the host's definitions file includes the layer") {
    RawMods mods;
    CHECK(mods.host->definitions().find("---@class t.raw.Player\n") != std::string::npos);
}
