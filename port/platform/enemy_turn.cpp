#include <kf/platform/enemy_turn.h>

#include <kf/platform/input.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <utility>

namespace kf {
namespace {

constexpr float automatic_max = 3.0f;
constexpr float pointer_scale = automatic_max;
constexpr std::array<const char *, 3> input_names = {"buttons", "stick", "pointer"};

struct EnemyTurnState {
    EnemyTurnOptions options;
    TurnInput last = TurnInput::Buttons;
    std::optional<TurnInput> this_update;   // the fastest to turn so far
};
EnemyTurnState state;

void apply() {
    const EnemyTurn turn = enemy_turn_for(state.options, state.last);
    if (turn.scale == enemy_turn_scale && turn.track == enemy_tracks_player)
        return;
    enemy_turn_scale = turn.scale;
    enemy_tracks_player = turn.track;
    if (state.options.mode == EnemyTurnMode::Automatic)
        std::printf("[enemy turn] x%.2f%s (%s)\n", turn.scale, turn.track ? ", tracking" : "",
                    input_names[static_cast<std::size_t>(state.last)]);
}

} // namespace

EnemyTurn enemy_turn_for(const EnemyTurnOptions &options, TurnInput input) {
    if (options.mode == EnemyTurnMode::Fixed) {
        const float scale = std::clamp(options.fixed_scale, 1.0f, 4.0f);
        return {scale, options.fixed_track && scale > 1.0f};
    }
    float scale = 1.0f;
    if (input == TurnInput::Stick)
        scale = options.stick_reach;
    else if (input == TurnInput::Pointer)
        scale = pointer_scale;
    scale = std::clamp(scale, 1.0f, automatic_max);
    return {scale, scale > 1.0f};
}

float stick_turn_reach(const AnalogOptions &options) {
    const float acceleration = options.look_acceleration ? options.acceleration_max : 1.0f;
    return options.turn_sensitivity * acceleration;
}

void host_set_enemy_turn(const EnemyTurnOptions &options) {
    state.options = options;
    apply();
}

void host_turned_with(TurnInput input) {
    const auto scale = [](TurnInput each) { return enemy_turn_for(state.options, each).scale; };
    if (!state.this_update || scale(input) > scale(*state.this_update))
        state.this_update = input;
}

void host_enemy_turn_between_updates() {
    const std::optional<TurnInput> turned = std::exchange(state.this_update, std::nullopt);
    if (!turned || *turned == state.last)
        return;
    state.last = *turned;
    apply();
}

} // namespace kf
