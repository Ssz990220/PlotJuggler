// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// Shared unit-cube geometry + a single-pass edge helper for every instanced-cube
// pass (the point-cloud "cube" shape and the voxel grid). Keeping the mesh and its
// vertex-attrib contract (location 0 = position, location 1 = outward normal) in
// one place means the two passes can't drift, and both draw an identically-edged
// cube ("unify the cube").

namespace pj::scene3d {

// One vertex of the unit cube: position (each axis +/-0.5) + outward face normal.
struct CubeVertex {
  float px, py, pz;
  float nx, ny, nz;
};

// 24-vertex unit cube (4 verts per face x 6 faces), each carrying its outward face
// normal. Winding is CCW viewed from outside the face (OpenGL default front face).
inline constexpr std::array<CubeVertex, 24> kCubeVertices = {{
    // +X face, normal (1, 0, 0)
    {0.5f, -0.5f, -0.5f, 1.0f, 0.0f, 0.0f},
    {0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f},
    {0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f},
    {0.5f, 0.5f, -0.5f, 1.0f, 0.0f, 0.0f},
    // -X face, normal (-1, 0, 0)
    {-0.5f, -0.5f, 0.5f, -1.0f, 0.0f, 0.0f},
    {-0.5f, -0.5f, -0.5f, -1.0f, 0.0f, 0.0f},
    {-0.5f, 0.5f, -0.5f, -1.0f, 0.0f, 0.0f},
    {-0.5f, 0.5f, 0.5f, -1.0f, 0.0f, 0.0f},
    // +Y face, normal (0, 1, 0)
    {-0.5f, 0.5f, -0.5f, 0.0f, 1.0f, 0.0f},
    {0.5f, 0.5f, -0.5f, 0.0f, 1.0f, 0.0f},
    {0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 0.0f},
    {-0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 0.0f},
    // -Y face, normal (0, -1, 0)
    {-0.5f, -0.5f, 0.5f, 0.0f, -1.0f, 0.0f},
    {0.5f, -0.5f, 0.5f, 0.0f, -1.0f, 0.0f},
    {0.5f, -0.5f, -0.5f, 0.0f, -1.0f, 0.0f},
    {-0.5f, -0.5f, -0.5f, 0.0f, -1.0f, 0.0f},
    // +Z face, normal (0, 0, 1)
    {-0.5f, -0.5f, 0.5f, 0.0f, 0.0f, 1.0f},
    {-0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f},
    {0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f},
    {0.5f, -0.5f, 0.5f, 0.0f, 0.0f, 1.0f},
    // -Z face, normal (0, 0, -1)
    {0.5f, -0.5f, -0.5f, 0.0f, 0.0f, -1.0f},
    {0.5f, 0.5f, -0.5f, 0.0f, 0.0f, -1.0f},
    {-0.5f, 0.5f, -0.5f, 0.0f, 0.0f, -1.0f},
    {-0.5f, -0.5f, -0.5f, 0.0f, 0.0f, -1.0f},
}};

inline constexpr std::array<uint8_t, 36> kCubeIndices = {{
    0,  1,  2,  0,  2,  3,   // +X
    4,  5,  6,  4,  6,  7,   // -X
    8,  9,  10, 8,  10, 11,  // +Y
    12, 13, 14, 12, 14, 15,  // -Y
    16, 17, 18, 16, 18, 19,  // +Z
    20, 21, 22, 20, 22, 23,  // -Z
}};

// GLSL helper: a single-pass darker-edge factor for the unit cube. `local` is the
// interpolated cube-corner position in [-0.5, 0.5]^3 (pass `in_corner_pos`
// straight through to the fragment shader). On a face the normal-axis distance to
// its plane is ~0; the nearest face EDGE is where the SECOND distance is also
// small, so the middle of the three distance-to-plane values is the distance to
// the nearest edge. Returns ~1 on an edge, 0 in the face interior — mix the fill
// toward a darker shade (e.g. base * 0.4) to outline each cube. The width is a
// fixed fraction of the cube, so edges scale with the cube on screen.
inline constexpr std::string_view kCubeEdgeGlsl = R"(
float cubeEdgeFactor(vec3 local) {
  vec3 d = vec3(0.5) - abs(local);
  float dmin = min(d.x, min(d.y, d.z));
  float dmax = max(d.x, max(d.y, d.z));
  float dmid = (d.x + d.y + d.z) - dmin - dmax;  // middle value = distance to nearest edge
  return 1.0 - smoothstep(0.0, 0.06, dmid);
}
)";

}  // namespace pj::scene3d
