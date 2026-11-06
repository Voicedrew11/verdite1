#include "../ext/death_reload_rule.h"

#include <doctest/doctest.h>

using namespace kf;
using kf::ext::death_reload_slot;

TEST_CASE("a death reloads the save last made or loaded") {
    CHECK(death_reload_slot({true, false, SaveSlot::Second}) == SaveSlot::Second);
    CHECK(death_reload_slot({true, false, SaveSlot::Quick}) == SaveSlot::Quick);
}

TEST_CASE("the game restarts when there is nothing to reload or reloading is not ours") {
    CHECK_FALSE(death_reload_slot({true, false, std::nullopt}));
    CHECK_FALSE(death_reload_slot({false, false, SaveSlot::First}));   // off, or Classic mode
    CHECK_FALSE(death_reload_slot({true, true, SaveSlot::First}));     // a coop room has its own death
    CHECK_FALSE(death_reload_slot({true, false, SaveSlot::Aside}));    // a game set aside, not in play
}
