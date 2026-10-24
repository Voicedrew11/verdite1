#ifndef VERDITE_PLACES_H
#define VERDITE_PLACES_H
// Place names over a floor's grid of cells, from a few named seed cells: every
// walkable cell within reach of a seed, stepping through walkable cells only,
// takes the name of the seed it is fewest steps from (the earlier seed on a
// tie); the rest of the floor has none. Game-agnostic: the game hands in its
// grid and its seeds.
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace verdite {

struct PlaceSeed {
    int x = 0;
    int z = 0;
    std::string_view name;   // borrowed: the seed table outlives the map
};

class PlaceMap {
public:
    PlaceMap() = default;
    // walkable is columns * rows cells, [z * columns + x], nonzero where one
    // can walk. Seeds off the grid or on a solid cell name nothing.
    PlaceMap(int columns, int rows, std::span<const std::uint8_t> walkable,
             std::span<const PlaceSeed> seeds, int reach_steps);

    // The place at a cell; empty off the grid or where nothing is named.
    std::string_view at(int x, int z) const;

private:
    int columns_ = 0;
    int rows_ = 0;
    std::vector<std::string_view> names_;   // per cell
};

} // namespace verdite

#endif // VERDITE_PLACES_H
