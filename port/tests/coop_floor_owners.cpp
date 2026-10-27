#include "../ext/coop_floor_owners.h"

#include <doctest/doctest.h>

using namespace kf::ext;

namespace {

constexpr CoopPlayerId host = 10, first = 20, second = 30;

template <typename Order>
std::vector<Order> of(const std::vector<CoopFloorOrder> &orders) {
    std::vector<Order> found;
    for (const CoopFloorOrder &order : orders)
        if (const auto *one = std::get_if<Order>(&order))
            found.push_back(*one);
    return found;
}

bool restarts(const std::vector<CoopFloorOrder> &orders, int floor, CoopPlayerId to) {
    for (const CoopRunFromRecord &order : of<CoopRunFromRecord>(orders))
        if (order.floor == floor && order.to == to)
            return true;
    return false;
}

// The host and two guests on floor 1, in that order.
CoopFloorOwners three_on_floor1() {
    CoopFloorOwners table(host);
    table.arrived(host, 1, 0);
    table.arrived(first, 1, 0);
    table.arrived(second, 1, 0);
    return table;
}

} // namespace

TEST_CASE("the first on an empty floor runs it from its record") {
    CoopFloorOwners table(host);
    const auto orders = table.arrived(host, 2, 0);
    CHECK(restarts(orders, 2, host));
    const auto rows = of<CoopFloorOwnership>(orders);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].floor == 2);
    CHECK(rows[0].owner == host);
    CHECK(rows[0].heir == 0);
}

TEST_CASE("a junior arriving is shown the floor and becomes its heir") {
    CoopFloorOwners table(host);
    table.arrived(host, 1, 0);
    const auto orders = table.arrived(first, 1, 0);
    const auto shows = of<CoopShowFloor>(orders);
    REQUIRE(shows.size() == 1);
    CHECK(shows[0].owner == host);
    CHECK(shows[0].to == first);
    CHECK(of<CoopRunFromRecord>(orders).empty());
    CHECK(table.owner(1) == host);
    CHECK(table.heir(1) == first);
}

TEST_CASE("an owner walking off to its heir hands over the live floor, with no restart") {
    CoopFloorOwners table = three_on_floor1();
    const auto orders = table.arrived(host, 2, first);
    CHECK_FALSE(restarts(orders, 1, first));
    CHECK(table.owner(1) == first);
    CHECK(table.heir(1) == second);
    CHECK(table.owner(2) == host);
    CHECK(table.records_kept_from(first, 1));
    CHECK_FALSE(table.records_kept_from(host, 1));
}

TEST_CASE("a snapshot sent to an heir who had just left is lost, so the next restarts the floor") {
    CoopFloorOwners table = three_on_floor1();
    table.arrived(first, 3, 0);
    // The host walked off before it heard that the first guest left.
    const auto orders = table.arrived(host, 2, first);
    CHECK(restarts(orders, 1, second));
    CHECK(table.owner(1) == second);
}

TEST_CASE("the host arriving takes the floor from a guest, whose writes are then refused") {
    CoopFloorOwners table(host);
    table.arrived(host, 1, 0);
    table.arrived(first, 2, 0);
    CHECK(table.owner(2) == first);
    const auto orders = table.arrived(host, 2, 0);
    const auto takes = of<CoopTakeFloor>(orders);
    REQUIRE(takes.size() == 1);
    CHECK(takes[0].from == first);
    CHECK(takes[0].to == host);
    CHECK(table.owner(2) == host);
    CHECK(table.heir(2) == first);
    CHECK(table.records_kept_from(host, 2));
    CHECK_FALSE(table.records_kept_from(first, 2));
    CHECK_FALSE(table.owner(1).has_value());
}

TEST_CASE("a giver who leaves after sending its take loses nothing; one who leaves before does") {
    SUBCASE("sent") {
        CoopFloorOwners table(host);
        table.arrived(first, 2, 0);
        table.arrived(host, 2, 0);
        CHECK_FALSE(restarts(table.arrived(first, 3, host), 2, host));
    }
    SUBCASE("left before the order reached it") {
        CoopFloorOwners table(host);
        table.arrived(first, 2, 0);
        table.arrived(host, 2, 0);
        CHECK(restarts(table.arrived(first, 3, 0), 2, host));
    }
    SUBCASE("dropped after the snapshot arrived") {
        CoopFloorOwners table(host);
        table.arrived(first, 2, 0);
        table.arrived(host, 2, 0);
        table.taken(host, 2);
        const auto orders = table.gone(first);
        CHECK(of<CoopFloorOwnership>(orders).size() == 1);   // the heir's row changes
        CHECK_FALSE(restarts(orders, 2, host));
        CHECK(table.owner(2) == host);
    }
    SUBCASE("dropped before it arrived") {
        CoopFloorOwners table(host);
        table.arrived(first, 2, 0);
        table.arrived(host, 2, 0);
        CHECK(restarts(table.gone(first), 2, host));
    }
}

TEST_CASE("an owner who drops leaves the floor to the next, from the record") {
    CoopFloorOwners table(host);
    table.arrived(host, 1, 0);
    table.arrived(first, 2, 0);
    table.arrived(second, 2, 0);
    const auto orders = table.gone(first);
    CHECK(restarts(orders, 2, second));
    CHECK(table.owner(2) == second);
    CHECK_FALSE(table.heir(2).has_value());
    // The last one there dropping empties the floor.
    const auto last = table.gone(second);
    CHECK(of<CoopRunFromRecord>(last).empty());
    CHECK_FALSE(table.owner(2).has_value());
}

TEST_CASE("seniority is the order of arrival in the room, not the id") {
    constexpr CoopPlayerId early = 99, late = 5;
    CoopFloorOwners table(host);
    table.arrived(host, 1, 0);
    table.arrived(early, 3, 0);
    table.arrived(late, 4, 0);
    const auto orders = table.arrived(late, 3, 0);
    CHECK(of<CoopTakeFloor>(orders).empty());
    CHECK(of<CoopShowFloor>(orders).size() == 1);
    CHECK(table.owner(3) == early);
}

TEST_CASE("an owner walking off before its own take arrived leaves the next to the record") {
    CoopFloorOwners table(host);
    table.arrived(first, 2, 0);
    table.arrived(host, 2, 0);
    const auto orders = table.arrived(host, 1, first);
    CHECK(restarts(orders, 2, first));
    CHECK(table.owner(2) == first);
}

TEST_CASE("what is out of range changes nothing") {
    CoopFloorOwners table(host);
    CHECK(table.arrived(host, 0, 0).empty());
    CHECK(table.arrived(host, coop_floor_count + 1, 0).empty());
    CHECK(table.arrived(0, 1, 0).empty());
    table.arrived(host, 1, 0);
    CHECK(table.arrived(host, 1, 0).empty());
    CHECK(table.gone(host).empty());
    CHECK(table.owner(1) == host);
    CHECK(table.gone(first).empty());
    CHECK_FALSE(table.owner(9).has_value());
}
