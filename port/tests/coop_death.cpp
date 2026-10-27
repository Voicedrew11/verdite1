#include "../ext/coop_death.h"

#include <doctest/doctest.h>

#include <vector>

using namespace kf::ext;

namespace {

constexpr CoopPlayerId host = 10, first = 20, second = 30;

CoopPartyMember member(CoopPlayerId id, int floor, float x, float z, bool down = false) {
    return {id, floor, x, z, down, false};
}

} // namespace

TEST_CASE("a down player keeps the eyes it has while that player is up") {
    const std::vector room = {member(host, 1, 10, 10), member(first, 1, 50, 50)};
    CHECK(coop_eyes_choose(room, first, 1, 11, 11) == first);
}

TEST_CASE("a down player sees through the nearest up player on its floor") {
    const std::vector room = {member(host, 1, 40, 40), member(first, 1, 12, 10), member(second, 2, 10, 10)};
    CHECK(coop_eyes_choose(room, 0, 1, 10, 10) == first);
}

TEST_CASE("eyes that went down or left are replaced") {
    const std::vector room = {member(host, 1, 40, 40), member(first, 1, 12, 10, true)};
    CHECK(coop_eyes_choose(room, first, 1, 10, 10) == host);
    CHECK(coop_eyes_choose(room, second, 1, 10, 10) == host);
}

TEST_CASE("with no one up on its floor, a down player sees through the first up player in the room") {
    const std::vector room = {member(host, 3, 40, 40, true), member(first, 2, 12, 10), member(second, 4, 10, 10)};
    CHECK(coop_eyes_choose(room, 0, 1, 10, 10) == first);
}

TEST_CASE("no one up: no eyes") {
    const std::vector room = {member(host, 1, 40, 40, true), member(first, 1, 12, 10, true)};
    CHECK(coop_eyes_choose(room, host, 1, 10, 10) == 0);
    CHECK(coop_eyes_next(room, host) == 0);
}

TEST_CASE("the next eyes go round the up players in the room's order") {
    const std::vector room = {member(host, 1, 0, 0), member(first, 1, 0, 0, true), member(second, 2, 0, 0)};
    CHECK(coop_eyes_next(room, host) == second);
    CHECK(coop_eyes_next(room, second) == host);
    CHECK(coop_eyes_next(room, 99) == host);
}

TEST_CASE("the only up player stays the eyes") {
    const std::vector room = {member(host, 1, 0, 0), member(first, 1, 0, 0, true)};
    CHECK(coop_eyes_next(room, host) == host);
}

TEST_CASE("the party is down only when every player is") {
    const std::vector up = {member(first, 1, 0, 0), member(second, 1, 0, 0, true)};
    const std::vector down = {member(first, 1, 0, 0, true), member(second, 1, 0, 0, true)};
    CHECK_FALSE(coop_party_down(true, up));
    CHECK_FALSE(coop_party_down(false, down));
    CHECK(coop_party_down(true, down));
}

TEST_CASE("a host alone is a party of one") {
    CHECK(coop_party_down(true, {}));
    CHECK_FALSE(coop_party_down(false, {}));
}

TEST_CASE("a down player comes back on the first free cell beside, then a corner, then the same cell") {
    const CoopCell at{20, 30};
    CHECK(coop_cell_beside(at, [](CoopCell) { return true; }) == CoopCell{20, 31});
    CHECK(coop_cell_beside(at, [](CoopCell cell) { return cell == CoopCell{19, 30}; }) == CoopCell{19, 30});
    CHECK(coop_cell_beside(at, [](CoopCell cell) { return cell == CoopCell{21, 29}; }) == CoopCell{21, 29});
    CHECK(coop_cell_beside(at, [](CoopCell) { return false; }) == at);
}

TEST_CASE("a hit keeps a player in a fight for the calm updates") {
    CoopFight fight(3);
    CHECK_FALSE(fight.fighting());
    fight.hit();
    CHECK(fight.fighting());
    fight.update(false);
    fight.update(false);
    CHECK(fight.fighting());
    fight.update(false);
    CHECK_FALSE(fight.fighting());
}

TEST_CASE("an enemy chasing keeps a player in a fight however long since a hit") {
    CoopFight fight(2);
    fight.update(true);
    CHECK(fight.fighting());
    fight.update(false);
    CHECK_FALSE(fight.fighting());
}

TEST_CASE("a player with no pose yet is up but has no eyes to see through") {
    const std::vector room = {member(host, 0, 0, 0), member(first, 2, 5, 5)};
    CHECK(coop_eyes_choose(room, host, 1, 0, 0) == first);
    CHECK(coop_eyes_next(room, first) == first);
    CHECK_FALSE(coop_party_down(true, room));
}
