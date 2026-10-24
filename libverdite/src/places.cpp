#include <verdite/places.h>

#include <array>
#include <deque>

namespace verdite {
namespace {

struct Step {
    int cell = 0;
    int steps = 0;
};

constexpr std::array<std::array<int, 2>, 4> neighbours = {{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};

} // namespace

// A breadth-first walk from every seed at once, so each cell takes the seed it
// is fewest steps from.
PlaceMap::PlaceMap(int columns, int rows, std::span<const std::uint8_t> walkable,
                   std::span<const PlaceSeed> seeds, int reach_steps)
    : columns_(columns), rows_(rows) {
    if (columns <= 0 || rows <= 0 || walkable.size() != static_cast<std::size_t>(columns) * rows) {
        columns_ = rows_ = 0;
        return;
    }
    names_.assign(walkable.size(), {});
    std::deque<Step> queue;
    for (const auto &seed : seeds) {
        if (seed.x < 0 || seed.z < 0 || seed.x >= columns || seed.z >= rows)
            continue;
        const int cell = seed.z * columns + seed.x;
        if (!walkable[cell] || !names_[cell].empty())
            continue;
        names_[cell] = seed.name;
        queue.push_back({cell, 0});
    }
    while (!queue.empty()) {
        const Step from = queue.front();
        queue.pop_front();
        if (from.steps >= reach_steps)
            continue;
        const int x = from.cell % columns, z = from.cell / columns;
        for (const auto &[dx, dz] : neighbours) {
            const int nx = x + dx, nz = z + dz;
            if (nx < 0 || nz < 0 || nx >= columns || nz >= rows)
                continue;
            const int cell = nz * columns + nx;
            if (!walkable[cell] || !names_[cell].empty())
                continue;
            names_[cell] = names_[from.cell];
            queue.push_back({cell, from.steps + 1});
        }
    }
}

std::string_view PlaceMap::at(int x, int z) const {
    if (x < 0 || z < 0 || x >= columns_ || z >= rows_)
        return {};
    return names_[static_cast<std::size_t>(z) * columns_ + x];
}

} // namespace verdite
