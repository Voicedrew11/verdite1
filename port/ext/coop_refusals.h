#pragma once
// What this machine refused from other players (docs/COOP_PLAN.md, "What is refused"). A refusal
// only drops what was sent: no one is disconnected for it, since a bug or a rare game state can
// look the same as a forgery. Each is counted, so the coop tests can say that honest play refuses
// nothing, and logged at most once a second for each kind, so a flood cannot fill the log.
#include <cstddef>
#include <cstdint>
#include <string>

namespace kf::ext {

enum class CoopRefusal : std::uint8_t {
    NotAMessage,   // CONTROL bytes that do not decode
    Frame,         // a floor owner's actors and effects
    Pose,          // a player's pose or the spells it carries
    FloorWrites,   // changes to the floor's lasting state
    FloorState,    // a floor's record or live snapshot
    Character,     // a guest's character, which the host would keep and give back
};
inline constexpr std::size_t coop_refusal_kinds = 6;

// Notes one refusal from `peer` at `now_ms` on a steady clock. True when it was logged.
bool coop_refused(CoopRefusal what, std::uint64_t peer, std::uint64_t now_ms);
// How many of this kind since the room was joined.
std::uint64_t coop_refused_count(CoopRefusal what);
// The counts, for the agent's coop command: "refused":{"frame":0,...}
std::string coop_refused_json();
// A new room: the counts start again.
void coop_refused_forget();

}
