#include <verdite/crash.h>

#include "crash_report.h"

#include <cpptrace/basic.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>
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
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#include <unistd.h>

#include <csetjmp>
#include <csignal>
#endif

// Android's way of taking a stack; VERDITE_CRASH_FRAME_POINTERS tries it on a desktop's tests.
#if defined(__ANDROID__) || defined(VERDITE_CRASH_FRAME_POINTERS)
#define VERDITE_WALK_FRAME_POINTERS 1
#include <dlfcn.h>
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
constexpr int soft_report_limit = 8;
constexpr std::size_t kept_report_limit = 20;
constexpr std::size_t max_frames = 100;
constexpr int hang_tick_ms = 250;
constexpr std::string_view crash_prefix = "crash-";
constexpr std::string_view hang_prefix = "hang-";
constexpr std::string_view unseen_name = "unseen"; // in a report's folder until the player is told
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
    bool window_enabled = false;
#ifdef _WIN32
    HANDLE game_thread = nullptr;
#else
    pthread_t game_thread{};
#endif
};
State state;
thread_local bool on_game_thread = false;

// Declared after state, so its thread has stopped before state goes at exit.
struct HangWatch {
    std::atomic<std::uint64_t> beats{0};
    std::atomic<int> pauses{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> asked{false};    // the watch thread to the game thread: report yourself
    std::atomic<bool> answered{false}; // and back; folder and file are then set
    int hang_ms = 0;
    PathText folder;
    PathText file;
    std::array<char, 128> what{};
    std::size_t what_length = 0;
    std::thread thread;

    ~HangWatch() {
        stop.store(true);
        if (thread.joinable())
            thread.join();
    }
};
HangWatch hang_watch;

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
// Paths are UTF-8 here, and a user folder may be outside the code page.
std::array<wchar_t, path_capacity> wide(const char *path) {
    std::array<wchar_t, path_capacity> text{};
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, text.data(), static_cast<int>(text.size())) == 0)
        text[0] = L'\0';
    return text;
}
int file_create(const char *path) {
    return _wopen(wide(path).data(), _O_CREAT | _O_WRONLY | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
}
int file_open(const char *path) {
    return _wopen(wide(path).data(), _O_RDONLY | _O_BINARY);
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
    return _wmkdir(wide(path).data()) == 0;
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

std::filesystem::path path_of(std::string_view utf8) {
    return {std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size())};
}

std::string_view base_name(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// ---- the report ------------------------------------------------------------------------------

struct RegisterValue {
    const char *name;
    std::uint64_t value;
};

struct Fault {
    std::string_view what;     // "SIGSEGV (bad memory access)", "soft: ..."
    std::uint64_t address = 0; // the address it touched
    bool has_address = false;
    std::string_view folder_prefix = crash_prefix;
    bool tell_player = true;       // a fault the game survives has nothing to tell
    bool about_game_thread = false; // written by another thread on the game thread's behalf
};

#ifndef _WIN32
sigjmp_buf section_jump;
volatile sig_atomic_t in_section = 0;
pthread_t section_thread{};
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
    section_thread = pthread_self();
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
        folder_.add("/");
        folder_.add(fault.folder_prefix);
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
        if (fault.tell_player)
            mark_unseen();
        header(fault, time);
        return true;
    }

    void write(std::string_view text) {
        if (descriptor_ >= 0 && !file_write(descriptor_, text)) {
            file_close(descriptor_);
            descriptor_ = -1;
        }
    }

    void registers(std::span<const RegisterValue> values) {
        write("== registers\n");
        for (const RegisterValue &value : values) {
            std::array<char, 64> line{};
            CrashWriter out(line);
            out.hex(value.name, value.value);
            write(out.written());
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
    void mark_unseen() {
        PathText marker = folder_;
        marker.add("/");
        marker.add(unseen_name);
        if (marker.overflowed)
            return;
        const int file = file_create(marker.c_str());
        if (file >= 0)
            file_close(file);
    }

    void header(const Fault &fault, const UtcTime &time) {
        // Its own buffer: a hang's report may be written while the game thread is stuck in a handler.
        std::array<char, 2048> buffer{};
        CrashWriter out(buffer);
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
        out.line("thread", on_game_thread || fault.about_game_thread ? "the game's" : "not the game's");
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
// ---- Windows: the stack is walked from the thread's context and named by the helper ----------

std::array<RegisterValue, 17> registers_of(const CONTEXT &context) {
    return {{{"rax", context.Rax}, {"rbx", context.Rbx}, {"rcx", context.Rcx}, {"rdx", context.Rdx},
             {"rsi", context.Rsi}, {"rdi", context.Rdi}, {"rbp", context.Rbp}, {"rsp", context.Rsp},
             {"r8", context.R8},   {"r9", context.R9},   {"r10", context.R10}, {"r11", context.R11},
             {"r12", context.R12}, {"r13", context.R13}, {"r14", context.R14}, {"r15", context.R15},
             {"rip", context.Rip}}};
}

// Allocates nothing, so it may run with the game thread suspended or the heap damaged.
std::span<const cpptrace::frame_ptr> walk_stack(CONTEXT context) {
    static std::array<cpptrace::frame_ptr, max_frames> frames{};
    std::size_t count = 0;
    while (count < frames.size() && context.Rip != 0) {
        // A return address is the instruction after the call.
        frames[count] = context.Rip - (count > 0 ? 1 : 0);
        ++count;
        DWORD64 image_base = 0;
        const RUNTIME_FUNCTION *entry = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (!entry) {
            context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
            context.Rsp += sizeof(DWORD64);
            continue;
        }
        void *handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip,
                         const_cast<RUNTIME_FUNCTION *>(entry), &context, &handler_data, &establisher,
                         nullptr);
    }
    return {frames.data(), count};
}

std::string_view utf8_of(const wchar_t *text, std::span<char> into) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, into.data(), static_cast<int>(into.size()),
                                           nullptr, nullptr);
    return length > 0 ? std::string_view(into.data(), static_cast<std::size_t>(length - 1)) : std::string_view();
}

// The helper names a frame from the module's address, so it needs the offset and the module's path.
void write_frames(Report &report, std::span<const cpptrace::frame_ptr> frames) {
    report.write(crash_report::frames_heading);
    for (std::size_t at = 0; at < frames.size(); ++at) {
        HMODULE module = nullptr;
        std::array<wchar_t, path_capacity> name{};
        static std::array<char, path_capacity * 3> name_utf8{};
        std::string_view path;
        std::uint64_t offset = 0;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(frames[at]), &module) &&
            GetModuleFileNameW(module, name.data(), static_cast<DWORD>(name.size())) != 0) {
            path = utf8_of(name.data(), name_utf8);
            offset = frames[at] - reinterpret_cast<std::uintptr_t>(module);
        }
        CrashWriter out(line_buffer);
        out.text("#");
        out.number(static_cast<std::int64_t>(at));
        out.text(" ");
        out.hex_value(frames[at]);
        out.text(" ");
        out.hex_value(offset);
        out.text(" ");
        out.text(path.empty() ? std::string_view("?") : path);
        out.text("\n");
        report.write(out.written());
    }
}

// This binary again, with these arguments after its name; quoted, as a path may hold spaces.
HANDLE start_self(std::string_view flag, const char *folder_or_file, std::string_view last) {
    static std::array<wchar_t, path_capacity> binary{};
    if (GetModuleFileNameW(nullptr, binary.data(), static_cast<DWORD>(binary.size())) == 0)
        return nullptr;
    PathText narrow_arguments;
    narrow_arguments.add(flag);
    narrow_arguments.add(" \"");
    narrow_arguments.add(folder_or_file);
    narrow_arguments.add("\" ");
    narrow_arguments.add(last);
    static std::array<wchar_t, path_capacity * 2> line{};
    const int start = _snwprintf(line.data(), line.size() - 1, L"\"%ls\" ", binary.data());
    if (start < 0 || narrow_arguments.overflowed ||
        MultiByteToWideChar(CP_UTF8, 0, narrow_arguments.c_str(), -1, line.data() + start,
                            static_cast<int>(line.size()) - start) == 0)
        return nullptr;
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(binary.data(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS,
                        nullptr, nullptr, &startup, &process))
        return nullptr;
    CloseHandle(process.hThread);
    return process.hProcess;
}

void name_stack(const char *report_file) {
    constexpr DWORD wait_ms = 30000;
    const HANDLE helper = start_self(crash_report::helper_flag, report_file, "");
    if (!helper)
        return;
    if (WaitForSingleObject(helper, wait_ms) != WAIT_OBJECT_0)
        TerminateProcess(helper, 1);
    CloseHandle(helper);
}

void start_window(const char *folder) {
    if (!state.window_enabled)
        return;
    if (const HANDLE window = start_self(crash_report::window_flag, folder, "1"))
        CloseHandle(window);
}

// The frames last: walking a damaged stack can fault, and everything before is then already written.
bool write_context_report(Report &report, const Fault &fault, const CONTEXT &context) {
    if (!report.open(fault))
        return false;
    report.write(crash_report::stack_heading);
    report.write(crash_report::stack_unnamed);
    const auto registers = registers_of(context);
    report.registers(registers);
    report.events();
    report.kept_files();
    report.sections();
    write_frames(report, walk_stack(context));
    report.close();
    return true;
}

void report_and_tell(const Fault &fault, const CONTEXT &context) {
    static Report report;
    static std::atomic<bool> reporting{false};
    if (reporting.exchange(true) || !write_context_report(report, fault, context))
        return;
    name_stack(report.file());
    tell(" crashed: report in ", report.folder());
    start_window(report.folder());
}

LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS *pointers) {
    if (state.handling.exchange(true))
        return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD &record = *pointers->ExceptionRecord;
    static std::array<char, 64> what_buffer{};
    CrashWriter what(what_buffer);
    what.text(record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? "access violation "
              : record.ExceptionCode == EXCEPTION_STACK_OVERFLOW ? "stack overflow "
                                                                 : "exception ");
    what.hex("code", record.ExceptionCode);
    std::string_view what_text = what.written();
    what_text.remove_suffix(1);
    Fault fault{what_text};
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        fault.address = record.ExceptionInformation[1];
        fault.has_address = true;
    }
    report_and_tell(fault, *pointers->ContextRecord);
    return EXCEPTION_CONTINUE_SEARCH;
}

void on_abort(int) {
    if (!state.handling.exchange(true)) {
        static CONTEXT context{};
        RtlCaptureContext(&context);
        report_and_tell({"abort"}, context);
    }
    std::signal(SIGABRT, SIG_DFL);
}

bool install_handlers() {
    // Room for the filter to run after a stack overflow; and no system dialog on top of ours.
    ULONG reserve_bytes = 128 * 1024;
    SetThreadStackGuarantee(&reserve_bytes);
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetUnhandledExceptionFilter(on_unhandled_exception);
    return std::signal(SIGABRT, on_abort) != SIG_ERR;
}

bool debugger_attached() {
    return IsDebuggerPresent() != FALSE;
}

// On the hang watch's thread. The game thread is held while its state is read, so nothing here
// that takes a lock it may hold (the heap, the C library's file table) until it runs again.
bool write_hang_report(const Fault &fault) {
    static Report report;
    if (!report.open(fault))
        return false;
    bool suspended = SuspendThread(state.game_thread) != static_cast<DWORD>(-1);
    report.write(crash_report::stack_heading);
    report.write(crash_report::stack_unnamed);
    static CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (suspended && GetThreadContext(state.game_thread, &context)) {
        const auto registers = registers_of(context);
        report.registers(registers);
        report.events();
        report.sections();
        write_frames(report, walk_stack(context));
    } else {
        report.write(crash_report::frames_heading);
        report.events();
    }
    if (suspended)
        ResumeThread(state.game_thread);
    report.kept_files();
    report.close();
    hang_watch.folder.clear();
    hang_watch.folder.add(report.folder());
    hang_watch.file.clear();
    hang_watch.file.add(report.file());
    return true;
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

#ifndef VERDITE_WALK_FRAME_POINTERS
// The frames from the one that faulted: the handler's own and the kernel's trampoline go.
std::span<const cpptrace::frame_ptr> from_fault(std::span<const cpptrace::frame_ptr> frames,
                                                std::uint64_t pc) {
    for (std::size_t at = 0; at < frames.size(); ++at)
        if (frames[at] == pc || frames[at] + 1 == pc)
            return frames.subspan(at);
    return frames;
}
#endif

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

void write_registers(Report &report, void *context) {
#if defined(__linux__) && defined(__x86_64__)
    struct Named {
        const char *name;
        int index;
    };
    static constexpr std::array<Named, 17> names = {
        {{"rax", REG_RAX}, {"rbx", REG_RBX}, {"rcx", REG_RCX}, {"rdx", REG_RDX}, {"rsi", REG_RSI}, {"rdi", REG_RDI},
         {"rbp", REG_RBP}, {"rsp", REG_RSP}, {"r8", REG_R8},   {"r9", REG_R9},   {"r10", REG_R10}, {"r11", REG_R11},
         {"r12", REG_R12}, {"r13", REG_R13}, {"r14", REG_R14}, {"r15", REG_R15}, {"rip", REG_RIP}}};
    const auto &registers = static_cast<ucontext_t *>(context)->uc_mcontext.gregs;
    std::array<RegisterValue, names.size()> values{};
    for (std::size_t at = 0; at < names.size(); ++at)
        values[at] = {names[at].name, static_cast<std::uint64_t>(registers[names[at].index])};
    report.registers(values);
#elif defined(__linux__) && defined(__aarch64__)
    static constexpr std::array<const char *, 31> names = {
        "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",  "x10",
        "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21",
        "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29", "x30"};
    const auto &machine = static_cast<ucontext_t *>(context)->uc_mcontext;
    std::array<RegisterValue, names.size() + 2> values{};
    for (std::size_t at = 0; at < names.size(); ++at)
        values[at] = {names[at], machine.regs[at]};
    values[names.size()] = {"sp", machine.sp};
    values[names.size() + 1] = {"pc", machine.pc};
    report.registers(values);
#else
    (void)report;
    (void)context;
#endif
}

#ifdef VERDITE_WALK_FRAME_POINTERS
// cpptrace's unwinder is not safe in a signal handler on Android, so it gives no frames at all
// here: the frame pointers are walked instead (the Android build keeps them, CMakeLists.txt). A
// damaged chain faults into the same jump a section's does, and the frames so far stand.
std::span<const cpptrace::frame_ptr> capture_stack(void *context) {
    static std::array<cpptrace::frame_ptr, max_frames> frames{};
    static volatile std::size_t count = 0;
    count = 0;
    const auto &machine = static_cast<ucontext_t *>(context)->uc_mcontext;
    frames[count] = fault_pc(context);
    count = count + 1;
#if defined(__aarch64__)
    std::uintptr_t frame = machine.regs[29];
    // A leaf function has not saved its return address yet; one that has repeats it below.
    const std::uintptr_t link = machine.regs[30];
#else
    auto frame = static_cast<std::uintptr_t>(machine.gregs[REG_RBP]);
    const std::uintptr_t link = 0;
#endif
    if (sigsetjmp(section_jump, 1) == 0) {
        section_thread = pthread_self();
        in_section = 1;
        const auto *record = reinterpret_cast<const volatile std::uintptr_t *>(frame);
        if (link != 0 && (frame == 0 || frame % sizeof(std::uintptr_t) != 0 || record[1] != link)) {
            frames[count] = link - 1;
            count = count + 1;
        }
        while (count < frames.size() && frame != 0 && frame % sizeof(std::uintptr_t) == 0) {
            record = reinterpret_cast<const volatile std::uintptr_t *>(frame);
            const std::uintptr_t next = record[0];
            const std::uintptr_t return_address = record[1];
            if (return_address == 0)
                break;
            frames[count] = return_address - 1;
            count = count + 1;
            if (next <= frame)
                break;
            frame = next;
        }
    }
    in_section = 0;
    return {frames.data(), count};
}

// dladdr where cpptrace's own needs glibc. It takes the linker's lock, which a crash inside the
// linker would hold: rare enough, and the watchdog of the system ends such a process.
void find_object(cpptrace::frame_ptr address, cpptrace::safe_object_frame &object) {
    object.raw_address = address;
    object.address_relative_to_object_start = 0;
    object.object_path[0] = '\0';
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(address), &info) == 0 || !info.dli_fname)
        return;
    object.address_relative_to_object_start = address - reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    const std::string_view path(info.dli_fname);
    const std::size_t length = std::min(path.size(), sizeof(object.object_path) - 1);
    std::copy_n(path.begin(), length, object.object_path);
    object.object_path[length] = '\0';
}
#else
std::span<const cpptrace::frame_ptr> capture_stack(void *context) {
    static std::array<cpptrace::frame_ptr, max_frames> frames{};
    const std::size_t count = cpptrace::safe_generate_raw_trace(frames.data(), frames.size());
    return from_fault({frames.data(), count}, fault_pc(context));
}

void find_object(cpptrace::frame_ptr address, cpptrace::safe_object_frame &object) {
    cpptrace::get_safe_object_frame(address, &object);
}
#endif

void write_frames(Report &report, std::span<const cpptrace::frame_ptr> frames) {
    static cpptrace::safe_object_frame object{};
    report.write(crash_report::frames_heading);
    for (std::size_t at = 0; at < frames.size(); ++at) {
        find_object(frames[at], object);
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
#ifdef KF_IOS
    // iOS cannot spawn a crash helper. Keep raw frames for offline symbolication.
    (void)report_file;
#else
    constexpr int wait_ms = 30000, poll_ms = 20;
    if (state.binary.length == 0)
        return;
    // Its own, as a hang's report is named on the watch thread and a crash may come meanwhile.
    std::array<char, 32> flag{};
    std::copy(crash_report::helper_flag.begin(), crash_report::helper_flag.end(), flag.begin());
    PathText file;
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
#endif
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

// Becomes the crash window, in this process: its window and mouse grab go, and it is still the
// child an AppImage's runtime waits on, so the mount stays. Returns only if it cannot.
void become_window(const char *folder, int exit_code) {
#ifdef KF_IOS
    (void)folder;
    (void)exit_code;
#else
    if (!state.window_enabled || state.binary.length == 0)
        return;
    std::array<char, 32> flag{};
    std::copy(crash_report::window_flag.begin(), crash_report::window_flag.end(), flag.begin());
    PathText target;
    target.add(folder);
    PathText code;
    code.add_number(static_cast<std::uint64_t>(exit_code));
    std::array<char *, 5> arguments = {state.binary.c_str(), flag.data(), target.c_str(), code.c_str(), nullptr};
    execv(arguments[0], arguments.data());
#endif
}

void write_context_report(Report &report, std::span<const cpptrace::frame_ptr> stack, void *context) {
    report.write(crash_report::stack_heading);
    report.write(crash_report::stack_unnamed);
    write_frames(report, stack);
    write_registers(report, context);
    report.events();
    report.kept_files();
    report.sections();
    report.close();
}

void on_fatal_signal(int number, siginfo_t *info, void *context) {
    if (in_section != 0 && pthread_equal(section_thread, pthread_self()) != 0)
        siglongjmp(section_jump, 1);
    if (state.handling.exchange(true))
        die(number);
    const std::span<const cpptrace::frame_ptr> stack = capture_stack(context);
    Fault fault{signal_what(number)};
    if (number != SIGABRT && number != SIGTRAP) {
        fault.address = reinterpret_cast<std::uintptr_t>(info->si_addr);
        fault.has_address = true;
    }
    static Report report;
    if (report.open(fault)) {
        write_context_report(report, stack, context);
        name_stack(report.file());
        tell(" crashed: report in ", report.folder());
        become_window(report.folder(), 128 + number);
    }
    die(number);
}

// On the game thread, asked by the hang watch: a report with its own stack and consistent state.
void on_hang_signal(int, siginfo_t *, void *context) {
    if (!hang_watch.asked.exchange(false) || state.handling.exchange(true))
        return;
    const std::span<const cpptrace::frame_ptr> stack = capture_stack(context);
    Fault fault{{hang_watch.what.data(), hang_watch.what_length}};
    fault.folder_prefix = hang_prefix;
    static Report report;
    if (report.open(fault)) {
        write_context_report(report, stack, context);
        hang_watch.folder.clear();
        hang_watch.folder.add(report.folder());
        hang_watch.file.clear();
        hang_watch.file.add(report.file());
        hang_watch.answered.store(true);
    }
    state.handling.store(false);
}

bool debugger_attached() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line))
        if (line.starts_with("TracerPid:"))
            return line.find_first_not_of("0 \t", line.find(':') + 1) != std::string::npos;
    return false;
}

// On the hang watch's thread: asks the game thread, which writes the report itself. A report
// without its stack if it does not answer, as when it is stuck in the kernel.
bool write_hang_report(const Fault &fault) {
    constexpr int hang_answer_ms = 2000;
    hang_watch.answered.store(false);
    hang_watch.asked.store(true);
    if (pthread_kill(state.game_thread, SIGUSR2) == 0)
        for (int waited_ms = 0; waited_ms < hang_answer_ms && !hang_watch.answered.load(); waited_ms += 20)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (hang_watch.answered.load())
        return true;
    hang_watch.asked.store(false);
    Report report;
    if (!report.open(fault))
        return false;
    report.write(crash_report::stack_heading);
    report.write("(the game's thread did not answer, so its stack could not be taken)\n");
    report.events();
    report.close();
    hang_watch.folder.clear();
    hang_watch.folder.add(report.folder());
    hang_watch.file.clear();
    hang_watch.file.add(report.file());
    return true;
}

bool install_hang_signal() {
    struct sigaction action{};
    action.sa_sigaction = on_hang_signal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGUSR2, &action, nullptr) == 0;
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

// ---- hangs -----------------------------------------------------------------------------------

// On the hang watch's thread.
void report_hang(int quiet_ms) {
    CrashWriter what(hang_watch.what);
    what.text("hang: no frame for ");
    what.number(quiet_ms / 1000);
    what.text(" s (the window may only have been held, as when it is dragged)");
    hang_watch.what_length = what.written().size();
    Fault fault{{hang_watch.what.data(), hang_watch.what_length}};
    fault.folder_prefix = hang_prefix;
    fault.about_game_thread = true;
    if (!write_hang_report(fault))
        return;
    name_stack(hang_watch.file.c_str());
    tell(" stopped responding: report in ", hang_watch.folder.c_str());
}

// On the hang watch's thread. The game was only slow: the report says so and no one is told.
void report_hang_over(int quiet_ms) {
    std::error_code error;
    const std::filesystem::path path = path_of(hang_watch.folder.view());
    std::filesystem::remove(path / unseen_name, error);
    std::ofstream(path / crash_report::file_name, std::ios::binary | std::ios::app)
        << "== recovered\nframes came back after " << quiet_ms / 1000 << " s\n";
    tell(" responding again", "");
}

// On the hang watch's own thread. Ticks are counted, not the clock read, so that a machine
// that was asleep has not been hanging.
void watch_hangs() {
    std::uint64_t seen = 0;
    int quiet_ms = 0;
    bool reported = false;
    while (!hang_watch.stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(hang_tick_ms));
        const std::uint64_t beats = hang_watch.beats.load(std::memory_order_relaxed);
        if (beats != seen || hang_watch.pauses.load() > 0) {
            if (reported && beats != seen)
                report_hang_over(quiet_ms);
            seen = beats;
            quiet_ms = 0;
            reported = false;
            continue;
        }
        if (beats == 0 || reported)
            continue;
        quiet_ms += hang_tick_ms;
        if (quiet_ms < hang_watch.hang_ms || debugger_attached())
            continue;
        reported = true;
        report_hang(quiet_ms);
    }
}

void prune_reports() {
    namespace fs = std::filesystem;
    std::error_code error;
    std::vector<std::pair<fs::file_time_type, fs::path>> reports;
    for (const auto &entry : fs::directory_iterator(path_of(state.directory.view()), error)) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with(crash_prefix) && !name.starts_with(hang_prefix))
            continue;
        const auto written = entry.last_write_time(error);
        if (!error)
            reports.emplace_back(written, entry.path());
    }
    if (reports.size() <= kept_report_limit)
        return;
    std::sort(reports.begin(), reports.end());
    for (std::size_t at = 0; at + kept_report_limit < reports.size(); ++at)
        fs::remove_all(reports[at].second, error);
}

// The binary's own path, for the helper; empty where there is no /proc, and on
// Android, where it is the system's app_process and the game is a library in it.
void find_binary() {
    state.binary.clear();
#ifdef _WIN32
    std::array<wchar_t, path_capacity> path{};
    std::array<char, path_capacity> text{};
    if (GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())) != 0)
        state.binary.add(utf8_of(path.data(), text));
#elif defined(__linux__) && !defined(__ANDROID__)
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
#ifdef _WIN32
    if (!state.game_thread)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &state.game_thread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
#else
    state.game_thread = pthread_self();
#endif
    if (!state.installed) {
        find_binary();
        warm_up();
    }
    prune_reports();
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

void crash_window_enable() {
    state.window_enabled = true;
}

std::optional<CrashWindowRequest> crash_window_request(int argc, char **argv) {
    if (argc != 4 || std::string_view(argv[1]) != crash_report::window_flag)
        return std::nullopt;
    return CrashWindowRequest{argv[2], std::atoi(argv[3])};
}

void crash_report_seen(std::string_view folder) {
    std::error_code error;
    std::filesystem::remove(path_of(folder) / unseen_name, error);
}

std::optional<std::string> crash_take_unseen_report() {
    namespace fs = std::filesystem;
    std::error_code error;
    std::optional<fs::path> newest;
    fs::file_time_type newest_time{};
    for (const auto &entry : fs::directory_iterator(path_of(state.directory.view()), error)) {
        std::error_code found;
        if (!fs::exists(entry.path() / unseen_name, found))
            continue;
        // Before the marker goes: removing it moves the folder's time to now.
        const auto written = fs::last_write_time(entry.path(), found);
        fs::remove(entry.path() / unseen_name, found);
        if (!newest || written > newest_time) {
            newest = entry.path();
            newest_time = written;
        }
    }
    if (!newest)
        return std::nullopt;
    const auto text = newest->u8string();
    return std::string(text.begin(), text.end());
}

CrashReportKind crash_report_kind(std::string_view folder) {
    return base_name(folder).starts_with(hang_prefix) ? CrashReportKind::Hang : CrashReportKind::Crash;
}

std::string crash_file_url(std::string_view folder) {
    std::string url = "file://";
    if (!folder.starts_with('/'))
        url += '/'; // C:\\...
    for (const char c : folder) {
        const auto byte = static_cast<unsigned char>(c);
        const bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
                           c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':';
        if (c == '\\') {
            url += '/';
        } else if (plain) {
            url += c;
        } else {
            url += '%';
            url += "0123456789ABCDEF"[byte >> 4];
            url += "0123456789ABCDEF"[byte & 0xf];
        }
    }
    return url;
}

bool crash_hang_watch_start(int hang_ms) {
    if (!state.installed || hang_ms <= 0 || hang_watch.thread.joinable())
        return false;
#ifndef _WIN32
    if (!install_hang_signal()) {
        std::fprintf(stderr, "[crash] cannot watch for hangs\n");
        return false;
    }
#endif
    hang_watch.hang_ms = hang_ms;
    hang_watch.thread = std::thread(watch_hangs);
    return true;
}

void crash_hang_beat() {
    hang_watch.beats.fetch_add(1, std::memory_order_relaxed);
}

void crash_hang_pause() {
    hang_watch.pauses.fetch_add(1);
}

void crash_hang_resume() {
    hang_watch.pauses.fetch_sub(1);
}

std::optional<std::string> crash_report_soft(std::string_view what) {
    if (!state.installed || state.soft_reports >= soft_report_limit)
        return std::nullopt;
    ++state.soft_reports;
    const std::string reason = "soft: " + std::string(what);
    crash_event(reason);
    Fault fault{reason};
    fault.tell_player = false;
    auto folder = report_named(fault, crash_report::named_stack_here(1));
    if (folder)
        tell(" soft fault: report in ", *folder);
    return folder;
}

} // namespace verdite
