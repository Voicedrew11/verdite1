// The crash helper: a second process of the same binary that names a report's raw frames, so
// the crashed one never reads debug info inside its signal handler.
#include <verdite/crash.h>

#include "crash_report.h"

#include <cpptrace/basic.hpp>

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace verdite {
namespace {

std::optional<std::string> read_text(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Replaces the file whole, so a reader never sees half of it.
bool write_text(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::path temporary = path;
    temporary += ".naming";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out.write(text.data(), static_cast<std::streamsize>(text.size())))
            return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

// The value of the header's "name: value" line.
std::string_view header_value(std::string_view text, std::string_view name) {
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        const std::string_view line = text.substr(at, end - at);
        if (line.size() > name.size() + 1 && line.starts_with(name) &&
            line.substr(name.size(), 2) == ": ")
            return line.substr(name.size() + 2);
        at = end + 1;
    }
    return {};
}

// The body of the "== <heading>" section: up to the next heading.
std::pair<std::size_t, std::size_t> section_body(std::string_view text, std::string_view heading) {
    const std::size_t at = text.find(heading);
    if (at == std::string_view::npos)
        return {std::string_view::npos, 0};
    const std::size_t begin = at + heading.size();
    const std::size_t next = text.find("\n== ", begin - 1);
    const std::size_t end = next == std::string_view::npos ? text.size() : next + 1;
    return {begin, end - begin};
}

std::optional<std::uintptr_t> parse_hex(std::string_view word) {
    if (!word.starts_with("0x"))
        return std::nullopt;
    word.remove_prefix(2);
    std::uintptr_t value = 0;
    const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value, 16);
    if (error != std::errc() || end != word.data() + word.size())
        return std::nullopt;
    return value;
}

// "#<n> 0x<address> 0x<address in its object> <object path>"
std::optional<cpptrace::object_frame> parse_frame(std::string_view line) {
    if (!line.starts_with('#'))
        return std::nullopt;
    std::array<std::string_view, 3> words;
    for (std::string_view &word : words) {
        const std::size_t space = line.find(' ');
        if (space == std::string_view::npos)
            return std::nullopt;
        word = line.substr(0, space);
        line.remove_prefix(space + 1);
    }
    const auto address = parse_hex(words[1]);
    const auto object_address = parse_hex(words[2]);
    if (!address || !object_address || line.empty())
        return std::nullopt;
    return cpptrace::object_frame{*address, *object_address, std::string(line)};
}

std::vector<cpptrace::object_frame> parse_frames(std::string_view body) {
    std::vector<cpptrace::object_frame> frames;
    for (std::size_t at = 0; at < body.size();) {
        const std::size_t end = std::min(body.find('\n', at), body.size());
        if (auto frame = parse_frame(body.substr(at, end - at)))
            frames.push_back(std::move(*frame));
        at = end + 1;
    }
    return frames;
}

std::string stack_text(const cpptrace::stacktrace &trace) {
    std::string text;
    int index = 0;
    for (const cpptrace::stacktrace_frame &frame : trace.frames) {
        text += "#" + std::to_string(index++) + " ";
        text += frame.symbol.empty() ? "??" : frame.symbol;
        if (!frame.filename.empty()) {
            text += "\n      " + frame.filename;
            if (frame.line.has_value())
                text += ":" + std::to_string(frame.line.value());
        }
        if (frame.is_inline)
            text += "  (inlined into the next)";
        text += "\n";
    }
    return text.empty() ? "(none)\n" : text;
}

bool name_report(const std::filesystem::path &path) {
    auto text = read_text(path);
    if (!text) {
        std::fprintf(stderr, "[crash] cannot read %s\n", path.string().c_str());
        return false;
    }
    const auto [frames_at, frames_length] = section_body(*text, crash_report::frames_heading);
    if (frames_at == std::string_view::npos)
        return false;
    std::vector<cpptrace::object_frame> frames =
        parse_frames(std::string_view(*text).substr(frames_at, frames_length));
    // The crashed process waits for this one, so its own image is still at /proc/<pid>/exe,
    // even if the binary has been rebuilt since it started.
    const std::string binary(header_value(*text, crash_report::binary_name));
    const std::string image =
        "/proc/" + std::string(header_value(*text, crash_report::pid_name)) + "/exe";
    std::error_code error;
    if (!binary.empty() && std::filesystem::exists(image, error))
        for (cpptrace::object_frame &frame : frames)
            if (frame.object_path == binary)
                frame.object_path = image;
    const std::string named = stack_text(cpptrace::object_trace{std::move(frames)}.resolve());
    const auto [stack_at, stack_length] = section_body(*text, crash_report::stack_heading);
    if (stack_at == std::string_view::npos)
        return false;
    text->replace(stack_at, stack_length, named);
    return write_text(path, *text);
}
} // namespace

std::string crash_report::named_stack_here(std::size_t skip) {
    return stack_text(cpptrace::generate_trace(skip + 1));
}

std::optional<int> crash_helper_run(int argc, char **argv) {
    if (argc != 3 || std::string_view(argv[1]) != crash_report::helper_flag)
        return std::nullopt;
    return name_report(std::filesystem::path(argv[2])) ? 0 : 1;
}

} // namespace verdite
