#ifndef KF_PLATFORM_PLAY_OVERRIDES_H
#define KF_PLATFORM_PLAY_OVERRIDES_H
// What mods change about play (kf.play in docs/MODDING.md), asked by the game's code at the point
// where it applies. The defaults are the game's own behaviour.
#include <kf/lib/types.h>

namespace kf {

struct PlayOverrides {
    bool walls_block_player = true;    // player_move_horizontal's wall and step checks
    bool player_takes_damage = true;   // player_apply_damage, player_adjust_hp losses
    bool enemies_attack = true;        // actor_try_attack_player
    s32 move_scale_permille = 1000;    // forward/strafe distance in player_update_movement_input
    s32 turn_scale_permille = 1000;    // turn velocity
};

// The running mods' values, combined; the defaults when no mod sets any.
const PlayOverrides &host_play_overrides();

} // namespace kf

#endif // KF_PLATFORM_PLAY_OVERRIDES_H
