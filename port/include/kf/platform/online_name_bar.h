#ifndef KF_PLATFORM_ONLINE_NAME_BAR_H
#define KF_PLATFORM_ONLINE_NAME_BAR_H
// PC extension (online play): the name bar above another player's knight, in the manner of
// Minecraft's: their name on a dark band, always facing the camera, its size fixed in the world so
// it shrinks with distance. Where its parts lie, in world units around the point above the knight's
// head that the band stands on: x to the camera's right, y down, so the band's bottom edge is at 0.
#include <kf/lib/types.h>
#include <kf/platform/online_players.h>

#include <algorithm>
#include <cmath>

namespace kf {

inline constexpr s32 online_name_bar_above_feet = 2030;       // the knight's crest is 1884 above its feet
inline constexpr s32 online_name_bar_units_per_px = 14;       // a font pixel, in world units
inline constexpr s32 online_name_bar_text_height_px = 12;     // the menu font's cell
inline constexpr s32 online_name_bar_margin_px = 2;           // of the band, around the text
inline constexpr s32 online_name_bar_nearest_depth = 700;     // nearer the camera it would fill the picture
// The largest camera-space component a bar's anchor keeps; a farther bar is drawn scaled about the eye.
inline constexpr s32 online_name_bar_reach = 30000;

struct OnlineNameBarRect {
    s32 left, top, right, bottom;
    bool operator==(const OnlineNameBarRect &) const = default;
};

// Whether a name is drawn above a player: in sight only where their cell is in view, always
// everywhere on the floor; never over the player whose eyes this is.
constexpr bool online_name_bar_shown(OnlineNameBars mode, bool eyes, bool in_window)
{
    if (eyes || mode == OnlineNameBars::Off)
        return false;
    return mode == OnlineNameBars::Always || in_window;
}

// Only a bar in sight is tested against the world's depth; one that shows through walls is not.
constexpr bool online_name_bar_depth_tested(OnlineNameBars mode)
{
    return mode == OnlineNameBars::InSight;
}

// The factor that draws a bar whose anchor is at (x, y, z) in camera space: 1 within reach. Scaling
// the bar and its anchor by the same factor about the eye leaves it where it is on screen, at the same size.
constexpr double online_name_bar_scale(double x, double y, double z)
{
    const double largest = std::max({std::abs(x), std::abs(y), std::abs(z)});
    return largest <= online_name_bar_reach ? 1.0 : online_name_bar_reach / largest;
}

// The band around a name `text_width_px` wide.
constexpr OnlineNameBarRect online_name_bar_band(s32 text_width_px) {
    const s32 half_width = (text_width_px + 2 * online_name_bar_margin_px) * online_name_bar_units_per_px / 2;
    const s32 height = (online_name_bar_text_height_px + 2 * online_name_bar_margin_px) * online_name_bar_units_per_px;
    return {-half_width, -height, half_width, 0};
}

// A piece of the text, at (`x_px`, `y_px`) from the text's top left and `width_px` by `height_px`.
constexpr OnlineNameBarRect online_name_bar_piece(s32 text_width_px, s32 x_px, s32 y_px, s32 width_px, s32 height_px) {
    const OnlineNameBarRect band = online_name_bar_band(text_width_px);
    const s32 left = band.left + (online_name_bar_margin_px + x_px) * online_name_bar_units_per_px;
    const s32 top = band.top + (online_name_bar_margin_px + y_px) * online_name_bar_units_per_px;
    return {left, top, left + width_px * online_name_bar_units_per_px, top + height_px * online_name_bar_units_per_px};
}

}

#endif // KF_PLATFORM_ONLINE_NAME_BAR_H
