#ifndef KF_PLATFORM_CRASH_H
#define KF_PLATFORM_CRASH_H
#include <optional>

// KF1's crash reports (libverdite's verdite/crash.h) in <user dir>/crashes: the stack named
// to the decomp's functions, what the game was doing read from its own state (the player, the
// actor and effect being updated), the recent events and copies of the saves.

namespace kf {
// First in main, on the game thread.
void crash_start();
// Next in main: when a crash started this process to tell the player, shows them and returns
// the exit code. Needs nothing else of the game.
std::optional<int> crash_window_run(int argc, char **argv);
// Once it is settled a player is at the screen (not a headless run): a crash from now on opens
// the window, and a crash or a hang of the last run that no one was told about is told now.
void crash_tell_player();
// After the host has started, for a player: 15 seconds without a poll writes a hang report.
// KF_HANG=seconds sets that, for any run; 0 turns it off.
constexpr int crash_player_hang_s = 15;
void crash_watch_hangs(int hang_s);
// Once a host poll: a change of input context, floor or the player's death becomes an event.
void crash_poll();
} // namespace kf

#endif // KF_PLATFORM_CRASH_H
