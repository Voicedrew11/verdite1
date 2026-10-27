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
    s32 foot_height;  // the feet's y, up is negative
    float stride_phase; // 0..1 through the walk; the right foot leaves the ground at 0, the left at 0.5
    float stride;       // 0 standing .. 1 a full stride
    bool eyes;          // this player, down, sees through theirs: their knight is not drawn here
};

// Whether the world keeps running while this player is held in a menu or a scripted loop: in a
// room (docs/COOP_PLAN.md), or with KF_TEST_WORLD_RUNS_WHILE_HELD=1 for tests without one.
bool online_world_runs_while_held();

// The other players on this floor (1..5) who are up, as last heard from; empty when not in a room.
std::vector<OnlinePlayer> online_players_on_floor(int floor);

// This player, as the others in the room are sent it.
struct OnlinePlayerPose {
    int floor;        // 1..5
    float x, z;       // in cells, as AutomapSnapshot
    float yaw;        // radians, as AutomapSnapshot
    s32 foot_height;  // the feet's y, up is negative
    u8 weapon;        // the equipped weapon (0..15), or online_no_weapon
    s16 attack_phase; // 0..4095 through a swing, or online_not_attacking
    s16 pitch;        // the camera's, in 1/4096 turns, positive looking down
};

// Once per world update, held or not: sends this player's pose to the others in the room, numbered
// by the update, and plays theirs on by one update (docs/PLAYER_MODEL.md).
void online_update(const OnlinePlayerPose &own);

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
