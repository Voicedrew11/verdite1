// The knight in each player's colour (kf/platform/online_players.h): a copy of its TMD per profile
// colour, the player's-colour primitives painted.
#include "coop_profile.h"

#include <kf/platform/online_players.h>

#include <array>
#include <vector>

namespace kf {
namespace {

std::array<std::vector<u8>, ext::coop_colour_count> painted;

} // namespace

std::span<const u8> online_player_tmd_in(u8 colour) {
    if (colour >= painted.size())
        colour = 0;
    std::vector<u8> &tmd = painted[colour];
    if (!tmd.empty())
        return tmd;
    const auto model = online_player_model_tmd();
    tmd.assign(model.begin(), model.end());
    const ext::CoopColour &paint = ext::coop_colours()[colour];
    for (const u32 offset : online_player_model_colour_offsets()) {
        if (offset + 3 > tmd.size())
            continue;
        tmd[offset] = paint.r;
        tmd[offset + 1] = paint.g;
        tmd[offset + 2] = paint.b;
    }
    return tmd;
}

} // namespace kf
