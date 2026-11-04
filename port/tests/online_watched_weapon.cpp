#include <kf/platform/online_watched_weapon.h>

#include <doctest/doctest.h>

#include <optional>
#include <vector>

using namespace kf;

namespace {

OnlinePlayer player(u32 id, bool eyes, u8 weapon, s16 attack_phase) {
    OnlinePlayer result{};
    result.id = id;
    result.eyes = eyes;
    result.weapon = weapon;
    result.attack_phase = attack_phase;
    return result;
}

std::optional<OnlineWatchedWeapon> watched_alone(const OnlinePlayer &only) {
    const std::vector<OnlinePlayer> players = {only};
    return online_watched_weapon(players);
}

}

TEST_CASE("nothing is drawn when no one is watched") {
    const std::vector<OnlinePlayer> players = {player(3, false, 2, 900)};
    CHECK_FALSE(online_watched_weapon(players).has_value());
    CHECK_FALSE(online_watched_weapon(std::vector<OnlinePlayer>{}).has_value());
}

TEST_CASE("the watched player's weapon and swing are drawn") {
    const std::vector<OnlinePlayer> players = {player(3, false, 1, 100), player(7, true, 5, 1500)};
    const auto watched = online_watched_weapon(players);
    REQUIRE(watched.has_value());
    CHECK(watched->weapon == 5);
    CHECK(watched->phase == 1500);
}

TEST_CASE("a watched player with no weapon draws none") {
    const std::vector<OnlinePlayer> players = {player(7, true, online_no_weapon, 1500)};
    CHECK_FALSE(online_watched_weapon(players).has_value());
}

TEST_CASE("a watched player not swinging draws none") {
    const std::vector<OnlinePlayer> players = {player(7, true, 5, online_not_attacking)};
    CHECK_FALSE(online_watched_weapon(players).has_value());
}

TEST_CASE("the swing's last phase is drawn and the one past it is not") {
    const auto last = watched_alone(player(7, true, 5, 4095));
    const auto past = watched_alone(player(7, true, 5, online_attack_phase_end));
    CHECK(last.has_value());
    CHECK_FALSE(past.has_value());
}

TEST_CASE("each player and weapon has its own identity, never the game's own weapon's 0") {
    const auto first = watched_alone(player(7, true, 5, 10));
    const auto other_weapon = watched_alone(player(7, true, 6, 10));
    const auto other_player = watched_alone(player(8, true, 5, 10));
    REQUIRE(first.has_value());
    REQUIRE(other_weapon.has_value());
    REQUIRE(other_player.has_value());
    CHECK(first->identity != 0);
    CHECK(first->identity != other_weapon->identity);
    CHECK(first->identity != other_player->identity);
}
