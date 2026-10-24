#include <verdite/crash.h>

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define CRASH_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CRASH_TEST_ASAN 1
#endif
#endif

using namespace verdite;
namespace fs = std::filesystem;

namespace {
std::string written(void (*write)(CrashWriter &), std::size_t capacity = 256) {
    std::string buffer(capacity, '\0');
    CrashWriter out(buffer);
    write(out);
    return std::string(out.written());
}

fs::path fresh_directory(const char *name) {
    const fs::path path = fs::temp_directory_path() /
                          ("verdite-crash-" + std::string(name) + "-" + std::to_string(::getpid()));
    fs::remove_all(path);
    return path;
}

std::string read_text(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The one report folder in directory.
fs::path only_report(const fs::path &directory) {
    fs::path found;
    int count = 0;
    for (const auto &entry : fs::directory_iterator(directory)) {
        found = entry.path();
        ++count;
    }
    REQUIRE(count == 1);
    return found;
}

CrashConfig config_in(const fs::path &directory, std::vector<CrashSection> sections = {}) {
    return {"TEST", directory.string(), "test build", std::move(sections)};
}

void write_player(CrashWriter &out) {
    out.line("hp", 12);
    out.triple("position", 1, -2, 3);
}

[[gnu::noinline]] std::optional<std::string> soft_report_from_here() {
    return crash_report_soft("a door with no key");
}
} // namespace

TEST_CASE("the writer formats numbers, hex and triples") {
    CHECK(written([](CrashWriter &out) { out.line("hp", -42); }) == "hp: -42\n");
    CHECK(written([](CrashWriter &out) {
              out.line("low", std::numeric_limits<std::int64_t>::min());
          }) == "low: -9223372036854775808\n");
    CHECK(written([](CrashWriter &out) { out.hex("address", 0x1f); }) == "address: 0x1f\n");
    CHECK(written([](CrashWriter &out) { out.hex("zero", 0); }) == "zero: 0x0\n");
    CHECK(written([](CrashWriter &out) { out.triple("pos", 1, -2, 3); }) == "pos: 1, -2, 3\n");
    CHECK(written([](CrashWriter &out) { out.line("floor", "Western Shore"); }) ==
          "floor: Western Shore\n");
}

TEST_CASE("the writer drops what does not fit") {
    CHECK(written([](CrashWriter &out) { out.line("name", "a long value"); }, 8) == "name: a ");
}

TEST_CASE("a process not started as the helper is left to run") {
    std::array<char, 8> name = {"game"};
    std::array<char *, 2> arguments = {name.data(), nullptr};
    CHECK_FALSE(crash_helper_run(1, arguments.data()).has_value());
}

TEST_CASE("a soft fault writes a named report and the game carries on") {
    const fs::path directory = fresh_directory("soft");
    const fs::path save =
        directory.parent_path() / ("verdite-crash-save-" + std::to_string(::getpid()) + ".kfs");
    std::ofstream(save) << "a save";
    REQUIRE(crash_install(config_in(directory, {{"player", write_player}})));
    crash_keep_file(save.string());
    for (int at = 0; at < 70; ++at)
        crash_event("event " + std::to_string(at));
    const fs::path folder = soft_report_from_here().value_or("");
    REQUIRE_FALSE(folder.empty());
    const std::string report = read_text(folder / "report.txt");
    CHECK(report.starts_with("TEST crash report\nbuild: test build\n"));
    CHECK(report.find("fault: soft: a door with no key\n") != std::string::npos);
    CHECK(report.find("soft_report_from_here") != std::string::npos);
    CHECK(report.find("tests/crash.cpp:") != std::string::npos);
    // The last 64 of the 70 and the soft fault's own, oldest first.
    CHECK(report.find("event 6\n") == std::string::npos);
    CHECK(report.find("== events (oldest first)\nevent 7\nevent 8\n") != std::string::npos);
    CHECK(report.find("event 69\nsoft: a door with no key\n") != std::string::npos);
    CHECK(report.find("== player\nhp: 12\nposition: 1, -2, 3\n") != std::string::npos);
    CHECK(report.find(save.filename().string() + " (copied)") != std::string::npos);
    CHECK(read_text(folder / save.filename()) == "a save");
    fs::remove_all(directory);
    fs::remove(save);
}

#ifndef _WIN32
namespace {
// Through a volatile pointer, so neither the compiler nor the analyzer sees the null.
[[gnu::noinline]] void fault_on_purpose() {
    int *volatile target = nullptr;
    *target = 1;
}

#ifndef CRASH_TEST_ASAN
// Volatile, so the analyzer does not take it for the null it is.
const int *volatile damaged_pointer = nullptr;

void write_damaged(CrashWriter &out) {
    out.line("before", 1);
    out.line("read", *damaged_pointer);
}

void write_after(CrashWriter &out) {
    out.line("after", 2);
}
#endif

// std::terminate while an exception is being handled, out of doctest's reach.
[[gnu::noinline]] void terminate_while_handling() {
    try {
        throw std::runtime_error("the chest table ran out");
    } catch (const std::exception &) {
        std::terminate();
    }
}

// Runs crash in a child with the handlers installed; returns the signal that ended it.
int crash_in_child(const fs::path &directory, std::vector<CrashSection> sections, void (*crash)()) {
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        if (!crash_install(config_in(directory, std::move(sections))))
            _exit(2);
        crash_event("entered floor 2");
        crash();
        _exit(3);
    }
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    return WIFSIGNALED(status) ? WTERMSIG(status) : 0;
}
} // namespace

#ifndef CRASH_TEST_ASAN
TEST_CASE("a fault is reported with its stack named by the helper") {
    const fs::path directory = fresh_directory("fault");
    CHECK(crash_in_child(directory, {{"damaged", write_damaged}, {"after", write_after}},
                         fault_on_purpose) == SIGSEGV);
    const std::string report = read_text(only_report(directory) / "report.txt");
    CHECK(report.find("fault: SIGSEGV (bad memory access)\naddress: 0x0\n") != std::string::npos);
    CHECK(report.find("thread: the game's\n") != std::string::npos);
    // The faulting function first: the handler's own frames are gone.
    CHECK(report.find("== stack\n#0 (anonymous namespace)::fault_on_purpose()\n") !=
          std::string::npos);
    CHECK(report.find("tests/crash.cpp:") != std::string::npos);
    CHECK(report.find("== frames\n#0 0x") != std::string::npos);
    // The child's events follow those it was forked with.
    CHECK(report.find("\nentered floor 2\n== files\n") != std::string::npos);
    // A section that faults on damaged state is cut short; the next is still written.
    CHECK(report.find("== damaged\nbefore: 1\n(stopped: reading this state faulted)\n") !=
          std::string::npos);
    CHECK(report.find("== after\nafter: 2\n") != std::string::npos);
    fs::remove_all(directory);
}
#else
TEST_CASE("under AddressSanitizer a fault is reported from its death callback") {
    const fs::path directory = fresh_directory("sanitizer");
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        if (!crash_install(config_in(directory, {{"player", write_player}})))
            _exit(2);
        fault_on_purpose();
        _exit(3);
    }
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    const std::string report = read_text(only_report(directory) / "report.txt");
    CHECK(report.find("fault: sanitizer: its report is in the output above\n") !=
          std::string::npos);
    CHECK(report.find("== player\nhp: 12\n") != std::string::npos);
    fs::remove_all(directory);
}
#endif

TEST_CASE("std::terminate is reported with what it was handed") {
    const fs::path directory = fresh_directory("terminate");
    CHECK(crash_in_child(directory, {}, terminate_while_handling) == SIGABRT);
    const std::string report = read_text(only_report(directory) / "report.txt");
    CHECK(report.find("fault: SIGABRT (aborted)\n") != std::string::npos);
    CHECK(report.find("message: the chest table ran out\n") != std::string::npos);
    CHECK(report.find("terminate_while_handling()") != std::string::npos);
    fs::remove_all(directory);
}
#endif
