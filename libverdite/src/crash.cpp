#include <verdite/crash.h>

#include "crash_report.h"

#include <cpptrace/basic.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>

#include <csignal>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#include <csetjmp>
#include <csignal>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define VERDITE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VERDITE_ASAN 1
#endif
#endif
#ifdef VERDITE_ASAN
#include <sanitizer/common_interface_defs.h>
#endif

namespace verdite {
namespace {
constexpr std::size_t event_limit = 64;
constexpr std::size_t event_capacity = 120;
constexpr std::size_t kept_file_limit = 8;
constexpr std::size_t path_capacity = 1024;
constexpr std::size_t folder_name_room = 64; // "/crash-YYYYMMDD-HHMMSS-<pid>-<n>/report.txt"
constexpr std::size_t max_frames = 100;
constexpr int soft_report_limit = 8;
constexpr char hex_digits[] = "0123456789abcdef";

// value's decimal digits, at least width of them, in digits.
std::string_view decimal(std::uint64_t value, std::array<char, 20> &digits, std::size_t width = 1) {
    std::size_t at = digits.size();
    do {
        digits[--at] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && at > 0);
    while (digits.size() - at < width && at > 0)
        digits[--at] = '0';
    return {digits.data() + at, digits.size() - at};
}

// A path built without allocating; too long a path is refused, not cut.
struct PathText {
    std::array<char, path_capacity> chars{};
    std::size_t length = 0;
    bool overflowed = false;

    void add(std::string_view part) {
        if (part.size() >= chars.size() - length) {
            overflowed = true;
            return;
        }
        std::copy(part.begin(), part.end(), chars.begin() + static_cast<std::ptrdiff_t>(length));
        length += part.size();
    }
    void add_number(std::uint64_t value, std::size_t width = 1) {
        std::array<char, 20> digits{};
        add(decimal(value, digits, width));
    }
    void clear() {
        length = 0;
        overflowed = false;
    }
    char *c_str() {
        chars[length] = '\0';
        return chars.data();
    }
    std::string_view view() const {
        return {chars.data(), length};
    }
};

struct Event {
    std::array<char, event_capacity> text{};
    std::size_t length = 0;
};

struct State {
    CrashConfig config;
    PathText directory;
    PathText binary; // this executable, started again as the helper
    std::array<Event, event_limit> events{};
    std::atomic<std::uint64_t> events_added{0};
    std::array<PathText, kept_file_limit> kept_files{};
    std::size_t kept_file_count = 0;
    std::atomic<int> reports{0};
    int soft_reports = 0;
    std::array<char, 512> terminate_message{};
    std::size_t terminate_message_length = 0;
    std::atomic<bool> handling{false};
    bool installed = false;
};
State state;
thread_local bool on_game_thread = false;

// The handler's buffers: static, so the handler's own stack stays small.
std::array<char, std::size_t{64} * 1024> section_buffer;
std::array<char, std::size_t{8} * 1024> line_buffer;
std::array<char, std::size_t{64} * 1024> copy_buffer;
CrashWriter section_out{section_buffer};

// ---- time, without the C library's calendar (not safe in a handler) -------------------------

struct UtcTime {
    std::int64_t year;
    unsigned month, day, hour, minute, second;
};

// Howard Hinnant's civil_from_days.
UtcTime utc_time(std::int64_t seconds) {
    const std::int64_t days = (seconds >= 0 ? seconds : seconds - 86399) / 86400;
    const std::int64_t of_day = seconds - days * 86400;
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto day_of_era = static_cast<unsigned>(z - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const unsigned day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const unsigned shifted_month = (5 * day_of_year + 2) / 153;
    const unsigned month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    return {static_cast<std::int64_t>(year_of_era) + era * 400 + (month <= 2 ? 1 : 0),
            month,
            day_of_year - (153 * shifted_month + 2) / 5 + 1,
            static_cast<unsigned>(of_day / 3600),
            static_cast<unsigned>(of_day / 60 % 60),
            static_cast<unsigned>(of_day % 60)};
}

std::int64_t seconds_now() {
#ifdef _WIN32
    return static_cast<std::int64_t>(std::time(nullptr));
#else
    timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return 0;
    return now.tv_sec;
#endif
}

// ---- files, with calls that are safe in a signal handler ------------------------------------

#ifdef _WIN32
int file_create(const char *path) {
    return _open(path, _O_CREAT | _O_WRONLY | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
}
int file_open(const char *path) {
    return _open(path, _O_RDONLY | _O_BINARY);
}
long file_read(int file, std::span<char> into) {
    return _read(file, into.data(), static_cast<unsigned>(into.size()));
}
long file_write_some(int file, std::string_view text) {
    return _write(file, text.data(), static_cast<unsigned>(text.size()));
}
void file_close(int file) {
    _close(file);
}
bool directory_make(const char *path) {
    return _mkdir(path) == 0;
}
std::uint64_t process_id() {
    return static_cast<std::uint64_t>(_getpid());
}
#else
int file_create(const char *path) {
    return open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
}
int file_open(const char *path) {
    return open(path, O_RDONLY | O_CLOEXEC);
}
long file_read(int file, std::span<char> into) {
    ssize_t count = 0;
    do {
        count = read(file, into.data(), into.size());
    } while (count < 0 && errno == EINTR);
    return static_cast<long>(count);
}
long file_write_some(int file, std::string_view text) {
    ssize_t count = 0;
    do {
        count = write(file, text.data(), text.size());
    } while (count < 0 && errno == EINTR);
    return static_cast<long>(count);
}
void file_close(int file) {
    close(file);
}
bool directory_make(const char *path) {
    return mkdir(path, 0755) == 0;
}
std::uint64_t process_id() {
    return static_cast<std::uint64_t>(getpid());
}
#endif

bool file_write(int file, std::string_view text) {
    while (!text.empty()) {
        const long count = file_write_some(file, text);
        if (count <= 0)
            return false;
        text.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
}

bool file_copy(const char *from, const char *to) {
    const int source = file_open(from);
    if (source < 0)
        return false;
    const int target = file_create(to);
    bool ok = target >= 0;
    while (ok) {
        const long count = file_read(source, copy_buffer);
        if (count <= 0) {
            ok = count == 0;
            break;
        }
        ok = file_write(target, {copy_buffer.data(), static_cast<std::size_t>(count)});
    }
    if (target >= 0)
        file_close(target);
    file_close(source);
    return ok;
}

std::string_view base_name(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// ---- the report ------------------------------------------------------------------------------

struct Fault {
    std::string_view what;     // "SIGSEGV (bad memory access)", "soft: ..."
    std::uint64_t address = 0; // the address it touched
    bool has_address = false;
};

#ifndef _WIN32
sigjmp_buf section_jump;
volatile sig_atomic_t in_section = 0;
#endif

// A section that faults on damaged state is cut short, not the report: the fatal handler
// jumps back here while in_section is set.
void run_section(const CrashSection &section) {
#ifndef _WIN32
    if (sigsetjmp(section_jump, 1) != 0) {
        in_section = 0;
        section_out.text("(stopped: reading this state faulted)\n");
        return;
    }
    in_section = 1;
#endif
    section.write(section_out);
#ifndef _WIN32
    in_section = 0;
#endif
}

class Report {
  public:
    // Makes the folder and its report.txt and writes the header; false if it cannot.
    bool open(const Fault &fault) {
        const UtcTime time = utc_time(seconds_now());
        folder_.clear();
        folder_.add(state.directory.view());
        folder_.add("/crash-");
        folder_.add_number(static_cast<std::uint64_t>(time.year), 4);
        folder_.add_number(time.month, 2);
        folder_.add_number(time.day, 2);
        folder_.add("-");
        folder_.add_number(time.hour, 2);
        folder_.add_number(time.minute, 2);
        folder_.add_number(time.second, 2);
        folder_.add("-");
        folder_.add_number(process_id());
        if (const int serial = state.reports.fetch_add(1); serial > 0) {
            folder_.add("-");
            folder_.add_number(static_cast<std::uint64_t>(serial));
        }
        file_ = folder_;
        file_.add("/");
        file_.add(crash_report::file_name);
        if (folder_.overflowed || file_.overflowed || !directory_make(folder_.c_str()))
            return false;
        descriptor_ = file_create(file_.c_str());
        if (descriptor_ < 0)
            return false;
        header(fault, time);
        return true;
    }

    void write(std::string_view text) {
        if (descriptor_ >= 0 && !file_write(descriptor_, text)) {
            file_close(descriptor_);
            descriptor_ = -1;
        }
    }

    void events() {
        write("== events (oldest first)\n");
        const std::uint64_t added = state.events_added.load(std::memory_order_acquire);
        const std::uint64_t first = added > event_limit ? added - event_limit : 0;
        for (std::uint64_t at = first; at < added; ++at) {
            const Event &event = state.events[at % event_limit];
            write({event.text.data(), std::min(event.length, event.text.size())});
            write("\n");
        }
        if (added == 0)
            write("(none)\n");
    }

    void kept_files() {
        write("== files\n");
        for (std::size_t at = 0; at < state.kept_file_count; ++at) {
            PathText &from = state.kept_files[at];
            const std::string_view name = base_name(from.view());
            PathText to = folder_;
            to.add("/");
            to.add(name);
            write(name);
            write(!to.overflowed && file_copy(from.c_str(), to.c_str()) ? " (copied)\n"
                                                                        : " (missing)\n");
        }
        if (state.kept_file_count == 0)
            write("(none)\n");
    }

    void sections() {
        for (const CrashSection &section : state.config.sections) {
            write("== ");
            write(section.title);
            write("\n");
            section_out = CrashWriter(section_buffer);
            run_section(section);
            write(section_out.written());
        }
    }

    void close() {
        if (descriptor_ >= 0)
            file_close(descriptor_);
        descriptor_ = -1;
    }

    const char *folder() {
        return folder_.c_str();
    }
    const char *file() {
        return file_.c_str();
    }

  private:
    void header(const Fault &fault, const UtcTime &time) {
        CrashWriter out(line_buffer);
        out.text(state.config.tag);
        out.text(" crash report\n");
        out.line("build", state.config.build);
        out.line(crash_report::binary_name, state.binary.view());
        out.line(crash_report::pid_name, static_cast<std::int64_t>(process_id()));
        out.text("time: ");
        out.number(time.year);
        out.text("-");
        two_digits(out, time.month);
        out.text("-");
        two_digits(out, time.day);
        out.text(" ");
        two_digits(out, time.hour);
        out.text(":");
        two_digits(out, time.minute);
        out.text(":");
        two_digits(out, time.second);
        out.text(" UTC\n");
        out.line("thread", on_game_thread ? "the game's" : "not the game's");
        out.line("fault", fault.what);
        if (fault.has_address)
            out.hex("address", fault.address);
        if (state.terminate_message_length > 0)
            out.line("message", {state.terminate_message.data(), state.terminate_message_length});
        write(out.written());
    }

    static void two_digits(CrashWriter &out, unsigned value) {
        std::array<char, 20> digits{};
        out.text(decimal(value, digits, 2));
    }

    PathText folder_;
    PathText file_;
    int descriptor_ = -1;
};

// A report whose stack is already named: a soft fault, Windows, a sanitizer's death. Not from
// a signal handler.
std::optional<std::string> report_named(const Fault &fault, std::string_view stack) {
    Report report;
    if (!report.open(fault))
        return std::nullopt;
    report.write(crash_report::stack_heading);
    report.write(stack.empty() ? std::string_view("(none)\n") : stack);
    report.events();
    report.kept_files();
    report.sections();
    report.close();
    return std::string(report.folder());
}

void tell(std::string_view first, std::string_view second) {
    file_write(2, state.config.tag);
    file_write(2, first);
    file_write(2, second);
    file_write(2, "\n");
}

void keep_terminate_message(std::string_view message) {
    const std::size_t length = std::min(message.size(), state.terminate_message.size());
    std::copy_n(message.begin(), length, state.terminate_message.begin());
    state.terminate_message_length = length;
}

// std::terminate keeps what it was handed for the report, then aborts into the handler.
[[noreturn]] void on_terminate() {
    keep_terminate_message("std::terminate without an exception");
    if (const std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception &error) {
            keep_terminate_message(error.what());
        } catch (...) {
            keep_terminate_message("an exception that is not a std::exception");
        }
    }
    std::abort();
}

#ifdef _WIN32
// ---- Windows: the stack is named in process ---------------------------------------------------

LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS *pointers) {
    if (state.handling.exchange(true))
        return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD &record = *pointers->ExceptionRecord;
    std::array<char, 64> what_buffer{};
    CrashWriter what(what_buffer);
    what.text(record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? "access violation "
                                                                 : "exception ");
    what.hex("code", record.ExceptionCode);
    std::string_view what_text = what.written();
    what_text.remove_suffix(1);
    Fault fault{what_text};
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        fault.address = record.ExceptionInformation[1];
        fault.has_address = true;
    }
    if (const auto folder = report_named(fault, crash_report::named_stack_here(0)))
        tell(" crashed: report in ", *folder);
    return EXCEPTION_CONTINUE_SEARCH;
}

void on_abort(int) {
    if (!state.handling.exchange(true)) {
        if (const auto folder = report_named({"abort"}, crash_report::named_stack_here(0)))
            tell(" crashed: report in ", *folder);
    }
    std::signal(SIGABRT, SIG_DFL);
}

bool install_handlers() {
    SetUnhandledExceptionFilter(on_unhandled_exception);
    return std::signal(SIGABRT, on_abort) != SIG_ERR;
}
#else
// ---- POSIX: the stack is captured raw in the handler and named by the helper ----------------

struct FatalSignal {
    int number;
    const char *what;
    bool sanitizer_reports; // AddressSanitizer has its own report for it
};
constexpr std::array<FatalSignal, 6> fatal_signals = {{
    {SIGSEGV, "SIGSEGV (bad memory access)", true},
    {SIGBUS, "SIGBUS (bad memory access)", true},
    {SIGFPE, "SIGFPE (arithmetic fault)", true},
    {SIGILL, "SIGILL (illegal instruction)", false},
    {SIGTRAP, "SIGTRAP (trap)", false},
    {SIGABRT, "SIGABRT (aborted)", false},
}};

std::uint64_t fault_pc(void *context) {
#if defined(__linux__) && defined(__x86_64__)
    return static_cast<std::uint64_t>(
        static_cast<ucontext_t *>(context)->uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
    return static_cast<ucontext_t *>(context)->uc_mcontext.pc;
#else
    (void)context;
    return 0;
#endif
}

// The frames from the one that faulted: the handler's own and the kernel's trampoline go.
std::span<const cpptrace::frame_ptr> from_fault(std::span<const cpptrace::frame_ptr> frames,
                                                std::uint64_t pc) {
    for (std::size_t at = 0; at < frames.size(); ++at)
        if (frames[at] == pc || frames[at] + 1 == pc)
            return frames.subspan(at);
    return frames;
}

void write_frames(Report &report, std::span<const cpptrace::frame_ptr> frames) {
    static cpptrace::safe_object_frame object{};
    report.write(crash_report::frames_heading);
    for (std::size_t at = 0; at < frames.size(); ++at) {
        cpptrace::get_safe_object_frame(frames[at], &object);
        CrashWriter out(line_buffer);
        out.text("#");
        out.number(static_cast<std::int64_t>(at));
        out.text(" ");
        out.hex_value(object.raw_address);
        out.text(" ");
        out.hex_value(object.address_relative_to_object_start);
        out.text(" ");
        out.text(object.object_path);
        out.text("\n");
        report.write(out.written());
    }
}

// Starts this binary again as the helper and waits for it to name the stack in the report.
void name_stack(const char *report_file) {
    constexpr int wait_ms = 30000, poll_ms = 20;
    if (state.binary.length == 0)
        return;
    static std::array<char, 32> flag{};
    std::copy(crash_report::helper_flag.begin(), crash_report::helper_flag.end(), flag.begin());
    static PathText file;
    file.clear();
    file.add(report_file);
    const pid_t child = fork();
    if (child < 0)
        return;
    if (child == 0) {
        std::array<char *, 4> arguments = {state.binary.c_str(), flag.data(), file.c_str(),
                                           nullptr};
        execv(arguments[0], arguments.data());
        _exit(127);
    }
    int status = 0;
    for (int waited_ms = 0; waited_ms < wait_ms; waited_ms += poll_ms) {
        if (waitpid(child, &status, WNOHANG) != 0)
            return;
        const timespec pause{0, poll_ms * 1000000L};
        nanosleep(&pause, nullptr);
    }
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
}

[[noreturn]] void die(int number) {
    struct sigaction plain{};
    plain.sa_handler = SIG_DFL;
    sigemptyset(&plain.sa_mask);
    sigaction(number, &plain, nullptr);
    raise(number);
    _exit(128 + number);
}

const char *signal_what(int number) {
    for (const FatalSignal &fatal : fatal_signals)
        if (fatal.number == number)
            return fatal.what;
    return "signal";
}

void on_fatal_signal(int number, siginfo_t *info, void *context) {
    if (in_section != 0)
        siglongjmp(section_jump, 1);
    if (state.handling.exchange(true))
        die(number);
    static std::array<cpptrace::frame_ptr, max_frames> frames{};
    const std::size_t count = cpptrace::safe_generate_raw_trace(frames.data(), frames.size());
    const std::span<const cpptrace::frame_ptr> stack =
        from_fault({frames.data(), count}, fault_pc(context));
    Fault fault{signal_what(number)};
    if (number != SIGABRT && number != SIGTRAP) {
        fault.address = reinterpret_cast<std::uintptr_t>(info->si_addr);
        fault.has_address = true;
    }
    static Report report;
    if (report.open(fault)) {
        report.write(crash_report::stack_heading);
        report.write(crash_report::stack_unnamed);
        write_frames(report, stack);
        report.events();
        report.kept_files();
        report.sections();
        report.close();
        name_stack(report.file());
        tell(" crashed: report in ", report.folder());
    }
    die(number);
}

bool install_handlers() {
#ifndef VERDITE_ASAN
    // The handler runs on its own stack, so a stack overflow is reported too. AddressSanitizer
    // brings its own.
    static std::array<char, std::size_t{128} * 1024> alternate{};
    stack_t stack{};
    stack.ss_sp = alternate.data();
    stack.ss_size = alternate.size();
    if (sigaltstack(&stack, nullptr) != 0)
        return false;
#endif
    struct sigaction action{};
    action.sa_sigaction = on_fatal_signal;
    // SA_NODEFER: a fault inside a section reaches the handler again, which jumps back.
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    for (const FatalSignal &fatal : fatal_signals) {
#ifdef VERDITE_ASAN
        if (fatal.sanitizer_reports)
            continue;
#endif
        if (sigaction(fatal.number, &action, nullptr) != 0)
            return false;
    }
    return true;
}
#endif

#ifdef VERDITE_ASAN
void on_sanitizer_death() {
    if (state.handling.exchange(true))
        return;
    if (const auto folder = report_named({"sanitizer: its report is in the output above"}, {}))
        tell(" crashed: report in ", *folder);
}
#endif

// The binary's own path, for the helper; empty where there is no /proc, and on
// Android, where it is the system's app_process and the game is a library in it.
void find_binary() {
    state.binary.clear();
#if defined(__linux__) && !defined(__ANDROID__)
    std::array<char, path_capacity> path{};
    const ssize_t length = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (length > 0)
        state.binary.add({path.data(), static_cast<std::size_t>(length)});
#endif
}

// cpptrace loads what it needs on first use; that must not happen in the handler.
void warm_up() {
#ifndef _WIN32
    std::array<cpptrace::frame_ptr, 4> frames{};
    const std::size_t count = cpptrace::safe_generate_raw_trace(frames.data(), frames.size());
    static cpptrace::safe_object_frame object{};
    if (count > 0)
        cpptrace::get_safe_object_frame(frames[0], &object);
#endif
}
} // namespace

CrashWriter::CrashWriter(std::span<char> buffer) : buffer_(buffer) {}

void CrashWriter::text(std::string_view text) {
    const std::size_t count = std::min(text.size(), buffer_.size() - used_);
    std::copy_n(text.begin(), count, buffer_.begin() + static_cast<std::ptrdiff_t>(used_));
    used_ += count;
}

void CrashWriter::number(std::int64_t value) {
    if (value < 0)
        text("-");
    const std::uint64_t magnitude =
        value < 0 ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    std::array<char, 20> digits{};
    text(decimal(magnitude, digits));
}

void CrashWriter::line(std::string_view name, std::int64_t value) {
    text(name);
    text(": ");
    number(value);
    text("\n");
}

void CrashWriter::line(std::string_view name, std::string_view value) {
    text(name);
    text(": ");
    text(value);
    text("\n");
}

void CrashWriter::hex(std::string_view name, std::uint64_t value) {
    text(name);
    text(": ");
    hex_value(value);
    text("\n");
}

void CrashWriter::hex_value(std::uint64_t value) {
    std::array<char, 16> digits{};
    std::size_t at = digits.size();
    do {
        digits[--at] = hex_digits[value & 0xf];
        value >>= 4;
    } while (value != 0);
    text("0x");
    text({digits.data() + at, digits.size() - at});
}

void CrashWriter::triple(std::string_view name, std::int64_t x, std::int64_t y, std::int64_t z) {
    text(name);
    text(": ");
    number(x);
    text(", ");
    number(y);
    text(", ");
    number(z);
    text("\n");
}

std::string_view CrashWriter::written() const {
    return {buffer_.data(), used_};
}

bool crash_install(CrashConfig config) {
    std::error_code error;
    std::filesystem::create_directories(config.directory, error);
    if (error) {
        std::fprintf(stderr, "[crash] cannot make %s: %s\n", config.directory.c_str(),
                     error.message().c_str());
        return false;
    }
    if (config.directory.size() + folder_name_room >= path_capacity) {
        std::fprintf(stderr, "[crash] the report folder's path is too long: %s\n",
                     config.directory.c_str());
        return false;
    }
    state.config = std::move(config);
    state.directory.clear();
    state.directory.add(state.config.directory);
    on_game_thread = true;
    if (!state.installed) {
        find_binary();
        warm_up();
    }
    // Again on each call: a library (a test framework) may have put its own in since.
    std::set_terminate(on_terminate);
#ifdef VERDITE_ASAN
    __sanitizer_set_death_callback(on_sanitizer_death);
#endif
    if (!install_handlers()) {
        std::fprintf(stderr, "[crash] cannot install the crash handlers\n");
        return false;
    }
    state.installed = true;
    return true;
}

void crash_keep_file(std::string_view path) {
    if (state.kept_file_count == state.kept_files.size())
        return;
    PathText &kept = state.kept_files[state.kept_file_count];
    kept.clear();
    kept.add(path);
    if (!kept.overflowed)
        ++state.kept_file_count;
}

void crash_event(std::string_view text) {
    const std::uint64_t at = state.events_added.load(std::memory_order_relaxed);
    Event &event = state.events[at % event_limit];
    event.length = std::min(text.size(), event.text.size());
    std::copy_n(text.begin(), event.length, event.text.begin());
    state.events_added.store(at + 1, std::memory_order_release);
}

std::optional<std::string> crash_report_soft(std::string_view what) {
    if (!state.installed || state.soft_reports >= soft_report_limit)
        return std::nullopt;
    ++state.soft_reports;
    const std::string reason = "soft: " + std::string(what);
    crash_event(reason);
    auto folder = report_named({reason}, crash_report::named_stack_here(1));
    if (folder)
        tell(" soft fault: report in ", *folder);
    return folder;
}

} // namespace verdite
