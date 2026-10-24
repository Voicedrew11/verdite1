#ifndef VERDITE_CRASH_REPORT_H
#define VERDITE_CRASH_REPORT_H
// Between crash.cpp, which writes a report, and crash_helper.cpp, which names its stack.
#include <cstddef>
#include <string>
#include <string_view>

namespace verdite::crash_report {

inline constexpr std::string_view helper_flag = "--verdite-crash-helper";
inline constexpr std::string_view file_name = "report.txt";
// Header lines ("name: value") the helper reads back.
inline constexpr std::string_view pid_name = "pid";
inline constexpr std::string_view binary_name = "binary";
// The stack section stands until the helper replaces it; the frames section holds one line a
// frame, "#<n> 0x<address> 0x<address in its object> <object path>", for the helper or for
// llvm-symbolizer by hand.
inline constexpr std::string_view stack_heading = "== stack\n";
inline constexpr std::string_view stack_unnamed = "(not named: the frames are below)\n";
inline constexpr std::string_view frames_heading = "== frames\n";

// The calling thread's stack named to functions, files and lines, skip frames left out.
// Allocates: never from a signal handler.
std::string named_stack_here(std::size_t skip);

} // namespace verdite::crash_report

#endif // VERDITE_CRASH_REPORT_H
