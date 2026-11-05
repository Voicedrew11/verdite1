#include "coop_refusals.h"

#include <array>
#include <cstdio>
#include <optional>

namespace kf::ext {
namespace {

constexpr std::uint64_t log_period_ms = 1000;

constexpr std::array<const char *, coop_refusal_kinds> names = {
    "message", "frame", "pose", "floorWrites", "floorState", "character"};

struct Kind {
    std::uint64_t count = 0;
    std::uint64_t logged_at_count = 0;
    std::optional<std::uint64_t> logged_ms;
};
std::array<Kind, coop_refusal_kinds> kinds;

} // namespace

bool coop_refused(CoopRefusal what, std::uint64_t peer, std::uint64_t now_ms) {
    const auto index = static_cast<std::size_t>(what);
    Kind &kind = kinds[index];
    ++kind.count;
    if (kind.logged_ms && now_ms - *kind.logged_ms < log_period_ms)
        return false;
    std::printf("[coop] refused %s from %016llx (%llu since the last line)\n", names[index],
        static_cast<unsigned long long>(peer), static_cast<unsigned long long>(kind.count - kind.logged_at_count));
    kind.logged_ms = now_ms;
    kind.logged_at_count = kind.count;
    return true;
}

std::uint64_t coop_refused_count(CoopRefusal what) {
    return kinds[static_cast<std::size_t>(what)].count;
}

std::string coop_refused_json() {
    std::string text = "\"refused\":{";
    for (std::size_t i = 0; i < coop_refusal_kinds; ++i)
        text += (i == 0 ? "\"" : ",\"") + std::string(names[i]) + "\":" + std::to_string(kinds[i].count);
    return text + "}";
}

void coop_refused_forget() {
    kinds = {};
}

}
