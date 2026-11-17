#include "../ext/creature_feedback_rule.h"

#include <doctest/doctest.h>

using namespace kf;
using namespace kf::ext;

namespace {

constexpr int update_vblanks = 4;   // 15 updates a second
constexpr CreatureFeedbackOptions both_on{true, true};

CreatureSighting awake(u16 health, u8 definition = 3) {
    return {definition, health, true, false};
}
CreatureSighting dead_awake(u8 definition = 3) {
    return {definition, 0, true, true};
}
CreatureSighting dead_asleep(u8 definition = 3) {
    return {definition, 0, false, true};
}

// The slot after a run of updates that each found it as seen.
CreatureStep run(CreatureStep step, const CreatureSighting &seen, int updates,
                 const CreatureFeedbackOptions &options = both_on) {
    for (int i = 0; i < updates; ++i)
        step = creature_step(step.track, seen, update_vblanks, options);
    return step;
}

} // namespace

TEST_CASE("a monster is drawn as the game draws it until something happens to it") {
    const CreatureStep step = run({}, awake(40), 3);
    CHECK(step.look.drawn);
    CHECK(step.look.red_q12 == 0);
    CHECK(step.look.keep == 255);
}

TEST_CASE("a blow flushes it red, and the red eases back over the flash") {
    CreatureStep step = run({}, awake(40), 2);
    step = creature_step(step.track, awake(31), update_vblanks, both_on);
    CHECK(step.look.red_q12 == 4096);
    const CreatureStep later = run(step, awake(31), 2);
    CHECK(later.look.red_q12 > 0);
    CHECK(later.look.red_q12 < step.look.red_q12);
    CHECK(run(step, awake(31), hit_flash_vblanks / update_vblanks + 1).look.red_q12 == 0);
}

TEST_CASE("only a fall in health is a blow") {
    CreatureStep step = run({}, awake(40), 2);
    CHECK(creature_step(step.track, awake(55), update_vblanks, both_on).look.red_q12 == 0);   // healed
    CHECK(creature_step(step.track, awake(10, 4), update_vblanks, both_on).look.red_q12 == 0);   // a new occupant
}

TEST_CASE("a slain monster's body lies a moment, fades, and is gone") {
    CreatureStep step = run({}, awake(5), 2);
    step = run(step, dead_awake(), 2);   // its death ends while the game still holds it
    CHECK(step.look.keep == 255);
    step = run(step, dead_asleep(), body_hold_vblanks / update_vblanks);   // let go: drawn from the slot
    CHECK(step.look.drawn);
    const CreatureStep fading = run(step, dead_asleep(), 3);
    CHECK(fading.look.drawn);
    CHECK(fading.look.keep < 255);
    CHECK_FALSE(run(fading, dead_asleep(), body_fade_vblanks / update_vblanks).look.drawn);
}

TEST_CASE("a body is drawn only where a death was seen") {
    CHECK_FALSE(creature_step({}, dead_asleep(), update_vblanks, both_on).look.drawn);   // a floor entered with it
    const CreatureStep step = run(run({}, awake(5), 1), dead_awake(), 1);
    CHECK_FALSE(creature_step(step.track, dead_asleep(7), update_vblanks, both_on).look.drawn);   // another occupant
    const CreatureStep asleep = run(run({}, awake(5), 1), {3, 5, false, false}, 1);   // gone to sleep alive
    CHECK_FALSE(asleep.look.drawn);
}

TEST_CASE("a monster waking in a slot whose body is fading starts afresh") {
    CreatureStep step = run(run(run({}, awake(5), 1), dead_awake(), 1), dead_asleep(), 6);
    step = creature_step(step.track, awake(40), update_vblanks, both_on);
    CHECK(step.look.drawn);
    CHECK(step.look.keep == 255);
    CHECK(step.look.red_q12 == 0);
}

TEST_CASE("off, each is the original's") {
    const CreatureFeedbackOptions off{false, false};
    CreatureStep step = run({}, awake(40), 2, off);
    step = creature_step(step.track, awake(30), update_vblanks, off);
    CHECK(step.look.red_q12 == 0);
    step = run(step, dead_awake(), 8, off);
    CHECK(step.look.keep == 255);   // drawn whole while the game holds it
    CHECK_FALSE(creature_step(step.track, dead_asleep(), update_vblanks, off).look.drawn);   // then gone
}
