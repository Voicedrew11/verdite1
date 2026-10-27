#ifndef KF_PLATFORM_ONLINE_PLAYERS_H
#define KF_PLATFORM_ONLINE_PLAYERS_H
// PC extension: online play. The other players in the online room, kept by the host
// (port/ext/online.cpp) for the game to draw in the world, and the knight they are drawn as
// (docs/PLAYER_MODEL.md).
#include <kf/lib/geometry_types.h>
#include <kf/lib/types.h>

#include <cstddef>
#include <span>
#include <vector>

namespace kf {
inline constexpr u8 online_no_weapon = 0xff;         // KF_OBJECT_NONE
inline constexpr s16 online_not_attacking = -1;      // KF_WEAPON_ATTACK_INACTIVE

struct OnlinePlayer {
    u32 id;           // the same while they stay in the room
    float x, z;       // in cells, as AutomapSnapshot (0..100)
    float yaw;        // radians, as AutomapSnapshot
    u8 weapon;        // the equipped weapon (0..15), or online_no_weapon
    s16 attack_phase; // 0..4095 through a swing, or online_not_attacking
    s16 pitch;        // the camera's, in 1/4096 turns, positive looking down
    float stride_phase; // 0..1 through the walk; the right foot leaves the ground at 0, the left at 0.5
    float stride;       // 0 standing .. 1 a full stride
};

// The other players on this floor (1..5), as last heard from; empty when not in a room.
std::vector<OnlinePlayer> online_players_on_floor(int floor);

// What this player holds, how far through a swing they are and where they look up or down, for
// the others to draw; the game publishes it once per update.
void online_publish_player(u8 weapon, s16 attack_phase, s16 pitch);

// The knight, one TMD object per part in this order, each part's vertices relative to its pivot.
enum class OnlinePlayerPart : u8 {
    Pelvis, Torso, Head, UpperArmRight, ForearmRight, UpperArmLeft, ForearmLeft,
    ThighRight, ShinRight, ThighLeft, ShinLeft
};
inline constexpr std::size_t online_player_part_count = 11;

struct OnlinePlayerPartJoint {
    s8 parent;     // a part index, or -1 for the pelvis
    SVECTOR pivot; // in the model's space (the feet at 0, up is -y, facing -z)
};

// The knight's TMD (4-byte aligned, as the TMD reader requires) and its joints, in part order.
std::span<const u8> online_player_model_tmd();
std::span<const OnlinePlayerPartJoint, online_player_part_count> online_player_model_joints();
// Where the right forearm meets the upper arm, in the model's space: the weapon arm's origin.
SVECTOR online_player_model_right_elbow();
}

#endif // KF_PLATFORM_ONLINE_PLAYERS_H
