#pragma once
// Who runs each floor in a room (docs/COOP_PLAN.md, "The host runs the session wherever it
// stands"): the host's table, kept only on the host, wherever the host stands. Players say which
// floor they are on; the table answers with the orders the host sends to everyone in the room, so
// every machine knows each floor's owner and heir and acts on the orders that name it. The most
// senior player on a floor owns it and the next is its heir: the host, then the guests in the
// order they first arrived. A snapshot goes straight from one player to another, never through
// the host. Rules only: no game state, no network; snapshots are carried by the session.
#include "coop_party.h"

#include <array>
#include <optional>
#include <variant>
#include <vector>

namespace kf::ext {

// A floor's row of the table, sent whenever it changes: who runs it and who takes it when its
// owner walks off. An owner leaving sends its live snapshot straight to the heir as it goes,
// without waiting for an order, and the heir runs the floor from it. 0: no one.
struct CoopFloorOwnership {
    int floor = 1;
    CoopPlayerId owner = 0;
    CoopPlayerId heir = 0;
};
// `to` runs the floor from its last record, which the host sends: no one was on it, or its live
// state was lost (its owner dropped, or a snapshot went to someone no longer there).
struct CoopRunFromRecord {
    int floor = 1;
    CoopPlayerId to = 0;
};
// `from` takes a live snapshot now and stops running the floor; `to`, more senior and just
// arrived, runs it from that snapshot, sent straight to it.
struct CoopTakeFloor {
    int floor = 1;
    CoopPlayerId from = 0;
    CoopPlayerId to = 0;
};
// `owner` sends `to`, just arrived, a live snapshot, to draw the floor's enemies as puppets;
// `owner` goes on running it.
struct CoopShowFloor {
    int floor = 1;
    CoopPlayerId owner = 0;
    CoopPlayerId to = 0;
};
using CoopFloorOrder = std::variant<CoopFloorOwnership, CoopRunFromRecord, CoopTakeFloor, CoopShowFloor>;

class CoopFloorOwners {
public:
    explicit CoopFloorOwners(CoopPlayerId host);

    // A player is now on `floor` (1 to coop_floor_count): arrived in the room there, walked onto
    // it, or, dead, watches a player on it. The host's own moves come here too. `left_floor_with`
    // is whom its machine last handed the floor it left to (0 for no one): the heir as it knew it
    // when it walked off running the floor, or the player a take sent it to. The host checks it
    // against its own table. A player told to hand over a floor it has already left (the order
    // crossed its move) ignores the order.
    std::vector<CoopFloorOrder> arrived(CoopPlayerId player, int floor, CoopPlayerId left_floor_with);
    // A guest left the room, dropped or sent away: a floor it ran, or was to hand over, goes on
    // from the record. The host leaving ends the room, so it never comes here.
    std::vector<CoopFloorOrder> gone(CoopPlayerId player);
    // The owner of `floor` runs it from the snapshot a take sent it.
    void taken(CoopPlayerId player, int floor);

    std::optional<CoopPlayerId> owner(int floor) const;
    std::optional<CoopPlayerId> heir(int floor) const;
    // Whether the host keeps a floor's records sent by `player`: only from the owner it named, so a
    // former owner's late writes are refused.
    bool records_kept_from(CoopPlayerId player, int floor) const;

private:
    struct Player {
        CoopPlayerId id = 0;
        int floor = 0;   // 0 until it first arrives
    };
    struct Row {
        CoopPlayerId owner = 0;
        CoopPlayerId heir = 0;
        CoopPlayerId awaiting = 0;   // whose snapshot the owner waits for, after a take; not sent
    };
    std::vector<CoopFloorOrder> left(CoopPlayerId player, int floor, CoopPlayerId sent_to);
    std::vector<CoopFloorOrder> entered(CoopPlayerId player, int floor);
    std::vector<CoopFloorOrder> settle(const std::array<Row, coop_floor_count> &before);
    CoopPlayerId most_senior_on(int floor, CoopPlayerId except) const;
    bool senior_to(CoopPlayerId a, CoopPlayerId b) const;

    std::vector<Player> players_;   // by seniority: the host first
    std::array<Row, coop_floor_count> rows_{};
};

}
