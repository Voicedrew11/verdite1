#include "mod_play.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace kf::ext {
namespace {

float clamp_speed(float speed) {
    return std::clamp(speed, min_mod_speed, max_mod_speed);
}

s32 permille_of(float scale) {
    return static_cast<s32>(std::lround(clamp_speed(scale) * 1000.0f));
}

} // namespace

ModPlayCombined combine_mod_play(std::span<const ModPlay> mods) {
    bool noclip = false;
    bool invincible = false;
    bool peaceful = false;
    bool paused = false;
    float move_speed = 1.0f;
    float turn_speed = 1.0f;
    float game_speed = 1.0f;
    for (const ModPlay &mod : mods) {
        noclip = noclip || mod.noclip;
        invincible = invincible || mod.invincible;
        peaceful = peaceful || mod.peaceful;
        paused = paused || mod.paused;
        move_speed *= mod.move_speed;
        turn_speed *= mod.turn_speed;
        game_speed *= mod.game_speed;
    }

    ModPlayCombined combined;
    combined.overrides.walls_block_player = !noclip;
    combined.overrides.player_takes_damage = !invincible;
    combined.overrides.enemies_attack = !peaceful;
    combined.overrides.move_scale_permille = permille_of(move_speed);
    combined.overrides.turn_scale_permille = permille_of(turn_speed);
    combined.game_speed = clamp_speed(game_speed);
    combined.paused = paused;
    return combined;
}

} // namespace kf::ext
