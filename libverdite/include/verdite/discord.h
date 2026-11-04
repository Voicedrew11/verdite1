#ifndef VERDITE_DISCORD_H
#define VERDITE_DISCORD_H
// Discord Rich Presence over the Discord app's local IPC (a Unix socket, or a
// named pipe on Windows), with no Discord SDK. A thread of its own finds
// Discord, keeps trying while it is closed, and sends the latest activity;
// the game thread only hands in what to show, which never blocks.
// Game-agnostic: the game names its Discord application and composes the text.
#include <cstdint>
#include <string>

namespace verdite {

struct DiscordActivity {
    std::string details;         // the first line; empty leaves it out
    std::string state;           // the second line; empty leaves it out
    std::string large_image;     // an art asset key of the application; empty for none
    std::string large_text;      // the image's hover text
    std::int64_t start_unix_s = 0;   // shown as the time elapsed; 0 for none

    bool operator==(const DiscordActivity &) const = default;
};

struct DiscordConfig {
    std::string application_id;  // the Discord application whose name shows as the game
    std::string tag;             // log lines are "[<tag>] discord: ..."
};

// Starts the publisher thread, which shows nothing until discord_show.
// False, and nothing started, without an application id.
bool discord_start(const DiscordConfig &config);
// Shows this activity soon. An unchanged one is not resent; a new first line
// goes within 50 ms, a change to the second alone within 250 ms, so a
// counting value does not flood Discord. Lines are clipped to Discord's 128
// characters.
void discord_show(const DiscordActivity &activity);
// Takes the presence down, keeping the connection, 1.5 s after the last
// activity at the soonest, so a client still looking that one up cannot undo it.
void discord_clear();
// Takes the presence down as discord_clear does, without waiting for Discord's
// reply, closes the connection and joins the thread; for exit. Blocks for up
// to 1.5 s when an activity has just gone out.
void discord_stop();

} // namespace verdite

#endif // VERDITE_DISCORD_H
