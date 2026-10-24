#include <verdite/places.h>

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace verdite;

namespace {

// A grid drawn as rows of text, '#' solid: [z][x] as the map reads top to bottom.
std::vector<std::uint8_t> grid(const std::vector<std::string> &rows) {
    std::vector<std::uint8_t> cells;
    for (const auto &row : rows)
        for (const char c : row)
            cells.push_back(c == '#' ? 0 : 1);
    return cells;
}

} // namespace

TEST_CASE("each walkable cell takes the name of the seed fewest steps away") {
    const auto cells = grid({
        ".....",
        ".....",
    });
    const std::array<PlaceSeed, 2> seeds = {{{0, 0, "West"}, {4, 0, "East"}}};
    const PlaceMap places(5, 2, cells, seeds, 10);
    CHECK(places.at(1, 1) == "West");
    CHECK(places.at(3, 1) == "East");
    SUBCASE("a tie goes to the earlier seed") { CHECK(places.at(2, 0) == "West"); }
}

TEST_CASE("names spread through walkable cells only, and only so far") {
    const auto cells = grid({
        "..#..",
        "..#..",
        ".....",
    });
    const std::array<PlaceSeed, 1> seeds = {{{0, 0, "Hall"}}};
    const PlaceMap places(5, 3, cells, seeds, 5);
    CHECK(places.at(2, 0).empty());            // solid
    CHECK(places.at(3, 2) == "Hall");          // round the wall: 5 steps
    CHECK(places.at(3, 1).empty());            // 6 steps
    CHECK(places.at(4, 0).empty());
}

TEST_CASE("seeds off the grid or on a solid cell name nothing; off the grid is nowhere") {
    const auto cells = grid({".#"});
    const std::array<PlaceSeed, 2> seeds = {{{1, 0, "Wall"}, {7, 0, "Outside"}}};
    const PlaceMap places(2, 1, cells, seeds, 10);
    CHECK(places.at(0, 0).empty());
    CHECK(places.at(1, 0).empty());
    CHECK(places.at(-1, 0).empty());
    CHECK(places.at(0, 1).empty());
}

TEST_CASE("a grid whose size does not match names nothing") {
    const auto cells = grid({"..."});
    const std::array<PlaceSeed, 1> seeds = {{{0, 0, "Room"}}};
    const PlaceMap places(4, 1, cells, seeds, 10);
    CHECK(places.at(0, 0).empty());
    CHECK(PlaceMap().at(0, 0).empty());
}
