#pragma once
// The sounds of the knights (docs/COOP_PLAN.md, "Other players' spells and sounds"): this player's
// counted for its pose as the game plays them (kf::online_sound_made), and another player's played
// from where their knight is drawn when the counts in their pose move.
#include "online_pose.h"

#include <array>
#include <cstdint>

namespace kf::ext {

// How many of each sound this player has made, wrapping, for its pose.
std::array<std::uint8_t, online_sound_kinds> online_sounds_made();
// Plays what another player on this floor made between two of their poses, from where the later
// one stands, as the game plays it for its own player but heard from there.
void online_sounds_play(const OnlinePose &was, const OnlinePose &now);
// How many of the others' sounds this machine has played, by kind, for the agent.
std::array<std::uint32_t, online_sound_kinds> online_sounds_heard();

}
