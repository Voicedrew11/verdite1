#include "coop_death.h"

#include <algorithm>
#include <array>
#include <limits>

namespace kf::ext {
namespace {

// Up, and with a pose to see through.
bool seeable(const CoopPartyMember &member) {
    return !member.down && member.floor != 0;
}

bool seeable_in(std::span<const CoopPartyMember> room, CoopPlayerId id) {
    const auto found = std::ranges::find(room, id, &CoopPartyMember::id);
    return found != room.end() && seeable(*found);
}

CoopPlayerId first_seeable(std::span<const CoopPartyMember> room) {
    const auto found = std::ranges::find_if(room, seeable);
    return found != room.end() ? found->id : 0;
}

} // namespace

CoopPlayerId coop_eyes_choose(std::span<const CoopPartyMember> room, CoopPlayerId current, int floor, float x, float z) {
    if (current != 0 && seeable_in(room, current))
        return current;
    const CoopPartyMember *nearest = nullptr;
    float nearest_squared = std::numeric_limits<float>::max();
    for (const CoopPartyMember &member : room) {
        if (!seeable(member) || member.floor != floor)
            continue;
        const float squared = (member.x - x) * (member.x - x) + (member.z - z) * (member.z - z);
        if (squared < nearest_squared) {
            nearest_squared = squared;
            nearest = &member;
        }
    }
    return nearest ? nearest->id : first_seeable(room);
}

CoopPlayerId coop_eyes_next(std::span<const CoopPartyMember> room, CoopPlayerId current) {
    const auto at = std::ranges::find(room, current, &CoopPartyMember::id);
    if (at == room.end())
        return first_seeable(room);
    const auto after = static_cast<std::size_t>(at - room.begin()) + 1;
    for (std::size_t step = 0; step < room.size(); ++step) {
        const CoopPartyMember &member = room[(after + step) % room.size()];
        if (seeable(member))
            return member.id;
    }
    return 0;
}

bool coop_party_down(bool self_down, std::span<const CoopPartyMember> others) {
    return self_down && std::ranges::all_of(others, &CoopPartyMember::down);
}

CoopCell coop_cell_beside(CoopCell cell, const std::function<bool(CoopCell)> &free) {
    constexpr std::array<CoopCell, 8> around = {{{0, 1}, {1, 0}, {0, -1}, {-1, 0}, {1, 1}, {1, -1}, {-1, -1}, {-1, 1}}};
    for (const CoopCell step : around) {
        const CoopCell beside{cell.x + step.x, cell.z + step.z};
        if (free(beside))
            return beside;
    }
    return cell;
}

CoopFight::CoopFight(std::uint32_t calm_updates) : calm_updates_(calm_updates), since_hit_(calm_updates) {}

void CoopFight::hit() {
    since_hit_ = 0;
}

void CoopFight::update(bool chased) {
    chased_ = chased;
    if (since_hit_ < calm_updates_)
        ++since_hit_;
}

bool CoopFight::fighting() const {
    return chased_ || since_hit_ < calm_updates_;
}

}
