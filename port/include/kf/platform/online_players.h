#ifndef KF_PLATFORM_ONLINE_PLAYERS_H
#define KF_PLATFORM_ONLINE_PLAYERS_H
// PC extension: online play. The other players in the online room, kept by the host
// (port/ext/online.cpp) for the game to draw in the world.
#include <kf/lib/types.h>

#include <vector>

namespace kf {
struct OnlinePlayer {
    u32 id;      // the same while they stay in the room
    float x, z;  // in cells, as AutomapSnapshot (0..100)
    float yaw;   // radians, as AutomapSnapshot
};

// The other players on this floor (1..5), as last heard from; empty when not in a room.
std::vector<OnlinePlayer> online_players_on_floor(int floor);
}

#endif // KF_PLATFORM_ONLINE_PLAYERS_H
