#include <kf/platform/enemy_turn.h>

#include <kf/platform/input.h>

#include <doctest/doctest.h>

using namespace kf;

namespace {
EnemyTurnOptions automatic(float stick_reach) {
    EnemyTurnOptions options;
    options.mode = EnemyTurnMode::Automatic;
    options.stick_reach = stick_reach;
    return options;
}
} // namespace

TEST_CASE("automatic enemy turn follows how fast the last input can turn") {
    const EnemyTurn buttons = enemy_turn_for(automatic(2.0f), TurnInput::Buttons);
    CHECK(buttons.scale == 1.0f);
    CHECK_FALSE(buttons.track);
    const EnemyTurn stick = enemy_turn_for(automatic(1.0f), TurnInput::Stick);
    CHECK(stick.scale == 1.0f);   // the stick at the D-pad's rate changes nothing
    CHECK_FALSE(stick.track);
    const EnemyTurn fast_stick = enemy_turn_for(automatic(2.2f), TurnInput::Stick);
    CHECK(fast_stick.scale == doctest::Approx(2.2f));
    CHECK(fast_stick.track);
    CHECK(enemy_turn_for(automatic(0.5f), TurnInput::Stick).scale == 1.0f);
    CHECK(enemy_turn_for(automatic(8.0f), TurnInput::Stick).scale == 3.0f);
    const EnemyTurn pointer = enemy_turn_for(automatic(1.0f), TurnInput::Pointer);
    CHECK(pointer.scale == 3.0f);
    CHECK(pointer.track);
}

TEST_CASE("fixed enemy turn is the setting, whatever turned") {
    EnemyTurnOptions options;
    options.mode = EnemyTurnMode::Fixed;
    options.fixed_scale = 1.5f;
    options.fixed_track = false;
    CHECK(enemy_turn_for(options, TurnInput::Pointer).scale == 1.5f);
    CHECK_FALSE(enemy_turn_for(options, TurnInput::Pointer).track);
    options.fixed_track = true;
    options.fixed_scale = 1.0f;
    CHECK_FALSE(enemy_turn_for(options, TurnInput::Buttons).track);   // no tracking at x1
}

TEST_CASE("the stick reaches its turn speed times its acceleration") {
    AnalogOptions options;
    options.turn_sensitivity = 1.5f;
    CHECK(stick_turn_reach(options) == 1.5f);
    options.look_acceleration = true;
    options.acceleration_max = 2.0f;
    CHECK(stick_turn_reach(options) == 3.0f);
}

TEST_CASE("the fastest input of an update counts, between updates") {
    host_set_enemy_turn(automatic(1.0f));
    host_turned_with(TurnInput::Buttons);
    host_enemy_turn_between_updates();
    CHECK(enemy_turn_scale == 1.0f);
    host_turned_with(TurnInput::Pointer);
    host_turned_with(TurnInput::Buttons);
    CHECK(enemy_turn_scale == 1.0f);   // nothing changes inside an update
    host_enemy_turn_between_updates();
    CHECK(enemy_turn_scale == 3.0f);
    CHECK(enemy_tracks_player);
    host_enemy_turn_between_updates();   // no turn: stays
    CHECK(enemy_turn_scale == 3.0f);
    host_turned_with(TurnInput::Buttons);
    host_enemy_turn_between_updates();
    CHECK(enemy_turn_scale == 1.0f);
    CHECK_FALSE(enemy_tracks_player);
}
