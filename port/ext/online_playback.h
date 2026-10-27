#pragma once
// Another player's poses played back at the game's rate: each pose is numbered by the update its
// player sent it in, so the knight is drawn at every one of their updates, a few behind the newest
// heard, with the gaps lost poses leave filled in (docs/PLAYER_MODEL.md).
#include "online_pose.h"

#include <cstdint>
#include <map>
#include <optional>

namespace kf::ext {

class OnlinePlayback {
public:
    // How far behind the newest pose heard the playback runs, to ride out poses that come late.
    explicit OnlinePlayback(std::uint32_t delay_updates);

    // A pose as it arrives, in any order; one already played or already heard is dropped.
    void receive(const OnlinePose &pose);

    // One of this machine's updates: the pose to draw for it, nothing before the first. Holds the
    // last when none has come, and waits for the delay to build again before going on; catches up
    // a step at a time when behind, and jumps when far behind (a stall).
    std::optional<OnlinePose> step();

private:
    OnlinePose pose_at(std::uint32_t update) const;

    std::uint32_t delay_updates_;
    std::map<std::uint32_t, OnlinePose> heard_; // by update, from the last one at or before the playhead
    std::optional<std::uint32_t> playhead_;
    bool waiting_ = true; // for the delay to build, at the start or after running out
};

}
