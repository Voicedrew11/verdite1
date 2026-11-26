#pragma once
// The ray test behind kf/platform/map_occlusion.h for one face corner, with no game state.
#include <span>

namespace kf::ext {

struct Point3 {
    float x = 0, y = 0, z = 0;
};

// A triangle ready for the ray test: one corner, its two edges from it, and its bounding box.
struct OccluderTriangle {
    Point3 origin, edge1, edge2;
    Point3 low, high;
};

OccluderTriangle occluder_triangle(Point3 a, Point3 b, Point3 c);

// The share of the hemisphere above unit normal at corner that the triangles
// block within reach, 0 open to 1 closed. Rays are cosine-weighted, and each
// blocked ray counts by how near its hit is: 1 at the corner, 0 at reach, so a
// crease fades out and a small room is not dark all over. The rays start a
// little off the face and toward inside (a point of the corner's own face),
// so faces meeting the corner, and a neighbour's face overlapping this one by
// a few units, are not hit at distance 0. Safe on any thread.
float corner_occlusion(Point3 corner, Point3 normal, Point3 inside, std::span<const OccluderTriangle> triangles,
                       float reach);

} // namespace kf::ext
