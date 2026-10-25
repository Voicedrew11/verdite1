#include <verdite/fuzz.h>

#include <doctest/doctest.h>

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace verdite;
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t walk = 1, attack = 2, back = 4, confirm = 8;

fs::path temp_path(const std::string &name) {
    return fs::temp_directory_path() / ("verdite-fuzz-" + std::to_string(::getpid()) + "-" + name);
}

struct Finished {
    int code;
};

// A small game: presses, one chance macro and one invariant that the test can break.
struct Game {
    std::vector<std::string> macro_runs;
    bool broken = false;
    bool menu = false;

    FuzzConfig config(std::uint64_t seed, const fs::path &recording) {
        FuzzConfig config;
        config.tag = "TEST";
        config.seed = seed;
        config.ticks = 100000;
        config.status_every_ticks = 0;
        config.recording = recording.string();
        config.presses = {{"walk", walk, 3, 2, 30}, {"attack", attack, 1, 1, 4}, {"idle", 0, 1, 1, 10}};
        config.menu_presses = {{"confirm", confirm, 1, 2, 2}, {"back", back, 1, 2, 2}};
        config.escape = {back, confirm};
        config.in_menu = [this] { return menu; };
        config.macros_table = {{"jump", 500,
                                [](FuzzRandom &random) { return std::optional(std::to_string(random.below(100))); },
                                [this](std::string_view args) {
                                    macro_runs.emplace_back(args);
                                    return !args.empty();
                                },
                                0}};
        config.invariants = {{"broken", [this]() -> std::optional<std::string> {
                                  return broken ? std::optional<std::string>("it broke") : std::nullopt;
                              }}};
        config.finish = [](int code) { throw Finished{code}; };
        return config;
    }
};

// Plays ticks 1..ticks with an update every fourth tick; returns the buttons at each tick.
std::vector<std::uint32_t> play(std::uint64_t ticks) {
    std::vector<std::uint32_t> held;
    for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
        fuzz_poll(tick);
        held.push_back(fuzz_buttons(tick));
        if (tick % 4 == 0)
            fuzz_between_updates(tick);
    }
    return held;
}

} // namespace

TEST_CASE("FuzzRandom is repeatable and stays in range") {
    FuzzRandom a(7), b(7), c(8);
    for (int i = 0; i < 100; ++i) {
        const auto x = a.next();
        CHECK(x == b.next());
        const int r = a.between(3, 5);
        CHECK(r == b.between(3, 5));
        CHECK((r >= 3 && r <= 5));
    }
    CHECK(FuzzRandom(7).next() != c.next());
    CHECK(FuzzRandom(1).below(0) == 0);
    CHECK(FuzzRandom(1).between(4, 4) == 4);
}

TEST_CASE("a seed plays the same buttons and macros, and the recording replays them") {
    const auto recording = temp_path("rec.log");
    Game first;
    REQUIRE(fuzz_start(first.config(42, recording)));
    const auto played = play(4000);
    CHECK(!first.macro_runs.empty());

    Game again;
    REQUIRE(fuzz_start(again.config(42, temp_path("again.log"))));
    CHECK(play(4000) == played);
    CHECK(again.macro_runs == first.macro_runs);

    Game replay;
    auto config = replay.config(999, temp_path("unused.log"));
    config.replay = recording.string();
    REQUIRE(fuzz_start(config));
    CHECK(fuzz_replaying());
    CHECK(play(4000) == played);
    CHECK(replay.macro_runs == first.macro_runs);
    CHECK(!fs::exists(temp_path("unused.log")));

    // Releases between presses: no press is held straight into the next.
    std::size_t edges = 0;
    for (std::size_t i = 1; i < played.size(); ++i)
        if (played[i] != 0 && played[i - 1] == 0)
            ++edges;
    CHECK(edges > 50);
    fs::remove(recording);
    fs::remove(temp_path("again.log"));
}

TEST_CASE("a different seed plays differently") {
    Game a, b;
    REQUIRE(fuzz_start(a.config(1, temp_path("a.log"))));
    const auto one = play(2000);
    REQUIRE(fuzz_start(b.config(2, temp_path("b.log"))));
    CHECK(play(2000) != one);
    fs::remove(temp_path("a.log"));
    fs::remove(temp_path("b.log"));
}

TEST_CASE("the menu table is used in menus") {
    Game game;
    game.menu = true;
    auto config = game.config(5, temp_path("menu.log"));
    config.macros = false;
    REQUIRE(fuzz_start(config));
    for (const auto buttons : play(1000))
        CHECK((buttons == 0 || buttons == confirm || buttons == back));
    fs::remove(temp_path("menu.log"));
}

TEST_CASE("invariants report, settle and stop") {
    Game game;
    auto config = game.config(3, temp_path("inv.log"));
    config.macros = false;
    config.stop_on_violation = true;
    REQUIRE(fuzz_start(config));
    fuzz_poll(1);
    fuzz_settle(2);
    game.broken = true;
    fuzz_between_updates(2);
    fuzz_between_updates(3);
    try {
        fuzz_between_updates(4);
        FAIL("no stop at the violation");
    } catch (const Finished &finished) {
        CHECK(finished.code == 3);
    }
    fs::remove(temp_path("inv.log"));
}

TEST_CASE("a stalled game gets the escape buttons, then a stall") {
    Game game;
    auto config = game.config(4, temp_path("stall.log"));
    config.macros = false;
    config.escape_after_ticks = 10;
    config.stall_after_ticks = 50;
    config.stop_on_violation = true;
    REQUIRE(fuzz_start(config));
    fuzz_poll(1);
    std::vector<std::uint32_t> held;
    for (std::uint64_t tick = 12; tick < 40; ++tick)
        held.push_back(fuzz_buttons(tick));
    for (const auto buttons : held)
        CHECK((buttons == 0 || buttons == back || buttons == confirm));
    CHECK(std::find(held.begin(), held.end(), back) != held.end());
    CHECK(std::find(held.begin(), held.end(), confirm) != held.end());
    try {
        fuzz_poll(60);
        FAIL("no stall");
    } catch (const Finished &finished) {
        CHECK(finished.code == 3);
    }
    fs::remove(temp_path("stall.log"));
}

TEST_CASE("the run ends after its ticks, and a trigger is recorded") {
    Game game;
    const auto recording = temp_path("end.log");
    auto config = game.config(6, recording);
    config.macros = false;
    config.ticks = 100;
    REQUIRE(fuzz_start(config));
    fuzz_poll(1);
    fuzz_between_updates(2);
    fuzz_trigger("jump", "17");
    CHECK(game.macro_runs == std::vector<std::string>{"17"});
    try {
        fuzz_poll(101);
        FAIL("the run did not end");
    } catch (const Finished &finished) {
        CHECK(finished.code == 0);
    }
    std::ifstream file(recording);
    std::stringstream text;
    text << file.rdbuf();
    CHECK(text.str().find("# verdite-fuzz 1\nseed 6\n") == 0);
    CHECK(text.str().find("\nm 1 jump 17\n") != std::string::npos);
    fs::remove(recording);
}

TEST_CASE("a recording that is not one is refused") {
    const auto path = temp_path("bad.log");
    std::ofstream(path) << "hello\n";
    Game game;
    auto config = game.config(1, temp_path("x.log"));
    config.replay = path.string();
    CHECK_FALSE(fuzz_start(config));
    CHECK_FALSE(fuzz_active());
    fs::remove(path);
}
