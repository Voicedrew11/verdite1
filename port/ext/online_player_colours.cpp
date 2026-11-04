// The knight in each player's colour (kf/platform/online_players.h): a copy of its TMD per profile
// colour, the player's-colour primitives painted.
#include "coop_profile.h"

#include <kf/platform/online_players.h>

#include <array>
#include <vector>

namespace kf {
namespace {

// A TMD texture page: 8-bit, in 64-column steps, the row's half in bit 4.
constexpr u16 knight_page = 151;   // a common page (docs/PLAYER_MODEL.md)
// A page whose metal square no TIM on the disc touches, so nothing here is overwritten or missed.
constexpr u16 title_knight_page = 130;   // at x 128, y 0

constexpr std::array<TextureMove, 2> title_moves = {{
    {{480, 385, 32, 126}, 160, 129},   // the 'knight' material, on page 151 (body.py's MATERIALS)
    {{0, 493, 256, 1}, 0, 493},        // its palette stays where it is: the logo has none
}};

using PaintedTmds = std::array<std::vector<u8>, ext::coop_colour_count>;
PaintedTmds painted, painted_for_title;

std::vector<u8> paint(u8 colour) {
    const auto model = online_player_model_tmd();
    std::vector<u8> tmd(model.begin(), model.end());
    const ext::CoopColour &wanted = ext::coop_colours()[colour];
    for (const u32 offset : online_player_model_colour_offsets()) {
        if (offset + 3 > tmd.size())
            continue;
        tmd[offset] = wanted.r;
        tmd[offset + 1] = wanted.g;
        tmd[offset + 2] = wanted.b;
    }
    for (const u32 offset : online_player_model_ink_offsets()) {
        if (offset + 3 > tmd.size())
            continue;
        tmd[offset] = wanted.ink.r;
        tmd[offset + 1] = wanted.ink.g;
        tmd[offset + 2] = wanted.ink.b;
    }
    return tmd;
}

void look_in_title_pages(std::vector<u8> &tmd) {
    for (const u32 offset : online_player_model_page_offsets()) {
        if (offset + 2 > tmd.size())
            continue;
        const u16 page = static_cast<u16>(tmd[offset] | tmd[offset + 1] << 8);
        if (page == knight_page)
            tmd[offset] = title_knight_page;
    }
}

} // namespace

OnlinePlayerRgb online_player_colour_rgb(u8 colour) {
    const ext::CoopColour &wanted = ext::coop_colours()[colour < ext::coop_colour_count ? colour : 0];
    return {wanted.r, wanted.g, wanted.b};
}

std::span<const u8> online_player_tmd_in(u8 colour) {
    if (colour >= painted.size())
        colour = 0;
    std::vector<u8> &tmd = painted[colour];
    if (tmd.empty())
        tmd = paint(colour);
    return tmd;
}

std::span<const TextureMove> online_player_title_texture_moves() {
    return title_moves;
}

std::span<const u8> online_player_title_tmd_in(u8 colour) {
    if (colour >= painted_for_title.size())
        colour = 0;
    std::vector<u8> &tmd = painted_for_title[colour];
    if (tmd.empty()) {
        tmd = paint(colour);
        look_in_title_pages(tmd);
    }
    return tmd;
}

} // namespace kf
