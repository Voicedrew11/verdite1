#include "coop_floor_owners.h"

#include <algorithm>

namespace kf::ext {
namespace {

bool floor_valid(int floor) {
    return floor >= 1 && floor <= coop_floor_count;
}

std::size_t floor_index(int floor) {
    return static_cast<std::size_t>(floor - 1);
}

void append(std::vector<CoopFloorOrder> &orders, std::vector<CoopFloorOrder> more) {
    orders.insert(orders.end(), more.begin(), more.end());
}

} // namespace

CoopFloorOwners::CoopFloorOwners(CoopPlayerId host) {
    players_.push_back({host, 0});
}

std::vector<CoopFloorOrder> CoopFloorOwners::arrived(CoopPlayerId player, int floor, CoopPlayerId left_floor_with) {
    if (player == 0 || !floor_valid(floor))
        return {};
    auto found = std::ranges::find(players_, player, &Player::id);
    if (found == players_.end()) {
        players_.push_back({player, 0});
        found = players_.end() - 1;
    }
    const int from = found->floor;
    if (from == floor)
        return {};
    const auto before = rows_;
    found->floor = floor;
    std::vector<CoopFloorOrder> orders;
    if (from != 0)
        orders = left(player, from, left_floor_with);
    append(orders, entered(player, floor));
    append(orders, settle(before));
    return orders;
}

std::vector<CoopFloorOrder> CoopFloorOwners::gone(CoopPlayerId player) {
    const auto found = std::ranges::find(players_, player, &Player::id);
    if (found == players_.end() || found == players_.begin())
        return {};
    const int floor = found->floor;
    const auto before = rows_;
    players_.erase(found);
    if (floor == 0)
        return {};
    // Gone without a word: whatever it held or was to send is lost.
    auto orders = left(player, floor, 0);
    append(orders, settle(before));
    return orders;
}

void CoopFloorOwners::taken(CoopPlayerId player, int floor) {
    if (!floor_valid(floor))
        return;
    Row &row = rows_[floor_index(floor)];
    if (row.owner == player)
        row.awaiting = 0;
}

std::optional<CoopPlayerId> CoopFloorOwners::owner(int floor) const {
    if (!floor_valid(floor) || rows_[floor_index(floor)].owner == 0)
        return std::nullopt;
    return rows_[floor_index(floor)].owner;
}

std::optional<CoopPlayerId> CoopFloorOwners::heir(int floor) const {
    if (!floor_valid(floor) || rows_[floor_index(floor)].heir == 0)
        return std::nullopt;
    return rows_[floor_index(floor)].heir;
}

bool CoopFloorOwners::records_kept_from(CoopPlayerId player, int floor) const {
    return player != 0 && owner(floor) == player;
}

// `player` is no longer on `floor`, and sent its live floor to `sent_to`.
std::vector<CoopFloorOrder> CoopFloorOwners::left(CoopPlayerId player, int floor, CoopPlayerId sent_to) {
    Row &row = rows_[floor_index(floor)];
    if (row.awaiting == player) {
        // It was to hand the floor to the owner and left first: the live floor went with it,
        // unless the snapshot it left with went to the owner.
        row.awaiting = 0;
        if (sent_to == row.owner)
            return {};
        return {CoopRunFromRecord{floor, row.owner}};
    }
    if (row.owner != player)
        return {};
    const bool had_live_floor = row.awaiting == 0;
    row.owner = most_senior_on(floor, 0);
    row.awaiting = 0;
    if (row.owner == 0 || (had_live_floor && sent_to == row.owner))
        return {};
    return {CoopRunFromRecord{floor, row.owner}};
}

std::vector<CoopFloorOrder> CoopFloorOwners::entered(CoopPlayerId player, int floor) {
    Row &row = rows_[floor_index(floor)];
    if (row.owner == 0) {
        row.owner = player;
        return {CoopRunFromRecord{floor, player}};
    }
    if (!senior_to(player, row.owner))
        return {CoopShowFloor{floor, row.owner, player}};
    const CoopPlayerId from = row.owner;
    row.owner = player;
    // A take while one was pending: the first giver's snapshot never comes, and the floor it would
    // have been is with no one. The arriving player takes it from the record.
    if (row.awaiting != 0) {
        row.awaiting = 0;
        return {CoopRunFromRecord{floor, player}};
    }
    row.awaiting = from;
    return {CoopTakeFloor{floor, from, player}};
}

// Names every floor's heir again and says each row that changed.
std::vector<CoopFloorOrder> CoopFloorOwners::settle(const std::array<Row, coop_floor_count> &before) {
    std::vector<CoopFloorOrder> orders;
    for (int floor = 1; floor <= coop_floor_count; ++floor) {
        Row &row = rows_[floor_index(floor)];
        row.heir = row.owner == 0 ? 0 : most_senior_on(floor, row.owner);
        const Row &was = before[floor_index(floor)];
        if (row.owner != was.owner || row.heir != was.heir)
            orders.emplace_back(CoopFloorOwnership{floor, row.owner, row.heir});
    }
    return orders;
}

CoopPlayerId CoopFloorOwners::most_senior_on(int floor, CoopPlayerId except) const {
    for (const Player &player : players_)
        if (player.floor == floor && player.id != except)
            return player.id;
    return 0;
}

bool CoopFloorOwners::senior_to(CoopPlayerId a, CoopPlayerId b) const {
    const auto at_a = std::ranges::find(players_, a, &Player::id);
    const auto at_b = std::ranges::find(players_, b, &Player::id);
    return at_a < at_b;
}

}
