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
    bool flies = false;
    bool paused = false;
    bool silencing = false;   // a mod pauses without keeping the music
    bool takes_mouse = false;
    bool no_look_lead = false;
    u32 held_actions = 0;
    u32 blocked_actions = 0;
    float move_speed = 1.0f;
    float turn_speed = 1.0f;
    float top_speed = 1.0f;
    float game_speed = 1.0f;
    for (const ModPlay &mod : mods) {
        noclip = noclip || mod.noclip;
        invincible = invincible || mod.invincible;
        peaceful = peaceful || mod.peaceful;
        flies = flies || mod.fly;
        paused = paused || mod.paused;
        silencing = silencing || (mod.paused && !mod.music_while_paused);
        takes_mouse = takes_mouse || mod.takes_mouse;
        no_look_lead = no_look_lead || mod.no_look_lead;
        held_actions |= mod.held_actions;
        blocked_actions |= mod.blocked_actions;
        move_speed *= mod.move_speed;
        turn_speed *= mod.turn_speed;
        top_speed *= mod.top_speed;
        game_speed *= mod.game_speed;
    }

    ModPlayCombined combined;
    combined.overrides.walls_block_player = !noclip;
    combined.overrides.player_takes_damage = !invincible;
    combined.overrides.enemies_attack = !peaceful;
    combined.overrides.player_flies = flies;
    combined.overrides.move_scale_permille = permille_of(move_speed);
    combined.overrides.turn_scale_permille = permille_of(turn_speed);
    combined.overrides.top_speed_permille = permille_of(top_speed);
    combined.game_speed = clamp_speed(game_speed);
    combined.paused = paused;
    combined.music_while_paused = paused && !silencing;
    combined.takes_mouse = takes_mouse;
    combined.look_lead = !no_look_lead;
    combined.held_actions = held_actions;
    combined.blocked_actions = blocked_actions;
    return combined;
}

} // namespace kf::ext
