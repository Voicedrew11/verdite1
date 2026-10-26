#ifndef KF_PLATFORM_ENEMY_TURN_H
#define KF_PLATFORM_ENEMY_TURN_H
// How fast an enemy turns toward the player (enemy_turn_scale, and whether it
// keeps the player in sight, in input.h). Fixed: the settings' multiplier.
// Automatic: as fast as the player can turn with the input that last turned
// the view, against the D-pad's rate, which the fights were built on:
//
//   D-pad, arrow keys, the left stick as a D-pad   x1, the original
//   an analog stick                                its turn speed times its look acceleration's top
//   the mouse, a look drag on a touch screen       x3: they have no top speed
//
// held to x1..x3, and the player kept in sight above x1. Of the inputs that
// turned in one update, the fastest counts, so keys and the mouse together do
// not flip it each update; it changes between updates.
#include <kf/lib/types.h>
#include <kf/platform/analog.h>

namespace kf {

enum class EnemyTurnMode : u8 { Fixed, Automatic };
enum class TurnInput : u8 { Buttons, Stick, Pointer };

struct EnemyTurnOptions {
    EnemyTurnMode mode = EnemyTurnMode::Automatic;
    float fixed_scale = 1.5f;     // Fixed
    bool fixed_track = true;      // Fixed, above x1
    float stick_reach = 1.0f;     // Automatic: the stick's top turn, times the D-pad's
};

struct EnemyTurn {
    float scale;
    bool track;
};

// The enemies' turn for these options after the player turned with this input.
EnemyTurn enemy_turn_for(const EnemyTurnOptions &options, TurnInput input);
// How many times the D-pad's rate the stick turns at most with these options.
float stick_turn_reach(const AnalogOptions &options);

// Settings: Gameplay > Enemies. Applies at once, from the last input to turn.
void host_set_enemy_turn(const EnemyTurnOptions &options);
// The view was turned with this input this update.
void host_turned_with(TurnInput input);
// Between two game updates: the fastest input of the one just run becomes the last.
void host_enemy_turn_between_updates();

} // namespace kf

#endif // KF_PLATFORM_ENEMY_TURN_H
