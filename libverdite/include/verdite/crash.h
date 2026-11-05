#ifndef VERDITE_CRASH_H
#define VERDITE_CRASH_H
// Crash reports. On a fatal signal, an unhandled Windows exception or std::terminate, a
// report goes in a new folder: the stack named to functions, files and lines, the build,
// the game's own sections, the last events and copies of the files that reproduce it.
// Game-agnostic: the game names its sections and records its events.
//
// The stack is captured in the handler without allocating and named afterwards by a
// helper process started from the same binary (crash_helper_run), so the shipped game
// carries its line tables. On Android the game is a library inside the system's app
// process, so there is no helper and the stack stays as addresses in each library, named
// off the phone against the release's symbols. Under AddressSanitizer the sanitizer's own
// report is kept: the report is written from its death callback, without a stack.
//
// A crash's report is for the player to find: where the process can start itself again it
// becomes a window (crash_window_enable), and otherwise the next launch tells them
// (crash_take_unseen_report). A game thread that stops beating gets a hang report
// (crash_hang_watch_start). Only the newest 20 reports are kept.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace verdite {

// Report text in a buffer the caller reserved, without allocating or locking, so a section
// can be written from a signal handler. What does not fit is dropped.
class CrashWriter {
  public:
    explicit CrashWriter(std::span<char> buffer);
    // "name: value"
    void line(std::string_view name, std::int64_t value);
    void line(std::string_view name, std::string_view text);
    // "name: 0x1f"
    void hex(std::string_view name, std::uint64_t value);
    // "name: x, y, z"
    void triple(std::string_view name, std::int64_t x, std::int64_t y, std::int64_t z);
    // Pieces of a line, for a layout the above do not give; a line ends with "\n".
    void text(std::string_view text);
    void number(std::int64_t value);
    void hex_value(std::uint64_t value); // "0x1f"
    std::string_view written() const;

  private:
    std::span<char> buffer_;
    std::size_t used_ = 0;
};

// One part of the report. write reads plain game state and must not allocate, lock or
// follow a pointer it has not bounds-checked: it runs in the crash handler.
struct CrashSection {
    const char *title; // "player", "current actor"
    void (*write)(CrashWriter &out);
};

struct CrashConfig {
    std::string tag;       // the game's tag: "KF1"
    std::string directory; // reports go in <directory>/crash-<UTC time>-<pid>/
    std::string build;     // version, commit and build type, one line
    std::vector<CrashSection> sections;
};

// Installs the handlers, on the game thread, as early as possible. Calling it again
// replaces the configuration.
bool crash_install(CrashConfig config);
// A copy of this file goes in each report (a save). Up to 8; the game thread.
void crash_keep_file(std::string_view path);
// An entry in the report's recent events; the last 64 are kept. The game thread.
void crash_event(std::string_view text);
// A fault the game survives: a report the same way, the stack named in process, then the
// game carries on. At most 8 a run. Returns the report's folder.
std::optional<std::string> crash_report_soft(std::string_view what);

// ---- telling the player ----

// From now on a crash starts this binary again to tell the player, once its report is done:
// on POSIX by replacing the crashed process, so its window and mouse grab go and an AppImage
// stays mounted. Not on Android, where there is no binary to start: the next launch tells them.
void crash_window_enable();

// What the process was started to show, when it was started by crash_window_enable. First
// thing in main, after crash_helper_run.
struct CrashWindowRequest {
    std::string folder; // the report's, UTF-8
    int exit_code = 1;  // what the process ends with, as the crash would have
};
std::optional<CrashWindowRequest> crash_window_request(int argc, char **argv);

// The player has been told about this report.
void crash_report_seen(std::string_view folder);

// The newest report of a crash or a hang no one has been told about, and marks them all told.
// For the launch after one. A hang the game came back from is not one of them.
std::optional<std::string> crash_take_unseen_report();

enum class CrashReportKind : std::uint8_t { Crash, Hang };
CrashReportKind crash_report_kind(std::string_view folder);

// The file:// address of a folder, to hand to the system.
std::string crash_file_url(std::string_view folder);

// ---- hangs ----

// Starts a thread that writes a report when crash_hang_beat has not been called for hang_ms,
// with the game thread's own stack and state: hang-<UTC time>-<pid>/. It waits for the first
// beat, never reports under a debugger, and a suspended machine does not count. If the beats
// come back the report says so and the player is not told. On the game thread, once.
bool crash_hang_watch_start(int hang_ms);
// The game thread is alive. Cheap: once a frame or more.
void crash_hang_beat();
// Around a wait known to be long and frameless, a dialog or the app in the background. They
// nest, and work from any thread.
void crash_hang_pause();
void crash_hang_resume();

// First thing in main: when the process was started as the crash helper, names the stack
// of the report it was given and returns the exit code; otherwise nothing.
std::optional<int> crash_helper_run(int argc, char **argv);

} // namespace verdite

#endif // VERDITE_CRASH_H
