#ifndef VERDITE_CRASH_H
#define VERDITE_CRASH_H
// Crash reports. On a fatal signal, an unhandled Windows exception or std::terminate, a
// report goes in a new folder: the stack named to functions, files and lines, the build,
// the game's own sections, the last events and copies of the files that reproduce it.
// Game-agnostic: the game names its sections and records its events.
//
// The stack is captured in the handler without allocating and named afterwards by a
// helper process started from the same binary (crash_helper_run), so the shipped game
// carries its line tables. Under AddressSanitizer the sanitizer's own report is kept:
// the report is written from its death callback, without a stack.
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

// First thing in main: when the process was started as the crash helper, names the stack
// of the report it was given and returns the exit code; otherwise nothing.
std::optional<int> crash_helper_run(int argc, char **argv);

} // namespace verdite

#endif // VERDITE_CRASH_H
