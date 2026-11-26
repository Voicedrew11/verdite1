#ifndef KF_PLATFORM_MAP_OCCLUSION_H
#define KF_PLATFORM_MAP_OCCLUSION_H
// Ambient occlusion baked into the map cells' corners (Display > Lighting >
// Vertex ambient occlusion). Each corner of a drawn cell's faces is darkened by
// how much of the hemisphere above it the map's own faces block nearby, the
// creases where walls meet floors and ceilings. Worked out from the meshes
// themselves for every cell when a floor loads (or when the setting is turned
// on), once per distinct layout of a cell and its neighbours, and kept until
// the next load, so it costs nothing per frame; a cell the game or a mod
// changes later is worked out when it is next drawn. Distant meshes (Detailed
// map off) are drawn without it. The renderer interpolates it across each face (Vertex::occlusion).
#include <kf/lib/types.h>

#include <span>

namespace kf {

// Setting: how dark a fully occluded corner is drawn, 0..1; 0 is off. Off in Classic mode.
inline float map_occlusion_strength = 0.0f;

// The occlusion of each corner of each FT3 and FT4 face of mesh object_index
// (in the map TMD, which must be selected) drawn at cell (col, row), turned by
// orientation (0..3, a quarter turn each): four per face in the mesh's packet
// order, a triangle's fourth unused, 0 open to 255 closed. Empty when off and
// for a distant mesh (object_index KF_MAP_MESHES_PER_BANK or more).
// Valid until the next call.
std::span<const u8> map_occlusion_cell(int col, int row, u16 object_index, int orientation);

// A floor was entered or reloaded, its grids restored: forgets the last floor's
// layouts and, when on, works out this one's. Prints how many cells and layouts, and how long.
void map_occlusion_floor_loaded();

} // namespace kf

#endif // KF_PLATFORM_MAP_OCCLUSION_H
