// compute/mesh.hpp
//
// Tiny mesh type shared by HS heightfield meshing and marching cubes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fsd::compute {

struct Vec3 {
    float x, y, z;
};

struct Mesh {
    std::vector<Vec3>     vertices;  // position only — flat shading in viewer
    std::vector<uint32_t> indices;   // triangle list, 3 indices per triangle

    size_t vertexCount()   const { return vertices.size(); }
    size_t triangleCount() const { return indices.size() / 3; }
};

// Axis-aligned bbox for centering / scaling in the viewer.
struct Bbox {
    Vec3 lo{0, 0, 0};
    Vec3 hi{0, 0, 0};
};

inline Bbox computeBbox(const Mesh& m) {
    if (m.vertices.empty()) return {};
    Bbox b{m.vertices[0], m.vertices[0]};
    for (const auto& v : m.vertices) {
        if (v.x < b.lo.x) b.lo.x = v.x;
        if (v.y < b.lo.y) b.lo.y = v.y;
        if (v.z < b.lo.z) b.lo.z = v.z;
        if (v.x > b.hi.x) b.hi.x = v.x;
        if (v.y > b.hi.y) b.hi.y = v.y;
        if (v.z > b.hi.z) b.hi.z = v.z;
    }
    return b;
}

} // namespace fsd::compute
