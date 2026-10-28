#pragma once
// The player's profile (docs/COOP_PLAN.md, "The profile"): the name, colour and helmet the others in
// a room see. Chosen on the title's CO-OP page, kept in the user folder beside the install's id,
// sent in the player's hello and the host's roster.
#include "coop_party.h"

#include <kf/platform/online_players.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace kf::ext {

// Words as the menu font draws them: English capitals, or kana without small kana.
struct CoopProfileWords {
    std::string_view english, japanese;
};

struct CoopColour {
    CoopProfileWords name;
    std::uint8_t r, g, b;   // the knight's tabard, sleeves, knee bands and crest
};
inline constexpr std::size_t coop_colour_count = 8;
std::span<const CoopColour, coop_colour_count> coop_colours();
// The helmets' names, in online_player_helmet_count's order.
std::span<const CoopProfileWords, online_player_helmet_count> coop_helmet_names();

struct CoopProfile {
    std::string name;          // coop_name_valid
    std::uint8_t colour = 0;   // < coop_colour_count
    std::uint8_t helmet = 0;   // < online_player_helmet_count
    bool operator==(const CoopProfile &) const = default;
};

// An install's first profile: PLAYER and four digits of its id, the first colour and helmet.
CoopProfile coop_profile_default(CoopPlayerId id);
bool coop_profile_valid(const CoopProfile &profile);

// The file's text ("name=ANDREW\ncolour=2\nhelmet=0\n"), and back: a part missing or bad in the
// text is the fallback's.
std::string coop_profile_text(const CoopProfile &profile);
CoopProfile coop_profile_read(std::string_view text, const CoopProfile &fallback);

} // namespace kf::ext
