#include <kf/platform/map_occlusion.h>

#include "corner_occlusion.h"

#include <kf/game/graphics.h>
#include <kf/lib/map_data.h>
#include <kf/lib/map_types.h>
#include <kf/lib/tmd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kf {
namespace {

using ext::OccluderTriangle;
using ext::Point3;

// How far a corner looks for what blocks it: half a cell, so a corridor's floor stays open down its middle.
constexpr float occlusion_reach = 1000.0f;
constexpr float matrix_unity = 4096.0f;
constexpr float tile_size = KF_MAP_TILE_SIZE;
constexpr u8 off_map = 0xff;
// Near meshes only: a distant one (render_map_cell adds KF_MAP_MESHES_PER_BANK) is drawn unshaded.
constexpr std::size_t mesh_capacity = KF_MAP_MESHES_PER_BANK;

// All a drawn cell's occlusion depends on, wherever on the map it is: its mesh and turn, and the
// attribute, orientation and floor height relative to its own of each of the 3x3 cells around it.
using LayoutKey = std::array<u8, 38>;

struct LayoutKeyHash {
    std::size_t operator()(const LayoutKey &key) const {
        std::size_t hash = 14695981039346656037ULL;   // FNV-1a
        for (const u8 byte : key)
            hash = (hash ^ byte) * 1099511628211ULL;
        return hash;
    }
};

// A map mesh's FT3 and FT4 faces in its own frame, in packet order: what render_enqueue_map draws.
struct MeshFace {
    int corners = 0;
    std::array<SVECTOR, 4> positions{};
    std::array<u16, 4> vertices{};   // indices, so a corner several faces share is baked once
    SVECTOR normal{};                // the normal the game lights the face with
    u16 normal_index = 0;
};
struct Mesh {
    bool decoded = false;
    std::vector<MeshFace> faces;
};

// A cell to bake: where it is, and what it is drawn with.
struct LayoutCell {
    int col = 0, row = 0;
    u16 object_index = 0;
    int orientation = 0;
};

struct State {
    bool floor_baked = false;   // this floor's cells baked, or no floor is loaded
    std::unordered_map<LayoutKey, std::vector<u8>, LayoutKeyHash> layouts;
    std::array<Mesh, mesh_capacity> meshes;   // this floor's, decoded as they are first needed
};
State state;

bool on_map(int col, int row) { return col >= 0 && col < KF_MAP_COLUMNS && row >= 0 && row < KF_MAP_ROWS; }

int floor_height(int col, int row) { return map_floor_height_grid.cells[row][col]; }

// The near mesh and turn a cell is drawn with, as the grids hold them; false for an empty cell.
bool cell_mesh(int col, int row, u16 &mesh, int &orientation) {
    const u8 attribute = kf_enum_encode<u8>(map_cell_attribute_grid.cells[row][col]);
    if (attribute == kf_enum_encode<u8>(KF_MAP_ATTRIBUTE_NONE) || attribute - 1 >= KF_MAP_MESHES_PER_BANK)
        return false;
    orientation = kf_enum_encode<u8>(map_cell_orientation_grid.cells[row][col]) - 1;
    mesh = static_cast<u16>(attribute - 1);
    return orientation >= 0 && orientation < KF_VIEW_QUADRANT_COUNT;
}

// Decodes a mesh out of the map TMD (which must be selected) the first time; selects its vertices.
void decode_mesh(u16 object_index) {
    Mesh &mesh = state.meshes[object_index];
    if (mesh.decoded)
        return;
    mesh.decoded = true;
    const KfTmdContext context = tmd_context();
    const KfTmdObject object = tmd_read_object(context, object_index);
    tmd_select_object_vertices(context, object_index);
    const std::span<const SVECTOR> vertices = context.current_vertices;
    const KfTmdBytes normals = tmd_normal_bytes(context, object);
    auto stream = tmd_primitive_stream(context, object);
    while (stream.remaining != 0) {
        const KfTmdPacket packet = tmd_next_packet(stream);
        if (packet.mode != KF_TMD_MODE_FT3 && packet.mode != KF_TMD_MODE_FT4)
            continue;
        const KfTmdFaceData p = tmd_decode_face(packet, object.vertex_count);
        MeshFace face;
        face.corners = packet.mode == KF_TMD_MODE_FT4 ? 4 : 3;
        for (int i = 0; i < face.corners; ++i) {
            face.vertices[i] = p.vertices[i];
            face.positions[i] = vertices[p.vertices[i]];
        }
        face.normal_index = p.normals[0];
        face.normal = tmd_read_normal(normals, p.normals[0]);
        mesh.faces.push_back(face);
    }
}

// Every mesh a cell's layout reads: its own and its neighbours' near ones.
void decode_layout_meshes(const LayoutCell &cell) {
    decode_mesh(cell.object_index);
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            u16 mesh = 0;
            int orientation = 0;
            if (on_map(cell.col + dx, cell.row + dz) && cell_mesh(cell.col + dx, cell.row + dz, mesh, orientation))
                decode_mesh(mesh);
        }
    }
}

LayoutKey layout_key(const LayoutCell &cell) {
    LayoutKey key{};
    std::size_t at = 0;
    key[at++] = static_cast<u8>(cell.object_index);
    key[at++] = static_cast<u8>(cell.orientation);
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int col = cell.col + dx, row = cell.row + dz;
            if (!on_map(col, row)) {
                for (int i = 0; i < 4; ++i)
                    key[at++] = off_map;
                continue;
            }
            const int relative = floor_height(col, row) - floor_height(cell.col, cell.row) + 256;   // 1..511
            key[at++] = kf_enum_encode<u8>(map_cell_attribute_grid.cells[row][col]);
            key[at++] = kf_enum_encode<u8>(map_cell_orientation_grid.cells[row][col]);
            key[at++] = static_cast<u8>(relative & 0xff);
            key[at++] = static_cast<u8>(relative >> 8);
        }
    }
    return key;
}

// ---- The bake: reads the grids, the decoded meshes and the quarter-turn matrices, and writes nothing
// shared, so any thread may run it while the game thread waits. ----

// Where a cell's mesh lies in the frame of the cell being baked (its corner at the origin, its floor
// height 0): turned by a quarter-turn matrix, then moved, as render_map_cell places it.
struct CellPlacement {
    const MATRIX *turn;
    Point3 offset;
};

CellPlacement placement(int dx, int dz, int relative_height, int orientation) {
    Point3 offset{static_cast<float>(dx) * tile_size, static_cast<float>(relative_height * -KF_MAP_HEIGHT_STEP),
                  static_cast<float>(dz) * tile_size};
    if (orientation == 1 || orientation == 2)
        offset.z += tile_size;
    if (orientation == 2 || orientation == 3)
        offset.x += tile_size;
    return {&game_graphics_runtime.render_state.quadrant_matrices[orientation], offset};
}

Point3 turned(const MATRIX &turn, const SVECTOR &v) {
    const auto &m = turn.m;
    return {static_cast<float>(m[0][0] * v.vx + m[0][1] * v.vy + m[0][2] * v.vz) / matrix_unity,
            static_cast<float>(m[1][0] * v.vx + m[1][1] * v.vy + m[1][2] * v.vz) / matrix_unity,
            static_cast<float>(m[2][0] * v.vx + m[2][1] * v.vy + m[2][2] * v.vz) / matrix_unity};
}

Point3 placed(const CellPlacement &cell, const SVECTOR &v) {
    const Point3 p = turned(*cell.turn, v);
    return {p.x + cell.offset.x, p.y + cell.offset.y, p.z + cell.offset.z};
}

// Every face of the 3x3 cells around the cell, in its frame.
void gather_occluders(const LayoutCell &cell, std::vector<OccluderTriangle> &out) {
    out.clear();
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int col = cell.col + dx, row = cell.row + dz;
            u16 mesh = 0;
            int orientation = 0;
            if (!on_map(col, row) || !cell_mesh(col, row, mesh, orientation))
                continue;
            const CellPlacement at =
                placement(dx, dz, floor_height(col, row) - floor_height(cell.col, cell.row), orientation);
            for (const MeshFace &face : state.meshes[mesh].faces) {
                const Point3 a = placed(at, face.positions[0]);
                const Point3 b = placed(at, face.positions[1]);
                const Point3 c = placed(at, face.positions[2]);
                out.push_back(ext::occluder_triangle(a, b, c));
                if (face.corners == 4)
                    out.push_back(ext::occluder_triangle(b, placed(at, face.positions[3]), c));
            }
        }
    }
}

// A corner baked already in this layout: the same vertex with the same normal.
struct BakedCorner {
    u16 vertex, normal_index;
    u8 occlusion;
};

std::vector<u8> bake_layout(const LayoutCell &cell) {
    thread_local std::vector<OccluderTriangle> occluders;   // kept for its capacity
    thread_local std::vector<BakedCorner> baked;
    gather_occluders(cell, occluders);
    baked.clear();
    const CellPlacement at = placement(0, 0, 0, cell.orientation);
    std::vector<u8> out;
    out.reserve(state.meshes[cell.object_index].faces.size() * 4);
    for (const MeshFace &face : state.meshes[cell.object_index].faces) {
        std::array<Point3, 4> corner{};
        Point3 inside{};
        const float share = 1.0f / static_cast<float>(face.corners);
        for (int i = 0; i < face.corners; ++i) {
            corner[i] = placed(at, face.positions[i]);
            inside = {inside.x + corner[i].x * share, inside.y + corner[i].y * share, inside.z + corner[i].z * share};
        }
        Point3 normal = turned(*at.turn, face.normal);
        const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
        for (int i = 0; i < 4; ++i) {
            if (i >= face.corners || length <= 0.5f) {
                out.push_back(0);
                continue;
            }
            const auto same = std::ranges::find_if(baked, [&](const BakedCorner &b) {
                return b.vertex == face.vertices[i] && b.normal_index == face.normal_index;
            });
            if (same != baked.end()) {
                out.push_back(same->occlusion);
                continue;
            }
            const Point3 unit{normal.x / length, normal.y / length, normal.z / length};
            const float occlusion = ext::corner_occlusion(corner[i], unit, inside, occluders, occlusion_reach);
            const auto value = static_cast<u8>(std::lround(occlusion * 255.0f));
            baked.push_back({face.vertices[i], face.normal_index, value});
            out.push_back(value);
        }
    }
    return out;
}

// Bakes the cells on every hardware thread; out[i] is cells[i]'s. Runs while the game thread waits.
std::vector<std::vector<u8>> bake_in_parallel(const std::vector<LayoutCell> &cells) {
    std::vector<std::vector<u8>> out(cells.size());
    std::atomic<std::size_t> next{0};
    const auto work = [&] {
        for (std::size_t i = next++; i < cells.size(); i = next++)
            out[i] = bake_layout(cells[i]);
    };
    const unsigned count = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    std::vector<std::thread> workers;
    for (unsigned i = 1; i < count; ++i)
        workers.emplace_back(work);
    work();
    for (std::thread &worker : workers)
        worker.join();
    return out;
}

// The cell's layout, baked if it is new. The map TMD must be selected; selects other meshes' vertices.
const std::vector<u8> &layout_of(const LayoutCell &cell) {
    const LayoutKey key = layout_key(cell);
    if (const auto found = state.layouts.find(key); found != state.layouts.end())
        return found->second;
    decode_layout_meshes(cell);
    return state.layouts.emplace(key, bake_layout(cell)).first->second;
}

// Bakes every cell of the floor in the map TMD, if it is loaded; the selected TMD is kept.
void bake_floor() {
    const KfTmdResource map = game_graphics_runtime.tmd_state.slots[kf_enum_encode<u16>(KF_TMD_SLOT_MAP)];
    if (!map.data)
        return;
    state.floor_baked = true;
    const auto started = std::chrono::steady_clock::now();
    const KfTmdResource selected_tmd = game_graphics_runtime.tmd_state.current_tmd;
    const std::span<const SVECTOR> selected_vertices = game_graphics_runtime.current_tmd_vertices;
    tmd_select(tmd_context(), KF_TMD_SLOT_MAP);

    // Each distinct layout once, its meshes decoded here on the game thread.
    std::vector<LayoutCell> cells;
    std::vector<LayoutKey> keys;
    std::unordered_map<LayoutKey, std::size_t, LayoutKeyHash> seen;
    int cell_count = 0;
    for (int row = 0; row < KF_MAP_ROWS; ++row) {
        for (int col = 0; col < KF_MAP_COLUMNS; ++col) {
            LayoutCell cell{col, row};
            u16 mesh = 0;
            if (!cell_mesh(col, row, mesh, cell.orientation))
                continue;
            cell.object_index = mesh;
            ++cell_count;
            const LayoutKey key = layout_key(cell);
            if (!seen.emplace(key, cells.size()).second)
                continue;
            decode_layout_meshes(cell);
            cells.push_back(cell);
            keys.push_back(key);
        }
    }
    game_graphics_runtime.tmd_state.current_tmd = selected_tmd;
    game_graphics_runtime.current_tmd_vertices = selected_vertices;

    std::vector<std::vector<u8>> baked = bake_in_parallel(cells);
    for (std::size_t i = 0; i < cells.size(); ++i)
        state.layouts.emplace(keys[i], std::move(baked[i]));
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::printf("[occlusion] floor baked: %d cells, %zu layouts, %.1f ms\n", cell_count, cells.size(), elapsed_ms);
}

} // namespace

std::span<const u8> map_occlusion_cell(int col, int row, u16 object_index, int orientation) {
    if (map_occlusion_strength <= 0.0f || !on_map(col, row) || orientation < 0 ||
        orientation >= KF_VIEW_QUADRANT_COUNT || object_index >= mesh_capacity)
        return {};
    if (!state.floor_baked)   // turned on since the floor loaded
        bake_floor();
    const std::span<const SVECTOR> selected = game_graphics_runtime.current_tmd_vertices;
    const std::vector<u8> &corners = layout_of({col, row, object_index, orientation});
    game_graphics_runtime.current_tmd_vertices = selected;
    return corners;
}

void map_occlusion_floor_loaded() {
    state.floor_baked = false;
    state.layouts.clear();
    state.meshes = {};
    if (map_occlusion_strength > 0.0f)
        bake_floor();
}

} // namespace kf
