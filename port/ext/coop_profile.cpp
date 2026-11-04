#include "coop_profile.h"

#include <charconv>
#include <optional>
#include <cstdio>

namespace kf::ext {
namespace {

constexpr std::array<CoopColour, coop_colour_count> colours = {{
    {{"BLUE", "あお"}, 60, 84, 180},
    {{"RED", "あか"}, 170, 40, 36},
    {{"GREEN", "みどり"}, 46, 120, 54},
    {{"GOLD", "きん"}, 190, 150, 40},
    {{"WHITE", "しろ"}, 200, 196, 184},
    {{"BLACK", "くろ"}, 40, 38, 40},
    {{"PURPLE", "むらさき"}, 110, 52, 140},
    {{"ORANGE", "オレンジ"}, 200, 100, 30},
}};

constexpr std::array<CoopProfileWords, online_player_helmet_count> helmet_names = {{
    {"GREAT HELM", "グレートヘルム"},
    {"FLUTED HELM", "フルーテツドヘルム"},
}};

// A line's value after "key=", if the line is that key's.
std::optional<std::string_view> value_of(std::string_view line, std::string_view key) {
    if (line.size() <= key.size() || !line.starts_with(key) || line[key.size()] != '=')
        return std::nullopt;
    return line.substr(key.size() + 1);
}

std::optional<std::uint8_t> index_below(std::string_view text, std::size_t count) {
    unsigned value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value >= count)
        return std::nullopt;
    return static_cast<std::uint8_t>(value);
}

} // namespace

std::span<const CoopColour, coop_colour_count> coop_colours() {
    return colours;
}

std::span<const CoopProfileWords, online_player_helmet_count> coop_helmet_names() {
    return helmet_names;
}

CoopProfile coop_profile_default(CoopPlayerId id) {
    std::array<char, coop_name_most + 1> name{};
    std::snprintf(name.data(), name.size(), "PLAYER%04u", static_cast<unsigned>(id % 10000));
    return {name.data(), 0, 0};
}

bool coop_profile_valid(const CoopProfile &profile) {
    return coop_name_valid(profile.name) && profile.colour < coop_colour_count &&
           profile.helmet < online_player_helmet_count;
}

std::string coop_profile_text(const CoopProfile &profile) {
    return "name=" + profile.name + "\ncolour=" + std::to_string(profile.colour) + "\nhelmet=" +
           std::to_string(profile.helmet) + "\n";
}

CoopProfile coop_profile_read(std::string_view text, const CoopProfile &fallback) {
    CoopProfile profile = fallback;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (const auto name = value_of(line, "name"); name && coop_name_valid(*name))
            profile.name = *name;
        else if (const auto colour = value_of(line, "colour"))
            profile.colour = index_below(*colour, coop_colour_count).value_or(fallback.colour);
        else if (const auto helmet = value_of(line, "helmet"))
            profile.helmet = index_below(*helmet, online_player_helmet_count).value_or(fallback.helmet);
    }
    return profile;
}

} // namespace kf::ext
