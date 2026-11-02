#include <verdite/content_ids.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <utility>

namespace verdite {
namespace {

constexpr std::size_t max_name_chars = 64;

bool plain_name(std::string_view name) {
    if (name.empty() || name.size() > max_name_chars)
        return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.' || c == '/';
    });
}

// A line is "kind key number".
std::optional<std::array<std::string_view, 3>> split_line(std::string_view line) {
    const auto first = line.find(' ');
    if (first == std::string_view::npos)
        return std::nullopt;
    const auto second = line.find(' ', first + 1);
    if (second == std::string_view::npos)
        return std::nullopt;
    return std::array{line.substr(0, first), line.substr(first + 1, second - first - 1), line.substr(second + 1)};
}

// Digits only, as a stored number is written; ten at most, and it must fit.
std::optional<std::uint32_t> parse_number(std::string_view text) {
    constexpr std::size_t max_digits = 10;
    if (text.empty() || text.size() > max_digits)
        return std::nullopt;
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9')
            return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (value > UINT32_MAX)
        return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

} // namespace

ContentIds::ContentIds(std::filesystem::path path) : path_(std::move(path)) {
    std::ifstream in(path_);
    std::string line;
    while (std::getline(in, line)) {
        const auto parts = split_line(line);
        if (!parts || !plain_name((*parts)[0]) || !plain_name((*parts)[1]))
            continue;
        const auto id = parse_number((*parts)[2]);
        if (id)
            entries_.push_back({std::string((*parts)[0]), std::string((*parts)[1]), *id});
    }
}

std::optional<std::uint32_t> ContentIds::find(std::string_view kind, std::string_view key) const {
    const auto found = std::ranges::find_if(entries_, [&](const Entry &entry) { return entry.kind == kind && entry.key == key; });
    if (found == entries_.end())
        return std::nullopt;
    return found->id;
}

std::optional<std::uint32_t> ContentIds::assign(std::string_view kind, std::string_view key, ContentIdRange range) {
    if (!plain_name(kind) || !plain_name(key))
        return std::nullopt;
    if (const auto known = find(kind, key)) {
        if (*known >= range.first && *known - range.first < range.count)
            return known;
        return std::nullopt;
    }
    for (std::uint32_t offset = 0; offset < range.count; ++offset) {
        const std::uint32_t id = range.first + offset;
        const bool taken = std::ranges::any_of(entries_, [&](const Entry &entry) { return entry.kind == kind && entry.id == id; });
        if (taken)
            continue;
        std::ofstream out(path_, std::ios::app);
        out << kind << ' ' << key << ' ' << id << '\n';
        out.flush();
        if (!out)
            return std::nullopt;
        entries_.push_back({std::string(kind), std::string(key), id});
        return id;
    }
    return std::nullopt;
}

} // namespace verdite
