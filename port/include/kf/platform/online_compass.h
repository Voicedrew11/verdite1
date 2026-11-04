#ifndef KF_PLATFORM_ONLINE_COMPASS_H
#define KF_PLATFORM_ONLINE_COMPASS_H
// PC extension (coop): dots outside the compass's N, E, S and W, one in each other player's colour,
// at the point nearest the way that player lies on this floor. The dial is fixed to the world (its
// needle shows where this player faces), so N is +z and E is +x whichever way this player looks.
#include <kf/lib/types.h>
#include <kf/platform/online_players.h>

#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <vector>

namespace kf {

inline constexpr s32 online_compass_dot_px = 4;
inline constexpr s32 online_compass_dot_gap_px = 1;   // from the dial, and between dots at one point

struct OnlineCompassRect {
    s32 left, top, right, bottom;
    bool operator==(const OnlineCompassRect &) const = default;
};

enum class OnlineCompassPoint : u8 { North, East, South, West };

// The point nearest the way from here; on a diagonal, north or south. None when exactly here.
constexpr std::optional<OnlineCompassPoint> online_compass_point(double east, double north)
{
    if (east == 0.0 && north == 0.0)
        return std::nullopt;
    if (std::abs(north) >= std::abs(east))
        return north > 0.0 ? OnlineCompassPoint::North : OnlineCompassPoint::South;
    return east > 0.0 ? OnlineCompassPoint::East : OnlineCompassPoint::West;
}

// Dot `index` of `count` at a point, in a row along the dial's edge, centred on the point.
constexpr OnlineCompassRect online_compass_dot_at(OnlineCompassRect dial, OnlineCompassPoint point, s32 index, s32 count)
{
    const s32 step = online_compass_dot_px + online_compass_dot_gap_px;
    const s32 row = count * step - online_compass_dot_gap_px;
    const s32 along_x = (dial.left + dial.right - row + 1) / 2 + index * step;
    const s32 along_y = (dial.top + dial.bottom - row + 1) / 2 + index * step;
    const s32 size = online_compass_dot_px;
    const s32 gap = online_compass_dot_gap_px;
    switch (point) {
    case OnlineCompassPoint::North: return {along_x, dial.top - gap - size, along_x + size, dial.top - gap};
    case OnlineCompassPoint::South: return {along_x, dial.bottom + gap, along_x + size, dial.bottom + gap + size};
    case OnlineCompassPoint::West: return {dial.left - gap - size, along_y, dial.left - gap, along_y + size};
    case OnlineCompassPoint::East: return {dial.right + gap, along_y, dial.right + gap + size, along_y + size};
    }
    return {};
}

struct OnlineCompassDot {
    OnlineCompassRect rect;
    u8 colour;   // the player's: an index of the profile's colours
};

// The dots for the others on this floor, seen from (own_x, own_z) in the cells OnlinePlayer's are in.
// None for the player whose eyes a down player sees through, who stands where the camera is.
inline std::vector<OnlineCompassDot> online_compass_dots(OnlineCompassRect dial, float own_x, float own_z,
    std::span<const OnlinePlayer> players)
{
    struct Placed {
        OnlineCompassPoint point;
        u8 colour;
    };
    std::vector<Placed> placed;
    std::array<s32, 4> at_point {};
    for (const OnlinePlayer &player : players) {
        if (player.eyes)
            continue;
        const auto point = online_compass_point(double(player.x) - own_x, double(player.z) - own_z);
        if (!point)
            continue;
        placed.push_back({*point, player.colour});
        ++at_point[static_cast<std::size_t>(*point)];
    }
    std::vector<OnlineCompassDot> dots;
    std::array<s32, 4> next {};
    for (const Placed &each : placed) {
        const auto index = static_cast<std::size_t>(each.point);
        dots.push_back({online_compass_dot_at(dial, each.point, next[index]++, at_point[index]), each.colour});
    }
    return dots;
}

}

#endif // KF_PLATFORM_ONLINE_COMPASS_H
