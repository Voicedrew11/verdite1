#include <kf/platform/automap.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace kf {
namespace {
constexpr int cell_count = automap_rows * automap_columns;
constexpr int sight_radius = 9;   // cells
constexpr u8 file_magic[4] = {'K', 'F', 'A', 'M'};
constexpr u8 file_version = 1;
constexpr const char *file_suffix = ".map";

AutomapSnapshot snapshot;
u8 seen[automap_floor_count][cell_count / 8];

bool in_bounds(int x, int z) { return x >= 0 && z >= 0 && x < automap_columns && z < automap_rows; }

void mark(int floor, int x, int z) {
    const int i = z * automap_columns + x;
    seen[floor - 1][i >> 3] |= static_cast<u8>(1u << (i & 7));
}

bool opaque(AutomapCell cell) {
    // Closed doors hide what lies behind them; the door cell itself is still seen.
    return cell == AutomapCell::Wall || cell == AutomapCell::Void || cell == AutomapCell::Door;
}

// Walks the cells a ray from the player passes through (Amanatides-Woo) and
// reports whether the target is reached before a solid cell.
bool visible(float from_x, float from_z, int target_x, int target_z, const AutomapCell *cells) {
    const float to_x = target_x + 0.5f, to_z = target_z + 0.5f;
    const float dx = to_x - from_x, dz = to_z - from_z;
    int x = static_cast<int>(std::floor(from_x)), z = static_cast<int>(std::floor(from_z));
    const int step_x = dx > 0 ? 1 : -1, step_z = dz > 0 ? 1 : -1;
    const float delta_x = dx != 0 ? std::abs(1.0f / dx) : 1e30f;
    const float delta_z = dz != 0 ? std::abs(1.0f / dz) : 1e30f;
    float next_x = dx != 0 ? (dx > 0 ? (x + 1 - from_x) : (from_x - x)) * delta_x : 1e30f;
    float next_z = dz != 0 ? (dz > 0 ? (z + 1 - from_z) : (from_z - z)) * delta_z : 1e30f;
    for (int guard = 0; guard < 64; ++guard) {
        if (x == target_x && z == target_z)
            return true;
        if (!in_bounds(x, z))
            return false;
        const auto cell = cells[z * automap_columns + x];
        const bool start = guard == 0;
        if (!start && opaque(cell))
            return false;
        if (next_x < next_z) {
            next_x += delta_x;
            x += step_x;
        } else {
            next_z += delta_z;
            z += step_z;
        }
    }
    return false;
}
}

AutomapSnapshot &automap_snapshot() { return snapshot; }

void automap_published() {
    const int floor = snapshot.floor;
    if (floor < 1 || floor > automap_floor_count)
        return;
    if (snapshot.owns_map) {
        std::memset(seen[floor - 1], 0xff, sizeof seen[floor - 1]);
        return;
    }
    const float px = snapshot.player_x, pz = snapshot.player_z;
    const int cx = static_cast<int>(px), cz = static_cast<int>(pz);
    for (int z = cz - sight_radius; z <= cz + sight_radius; ++z) {
        for (int x = cx - sight_radius; x <= cx + sight_radius; ++x) {
            if (!in_bounds(x, z))
                continue;
            const float ox = x + 0.5f - px, oz = z + 0.5f - pz;
            if (ox * ox + oz * oz > (sight_radius + 0.5f) * (sight_radius + 0.5f))
                continue;
            const bool near = std::abs(x - cx) <= 1 && std::abs(z - cz) <= 1;
            if (near || visible(px, pz, x, z, snapshot.cells))
                mark(floor, x, z);
        }
    }
}

void automap_reset() {
    std::memset(seen, 0, sizeof seen);
}

bool automap_seen(int floor, int x, int z) {
    if (floor < 1 || floor > automap_floor_count || !in_bounds(x, z))
        return false;
    const int i = z * automap_columns + x;
    return (seen[floor - 1][i >> 3] >> (i & 7)) & 1;
}

void automap_save(SaveSlot slot) {
    u8 data[sizeof file_magic + 1 + sizeof seen];
    std::memcpy(data, file_magic, sizeof file_magic);
    data[sizeof file_magic] = file_version;
    std::memcpy(data + sizeof file_magic + 1, seen, sizeof seen);
    const auto result = save_side_write(slot, file_suffix, data, sizeof data);
    if (result != SaveFileResult::Ok && result != SaveFileResult::Unavailable)
        std::printf("[automap] cannot save explored map (%d)\n", static_cast<int>(result));
}

void automap_load(SaveSlot slot) {
    // A save made before the automap existed starts with nothing explored.
    automap_reset();
    u8 data[sizeof file_magic + 1 + sizeof seen];
    std::size_t size = 0;
    if (save_side_read(slot, file_suffix, data, sizeof data, &size) != SaveFileResult::Ok)
        return;
    if (size != sizeof data || std::memcmp(data, file_magic, sizeof file_magic) != 0 || data[sizeof file_magic] != file_version) {
        std::printf("[automap] explored map file is not valid; starting unexplored\n");
        return;
    }
    std::memcpy(seen, data + sizeof file_magic + 1, sizeof seen);
}
}
