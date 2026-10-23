// Console capture: stdout/stderr are redirected into a pipe; a reader thread
// splits lines into a ring buffer for the Console panel, and still forwards the
// bytes to the original stdout and to kf.log.
#include "internal.hpp"
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define kf_pipe(fds) _pipe(fds, 65536, _O_BINARY)
#define kf_dup _dup
#define kf_dup2 _dup2
#define kf_read _read
#define kf_write _write
#else
#include <unistd.h>
#define kf_pipe(fds) pipe(fds)
#define kf_dup dup
#define kf_dup2 dup2
#define kf_read read
#define kf_write write
#endif

namespace kf::ext {
namespace {
constexpr std::size_t line_capacity = 8000;
std::mutex console_mutex;
std::deque<ConsoleLine> lines;
std::uint64_t next_serial = 1;
std::string partial;
int original_stdout = -1;
FILE *log_file;
bool started;

void push_line(std::string text) {
    if (!text.empty() && text.back() == '\r')
        text.pop_back();
    std::lock_guard lock(console_mutex);
    lines.push_back({std::move(text), next_serial++});
    while (lines.size() > line_capacity)
        lines.pop_front();
}

void reader(int fd) {
    char buffer[4096];
    for (;;) {
        const int count = kf_read(fd, buffer, sizeof buffer);
        if (count <= 0)
            return;
        if (original_stdout >= 0)
            kf_write(original_stdout, buffer, static_cast<unsigned>(count));
        if (log_file) {
            std::fwrite(buffer, 1, static_cast<std::size_t>(count), log_file);
            std::fflush(log_file);
        }
        for (int i = 0; i < count; ++i) {
            if (buffer[i] == '\n') {
                push_line(std::move(partial));
                partial.clear();
            } else {
                partial.push_back(buffer[i]);
            }
        }
    }
}
}

void console_start() {
    if (started)
        return;
    started = true;
    std::error_code error;
    std::filesystem::create_directories(user_dir(), error);
    log_file = std::fopen((user_dir() / "kf.log").string().c_str(), "wb");
    int fds[2];
    if (kf_pipe(fds) != 0)
        return;
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef _WIN32
    // A windowed (GUI subsystem) build starts without standard streams.
    if (_fileno(stdout) < 0)
        std::freopen("NUL", "w", stdout);
    if (_fileno(stderr) < 0)
        std::freopen("NUL", "w", stderr);
#endif
    original_stdout = kf_dup(fileno(stdout));
    kf_dup2(fds[1], fileno(stdout));
    kf_dup2(fds[1], fileno(stderr));
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::thread(reader, fds[0]).detach();
}

void console_line(const std::string &line) {
    std::printf("%s\n", line.c_str());
}

std::uint64_t console_copy(std::uint64_t after, std::vector<ConsoleLine> &out) {
    std::lock_guard lock(console_mutex);
    for (const auto &line : lines)
        if (line.serial > after)
            out.push_back(line);
    return next_serial - 1;
}

void console_clear() {
    std::lock_guard lock(console_mutex);
    lines.clear();
}

void console_pump() {}
}
