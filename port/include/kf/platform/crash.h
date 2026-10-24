#ifndef KF_PLATFORM_CRASH_H
#define KF_PLATFORM_CRASH_H
// KF1's crash reports (libverdite's verdite/crash.h) in <user dir>/crashes: the stack named
// to the decomp's functions, what the game was doing read from its own state (the player, the
// actor and effect being updated), the recent events and copies of the saves.

namespace kf {
// First in main, on the game thread.
void crash_start();
// Once a host poll: a change of input context, floor or the player's death becomes an event.
void crash_poll();
} // namespace kf

#endif // KF_PLATFORM_CRASH_H
