#include <kf/platform/logcat.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <string>
#include <thread>

namespace kf {
namespace {
constexpr const char *log_tag = "Verdite1";

// On the logcat thread, for the life of the process: each line read from the pipe, to logcat.
void forward_lines(int pipe_end) {
    std::array<char, 1024> chunk{};
    std::string line;
    for (;;) {
        const ssize_t got = read(pipe_end, chunk.data(), chunk.size());
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return;
        for (ssize_t i = 0; i < got; ++i) {
            const char c = chunk[static_cast<std::size_t>(i)];
            if (c != '\n') {
                line += c;
                continue;
            }
            __android_log_write(ANDROID_LOG_INFO, log_tag, line.c_str());
            line.clear();
        }
    }
}
} // namespace

bool logcat_start() {
    std::array<int, 2> ends{};
    if (pipe(ends.data()) != 0)
        return false;
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    const bool redirected = dup2(ends[1], STDOUT_FILENO) >= 0 && dup2(ends[1], STDERR_FILENO) >= 0;
    close(ends[1]);
    if (!redirected) {
        close(ends[0]);
        return false;
    }
    std::thread(forward_lines, ends[0]).detach();
    return true;
}
} // namespace kf
#else
namespace kf {
bool logcat_start() { return true; }
} // namespace kf
#endif
