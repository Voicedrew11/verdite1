#include "../ext/mod_play.h"

#include <doctest/doctest.h>

#include <array>
#include <span>

using namespace kf;
using namespace kf::ext;

TEST_CASE("no mods play the game as it is") {
    const ModPlayCombined combined = combine_mod_play({});
    CHECK(combined.overrides.walls_block_player);
    CHECK(combined.overrides.player_takes_damage);
    CHECK(combined.overrides.enemies_attack);
    CHECK(combined.overrides.move_scale_permille == 1000);
    CHECK(combined.overrides.turn_scale_permille == 1000);
    CHECK(combined.game_speed == 1.0f);
    CHECK_FALSE(combined.paused);
}

TEST_CASE("one mod's switch turns its own switch off in the game") {
    ModPlay noclip;
    noclip.noclip = true;
    const ModPlayCombined combined = combine_mod_play(std::span{&noclip, 1});
    CHECK_FALSE(combined.overrides.walls_block_player);
    CHECK(combined.overrides.player_takes_damage);
    CHECK(combined.overrides.enemies_attack);
}

TEST_CASE("switches are on when any mod turns them on") {
    std::array<ModPlay, 2> mods{};
    mods[0].noclip = true;
    mods[1].invincible = true;
    const ModPlayCombined combined = combine_mod_play(mods);
    CHECK_FALSE(combined.overrides.walls_block_player);
    CHECK_FALSE(combined.overrides.player_takes_damage);
    CHECK(combined.overrides.enemies_attack);

    std::array<ModPlay, 2> peaceful_pair{};
    peaceful_pair[1].peaceful = true;
    CHECK_FALSE(combine_mod_play(peaceful_pair).overrides.enemies_attack);
}

TEST_CASE("speeds multiply across mods") {
    std::array<ModPlay, 2> mods{};
    mods[0].move_speed = 2.0f;
    mods[1].move_speed = 1.5f;
    mods[0].turn_speed = 0.5f;
    mods[1].turn_speed = 0.5f;
    mods[0].game_speed = 2.0f;
    mods[1].game_speed = 2.0f;
    const ModPlayCombined combined = combine_mod_play(mods);
    CHECK(combined.overrides.move_scale_permille == 3000);
    CHECK(combined.overrides.turn_scale_permille == 250);
    CHECK(combined.game_speed == doctest::Approx(4.0f));
}

TEST_CASE("speeds are held to the top of their range") {
    std::array<ModPlay, 2> mods{};
    mods[0].move_speed = 4.0f;
    mods[1].move_speed = 4.0f;
    mods[0].game_speed = 4.0f;
    mods[1].game_speed = 4.0f;
    const ModPlayCombined combined = combine_mod_play(mods);
    CHECK(combined.overrides.move_scale_permille == 8000);
    CHECK(combined.game_speed == doctest::Approx(max_mod_speed));
}

TEST_CASE("speeds are held to the bottom of their range") {
    std::array<ModPlay, 3> mods{};
    for (ModPlay &mod : mods) {
        mod.turn_speed = 0.5f;
        mod.game_speed = 0.5f;
    }
    mods[0].move_speed = 0.1f;
    const ModPlayCombined combined = combine_mod_play(mods);
    CHECK(combined.overrides.move_scale_permille == 250);
    CHECK(combined.overrides.turn_scale_permille == 250);
    CHECK(combined.game_speed == doctest::Approx(min_mod_speed));
}

TEST_CASE("paused when any mod pauses") {
    ModPlay quiet;
    ModPlay paused;
    paused.paused = true;
    const std::array<ModPlay, 2> mixed{quiet, paused};
    CHECK(combine_mod_play(mixed).paused);
    CHECK_FALSE(combine_mod_play(std::span{&quiet, 1}).paused);
}
