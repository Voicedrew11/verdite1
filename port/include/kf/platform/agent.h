#ifndef KF_PLATFORM_AGENT_H
#define KF_PLATFORM_AGENT_H
// KF1's side of libverdite's agent channel: the state snapshot and the
// commands. KF_AGENT=1: the beacon; KF_SHELL=1 or a port: the command channel
// on 127.0.0.1:27901. See docs/verdite-port.md, "Driving the game".
#include <kf/lib/types.h>

namespace kf {
void agent_start();
// On every host poll: runs queued commands and the beacon.
void agent_poll();
// Game buttons held by the command channel's press, or'd into the pad.
u32 agent_buttons();
}

#endif // KF_PLATFORM_AGENT_H
