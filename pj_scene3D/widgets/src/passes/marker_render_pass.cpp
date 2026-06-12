// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/marker_render_pass.h"

#include <fmt/format.h>

#include <QOpenGLContext>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <glm/gtc/matrix_transform.hpp>
#include <numbers>
#include <optional>
#include <vector>

#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d {
namespace {

// glPolygonMode is desktop-GL only (absent from QOpenGLExtraFunctions, the GLES
// fallback), so it can't go through the polymorphic withGlFunctions. Apply it
// only when the 4.5-core functions are available; on a GLES fallback wireframe
// is silently unsupported.
void setPolygonMode(GLenum mode) {
  QOpenGLContext* context = QOpenGLContext::currentContext();
  if (context == nullptr) {
    return;
  }
  if (auto* functions = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(context); functions != nullptr) {
    functions->glPolygonMode(GL_FRONT_AND_BACK, mode);
  }
}

// Instanced solid shader. Per-vertex: pos (loc 0). Per-instance: the world
// matrix (loc 2-5, four vec4 columns) + rgba color (loc 6). One shared unit mesh
// per shape, one draw call per shape.
constexpr std::string_view kSolidVert = R"(#version 450 core
layout(location = 0) in vec3 in_pos;
layout(location = 2) in mat4 in_world;   // consumes locations 2,3,4,5
layout(location = 6) in vec4 in_color;
uniform mat4 u_view;
uniform mat4 u_proj;
out vec4 v_color;
void main() {
  gl_Position = u_proj * u_view * in_world * vec4(in_pos, 1.0);
  v_color = in_color;
}
)";

// Flat marker color: annotation geometry should show the exact marker color on
// every face, independent of normal direction or camera orientation.
constexpr std::string_view kFlatColorFrag = R"(#version 450 core
in vec4 v_color;
out vec4 frag_color;
void main() {
  frag_color = v_color;
}
)";

// Unit cube centred at the origin, side 1 (±0.5). 24 vertices (4 per face) so each
// face carries its own flat normal. Interleaved: pos.xyz, normal.xyz.
// Winding invariant: faces are CCW viewed from OUTSIDE — the instanced solid
// draws cull GL_BACK and rely on every closed solid mesh keeping this.
constexpr std::array<float, 24 * 6> kCubeVerts = {{
    0.5F,  -0.5F, -0.5F, 1,  0,  0,  0.5F,  0.5F,  -0.5F, 1,  0,  0,  0.5F,  0.5F,  0.5F,  1,  0,  0,
    0.5F,  -0.5F, 0.5F,  1,  0,  0,  -0.5F, -0.5F, 0.5F,  -1, 0,  0,  -0.5F, 0.5F,  0.5F,  -1, 0,  0,
    -0.5F, 0.5F,  -0.5F, -1, 0,  0,  -0.5F, -0.5F, -0.5F, -1, 0,  0,  -0.5F, 0.5F,  -0.5F, 0,  1,  0,
    -0.5F, 0.5F,  0.5F,  0,  1,  0,  0.5F,  0.5F,  0.5F,  0,  1,  0,  0.5F,  0.5F,  -0.5F, 0,  1,  0,
    -0.5F, -0.5F, 0.5F,  0,  -1, 0,  -0.5F, -0.5F, -0.5F, 0,  -1, 0,  0.5F,  -0.5F, -0.5F, 0,  -1, 0,
    0.5F,  -0.5F, 0.5F,  0,  -1, 0,  -0.5F, -0.5F, 0.5F,  0,  0,  1,  0.5F,  -0.5F, 0.5F,  0,  0,  1,
    0.5F,  0.5F,  0.5F,  0,  0,  1,  -0.5F, 0.5F,  0.5F,  0,  0,  1,  0.5F,  -0.5F, -0.5F, 0,  0,  -1,
    -0.5F, -0.5F, -0.5F, 0,  0,  -1, -0.5F, 0.5F,  -0.5F, 0,  0,  -1, 0.5F,  0.5F,  -0.5F, 0,  0,  -1,
}};

constexpr std::array<std::uint32_t, 36> kCubeIndices = {{
    0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
    12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
}};

// Cube edge overlay: front edges are drawn with the SAME per-instance color the
// fill used (viewer override + opacity already baked in), scaled by this factor
// and forced to alpha 1. Hidden/rear edges blend between the face color and that
// front-edge color so they remain visible without reading as foreground edges.
constexpr float kEdgeDarken = 0.6F;
constexpr float kHiddenEdgeMix = 0.45F;
constexpr float kEdgeVisibleEpsilon = 1e-4F;
constexpr GLsizei kCubeEdgeVertexCount = 24;

// Instanced cube-edge shader: same per-instance layout as kSolidVert (world at
// loc 2-5, color at loc 6) so the edge VAO can replay the instance_vbo_ contents
// the cube fill just used. Each edge endpoint also carries the two adjacent
// outward face normals and the edge center; if neither adjacent face faces the
// camera, the line is colored as a softer self-occluded edge. Fragment stage:
// kLineFrag.
constexpr std::string_view kEdgeVert = R"(#version 450 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal_a;
layout(location = 2) in mat4 in_world;   // consumes locations 2,3,4,5
layout(location = 6) in vec4 in_color;
layout(location = 7) in vec3 in_normal_b;
layout(location = 8) in vec3 in_edge_center;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_edge_darken;
uniform float u_hidden_edge_mix;
uniform float u_edge_visible_epsilon;
out vec4 v_color;
void main() {
  mat4 view_world = u_view * in_world;
  mat3 normal_matrix = transpose(inverse(mat3(view_world)));
  vec3 n_a = normalize(normal_matrix * in_normal_a);
  vec3 n_b = normalize(normal_matrix * in_normal_b);
  vec3 center_view = (view_world * vec4(in_edge_center, 1.0)).xyz;
  float center_len = length(center_view);
  vec3 to_camera = center_len > 1e-6 ? -center_view / center_len : vec3(0.0, 0.0, 1.0);
  float facing = max(dot(n_a, to_camera), dot(n_b, to_camera));
  vec3 front_color = in_color.rgb * u_edge_darken;
  vec3 hidden_color = mix(in_color.rgb, front_color, u_hidden_edge_mix);
  gl_Position = u_proj * view_world * vec4(in_pos, 1.0);
  v_color = vec4(facing > u_edge_visible_epsilon ? front_color : hidden_color, 1.0);
}
)";

// Per-instance record consumed by the solid shader (loc 2-6).
struct InstanceData {
  glm::mat4 world;
  glm::vec4 color;
};

struct Mesh {
  std::vector<float> verts;  // interleaved pos.xyz, normal.xyz
  std::vector<std::uint32_t> indices;
};

// Unit UV-sphere, radius 0.5 (matches the cube's ±0.5 extent under the same
// `model` scale, so `size` decodes identically). Normal = position direction.
// Triangles wind CCW viewed from outside (same invariant as the cube/cylinder);
// the instanced solid draws cull GL_BACK and would erase the sphere otherwise.
Mesh makeSphere(int rings, int sectors, float radius) {
  Mesh m;
  for (int r = 0; r <= rings; ++r) {
    const float phi = std::numbers::pi_v<float> * static_cast<float>(r) / static_cast<float>(rings);
    const float y = std::cos(phi);
    const float ring_r = std::sin(phi);
    for (int s = 0; s <= sectors; ++s) {
      const float theta = 2.0F * std::numbers::pi_v<float> * static_cast<float>(s) / static_cast<float>(sectors);
      const float x = ring_r * std::cos(theta);
      const float z = ring_r * std::sin(theta);
      m.verts.insert(m.verts.end(), {radius * x, radius * y, radius * z, x, y, z});
    }
  }
  const std::uint32_t stride = static_cast<std::uint32_t>(sectors + 1);
  for (std::uint32_t r = 0; r < static_cast<std::uint32_t>(rings); ++r) {
    for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(sectors); ++s) {
      const std::uint32_t a = r * stride + s;
      const std::uint32_t b = a + stride;
      m.indices.insert(m.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
    }
  }
  return m;
}

// Record the per-instance world(loc 2-5)+color(loc 6) attribs of `instance_vbo`
// into the currently bound VAO. Shared by the solid VAOs and the cube-edge VAO,
// which replays the very same InstanceData records the cube fill consumed.
void setupSolidInstanceAttribs(gl::Buffer& instance_vbo) {
  instance_vbo.bind(GL_ARRAY_BUFFER);
  withGlFunctions([](auto& functions) {
    constexpr GLsizei istride = static_cast<GLsizei>(sizeof(InstanceData));
    for (GLuint col = 0U; col < 4U; ++col) {
      const GLuint loc = 2U + col;
      functions.glEnableVertexAttribArray(loc);
      functions.glVertexAttribPointer(
          loc, 4, GL_FLOAT, GL_FALSE, istride, reinterpret_cast<const void*>(sizeof(glm::vec4) * col));
      functions.glVertexAttribDivisor(loc, 1U);
    }
    functions.glEnableVertexAttribArray(6U);
    functions.glVertexAttribPointer(
        6U, 4, GL_FLOAT, GL_FALSE, istride, reinterpret_cast<const void*>(offsetof(InstanceData, color)));
    functions.glVertexAttribDivisor(6U, 1U);
  });
}

// Wire a solid-mesh VAO: per-vertex pos/normal from `mesh_vbo`, per-instance
// world(loc 2-5)+color(loc 6) from the shared `instance_vbo`.
void setupSolidVao(
    gl::VertexArray& vao, gl::Buffer& mesh_vbo, gl::Buffer& mesh_ebo, const float* verts, GLsizeiptr vbytes,
    const std::uint32_t* indices, GLsizeiptr ibytes, gl::Buffer& instance_vbo) {
  vao.bind();
  mesh_vbo.uploadStatic(GL_ARRAY_BUFFER, verts, vbytes);
  mesh_vbo.bind(GL_ARRAY_BUFFER);
  withGlFunctions([](auto& functions) {
    constexpr GLsizei stride = static_cast<GLsizei>(sizeof(float) * 6);
    functions.glEnableVertexAttribArray(0U);
    functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    functions.glEnableVertexAttribArray(1U);
    functions.glVertexAttribPointer(
        1U, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(sizeof(float) * 3));
  });
  setupSolidInstanceAttribs(instance_vbo);
  mesh_ebo.uploadStatic(GL_ELEMENT_ARRAY_BUFFER, indices, ibytes);
  mesh_ebo.bind(GL_ELEMENT_ARRAY_BUFFER);  // record the EBO into the VAO
  vao.unbind();
}

std::array<float, static_cast<std::size_t>(kCubeEdgeVertexCount) * 12U> makeCubeEdgeVertices() {
  std::array<float, static_cast<std::size_t>(kCubeEdgeVertexCount) * 12U> out{};
  std::size_t i = 0;
  const auto emitVertex = [&](const glm::vec3& p, const glm::vec3& n_a, const glm::vec3& n_b, const glm::vec3& center) {
    const std::array<float, 12> v{p.x,   p.y,   p.z,   n_a.x,    n_a.y,    n_a.z,
                                  n_b.x, n_b.y, n_b.z, center.x, center.y, center.z};
    for (const float value : v) {
      out[i++] = value;
    }
  };
  const auto emitEdge = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& n_a, const glm::vec3& n_b) {
    const glm::vec3 center = (a + b) * 0.5F;
    emitVertex(a, n_a, n_b, center);
    emitVertex(b, n_a, n_b, center);
  };

  constexpr float h = 0.5F;
  const glm::vec3 nx{-1.0F, 0.0F, 0.0F};
  const glm::vec3 px{1.0F, 0.0F, 0.0F};
  const glm::vec3 ny{0.0F, -1.0F, 0.0F};
  const glm::vec3 py{0.0F, 1.0F, 0.0F};
  const glm::vec3 nz{0.0F, 0.0F, -1.0F};
  const glm::vec3 pz{0.0F, 0.0F, 1.0F};

  emitEdge({-h, -h, -h}, {h, -h, -h}, ny, nz);
  emitEdge({h, -h, -h}, {h, h, -h}, px, nz);
  emitEdge({h, h, -h}, {-h, h, -h}, py, nz);
  emitEdge({-h, h, -h}, {-h, -h, -h}, nx, nz);
  emitEdge({-h, -h, h}, {h, -h, h}, ny, pz);
  emitEdge({h, -h, h}, {h, h, h}, px, pz);
  emitEdge({h, h, h}, {-h, h, h}, py, pz);
  emitEdge({-h, h, h}, {-h, -h, h}, nx, pz);
  emitEdge({-h, -h, -h}, {-h, -h, h}, nx, ny);
  emitEdge({h, -h, -h}, {h, -h, h}, px, ny);
  emitEdge({h, h, -h}, {h, h, h}, px, py);
  emitEdge({-h, h, -h}, {-h, h, h}, nx, py);

  return out;
}

// Wire the cube-edge VAO: per-edge endpoint records (pos + adjacent face
// normals + edge center) from a static VBO plus the same instance attribs the
// solid VAOs use, drawn as GL_LINES.
void setupEdgeVao(gl::VertexArray& vao, gl::Buffer& edge_vbo, gl::Buffer& instance_vbo) {
  vao.bind();
  const auto edge_vertices = makeCubeEdgeVertices();
  edge_vbo.uploadStatic(GL_ARRAY_BUFFER, edge_vertices.data(), static_cast<GLsizeiptr>(sizeof(edge_vertices)));
  edge_vbo.bind(GL_ARRAY_BUFFER);
  withGlFunctions([](auto& functions) {
    constexpr GLsizei stride = static_cast<GLsizei>(sizeof(float) * 12);
    functions.glEnableVertexAttribArray(0U);  // endpoint position
    functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    functions.glEnableVertexAttribArray(1U);  // first adjacent face normal
    functions.glVertexAttribPointer(
        1U, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(sizeof(float) * 3));
    functions.glEnableVertexAttribArray(7U);  // second adjacent face normal
    functions.glVertexAttribPointer(
        7U, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(sizeof(float) * 6));
    functions.glEnableVertexAttribArray(8U);  // edge center, for stable per-edge facing
    functions.glVertexAttribPointer(
        8U, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(sizeof(float) * 9));
  });
  setupSolidInstanceAttribs(instance_vbo);
  vao.unbind();
}

// --- Cylinder / cone -------------------------------------------------------
// Own program: the per-instance taper (bottom/top radius scale) deforms the unit
// mesh in the vertex shader. Per-vertex: pos(0), taper_w(2). Per-instance:
// world(3-6), color(7), taper(8).
constexpr std::string_view kCylVert = R"(#version 450 core
layout(location = 0) in vec3 in_pos;
layout(location = 2) in float in_taper_w;   // 0 at bottom (-z face), 1 at top (+z face)
layout(location = 3) in mat4 in_world;      // consumes 3,4,5,6
layout(location = 7) in vec4 in_color;
layout(location = 8) in vec2 in_taper;      // (bottom_scale, top_scale)
uniform mat4 u_view;
uniform mat4 u_proj;
out vec4 v_color;
void main() {
  float rscale = mix(in_taper.x, in_taper.y, in_taper_w);
  vec3 p = vec3(in_pos.x * rscale, in_pos.y * rscale, in_pos.z);
  gl_Position = u_proj * u_view * in_world * vec4(p, 1.0);
  v_color = in_color;
}
)";

struct CylinderInstance {
  glm::mat4 world;
  glm::vec4 color;
  glm::vec2 taper;
};

// Unit cylinder: axis +Z, radius 0.5, height 1 (z in ±0.5). Stride-7 verts
// (pos.xyz, normal.xyz, taper_w). Side wall + 2 caps. Triangles wind CCW viewed
// from outside (same GL_BACK-culling invariant as the cube/sphere).
Mesh makeCylinder(int seg, float radius, float half_h) {
  Mesh m;
  const auto push = [&](float px, float py, float pz, float nx, float ny, float nz, float tw) {
    m.verts.insert(m.verts.end(), {px, py, pz, nx, ny, nz, tw});
  };
  const auto circle = [&](int s) {
    const float th = 2.0F * std::numbers::pi_v<float> * static_cast<float>(s) / static_cast<float>(seg);
    return std::pair<float, float>{std::cos(th), std::sin(th)};
  };

  // Side wall: bottom ring (taper_w 0) then top ring (taper_w 1).
  for (int s = 0; s <= seg; ++s) {
    auto [c, sn] = circle(s);
    push(radius * c, radius * sn, -half_h, c, sn, 0.0F, 0.0F);
  }
  for (int s = 0; s <= seg; ++s) {
    auto [c, sn] = circle(s);
    push(radius * c, radius * sn, half_h, c, sn, 0.0F, 1.0F);
  }
  const std::uint32_t ring = static_cast<std::uint32_t>(seg + 1);
  for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(seg); ++s) {
    const std::uint32_t b0 = s;
    const std::uint32_t b1 = s + 1;
    const std::uint32_t t0 = ring + s;
    const std::uint32_t t1 = ring + s + 1;
    m.indices.insert(m.indices.end(), {b0, b1, t0, t0, b1, t1});
  }

  // Bottom cap (-z).
  const std::uint32_t bc = static_cast<std::uint32_t>(m.verts.size() / 7);
  push(0.0F, 0.0F, -half_h, 0.0F, 0.0F, -1.0F, 0.0F);
  for (int s = 0; s <= seg; ++s) {
    auto [c, sn] = circle(s);
    push(radius * c, radius * sn, -half_h, 0.0F, 0.0F, -1.0F, 0.0F);
  }
  for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(seg); ++s) {
    m.indices.insert(m.indices.end(), {bc, bc + 1 + s + 1, bc + 1 + s});
  }

  // Top cap (+z).
  const std::uint32_t tc = static_cast<std::uint32_t>(m.verts.size() / 7);
  push(0.0F, 0.0F, half_h, 0.0F, 0.0F, 1.0F, 1.0F);
  for (int s = 0; s <= seg; ++s) {
    auto [c, sn] = circle(s);
    push(radius * c, radius * sn, half_h, 0.0F, 0.0F, 1.0F, 1.0F);
  }
  for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(seg); ++s) {
    m.indices.insert(m.indices.end(), {tc, tc + 1 + s, tc + 1 + s + 1});
  }
  return m;
}

void setupCylinderVao(gl::VertexArray& vao, gl::Buffer& vbo, gl::Buffer& ebo, const Mesh& mesh, gl::Buffer& inst) {
  vao.bind();
  vbo.uploadStatic(GL_ARRAY_BUFFER, mesh.verts.data(), static_cast<GLsizeiptr>(mesh.verts.size() * sizeof(float)));
  vbo.bind(GL_ARRAY_BUFFER);
  withGlFunctions([](auto& functions) {
    constexpr GLsizei stride = static_cast<GLsizei>(sizeof(float) * 7);
    functions.glEnableVertexAttribArray(0U);
    functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    functions.glEnableVertexAttribArray(1U);
    functions.glVertexAttribPointer(
        1U, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(sizeof(float) * 3));
    functions.glEnableVertexAttribArray(2U);
    functions.glVertexAttribPointer(
        2U, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(sizeof(float) * 6));
  });
  inst.bind(GL_ARRAY_BUFFER);
  withGlFunctions([](auto& functions) {
    constexpr GLsizei istride = static_cast<GLsizei>(sizeof(CylinderInstance));
    for (GLuint col = 0U; col < 4U; ++col) {
      const GLuint loc = 3U + col;
      functions.glEnableVertexAttribArray(loc);
      functions.glVertexAttribPointer(
          loc, 4, GL_FLOAT, GL_FALSE, istride, reinterpret_cast<const void*>(sizeof(glm::vec4) * col));
      functions.glVertexAttribDivisor(loc, 1U);
    }
    functions.glEnableVertexAttribArray(7U);
    functions.glVertexAttribPointer(
        7U, 4, GL_FLOAT, GL_FALSE, istride, reinterpret_cast<const void*>(offsetof(CylinderInstance, color)));
    functions.glVertexAttribDivisor(7U, 1U);
    functions.glEnableVertexAttribArray(8U);
    functions.glVertexAttribPointer(
        8U, 2, GL_FLOAT, GL_FALSE, istride, reinterpret_cast<const void*>(offsetof(CylinderInstance, taper)));
    functions.glVertexAttribDivisor(8U, 1U);
  });
  ebo.uploadStatic(
      GL_ELEMENT_ARRAY_BUFFER, mesh.indices.data(),
      static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(std::uint32_t)));
  ebo.bind(GL_ELEMENT_ARRAY_BUFFER);
  vao.unbind();
}

// --- Lines + triangles (flat color) ---------------------------------------
// One draw per batch; the VBO is re-streamed each frame. Vertices carry baked
// (override-applied) colors so no color uniforms are needed.
constexpr std::string_view kLineVert = R"(#version 450 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
uniform mat4 u_mvp;
out vec4 v_color;
void main() { gl_Position = u_mvp * vec4(in_pos, 1.0); v_color = in_color; }
)";

constexpr std::string_view kLineFrag = R"(#version 450 core
in vec4 v_color;
out vec4 frag_color;
void main() { frag_color = v_color; }
)";

constexpr std::string_view kTriVert = R"(#version 450 core
layout(location = 0) in vec3 in_pos;
layout(location = 2) in vec4 in_color;
uniform mat4 u_mvp;
out vec4 v_color;
void main() {
  gl_Position = u_mvp * vec4(in_pos, 1.0);
  v_color = in_color;
}
)";

// pos(0) + color(1), stride 7 floats.
void setupLineVao(gl::VertexArray& vao, gl::Buffer& vbo) {
  vao.bind();
  vbo.bind(GL_ARRAY_BUFFER);
  withGlFunctions([](auto& functions) {
    constexpr GLsizei stride = static_cast<GLsizei>(sizeof(float) * 7);
    functions.glEnableVertexAttribArray(0U);
    functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    functions.glEnableVertexAttribArray(1U);
    functions.glVertexAttribPointer(
        1U, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(sizeof(float) * 3));
  });
  vao.unbind();
}

// pos(0) + normal(1) + color(2), stride 10 floats.
void setupTriVao(gl::VertexArray& vao, gl::Buffer& vbo) {
  vao.bind();
  vbo.bind(GL_ARRAY_BUFFER);
  withGlFunctions([](auto& functions) {
    constexpr GLsizei stride = static_cast<GLsizei>(sizeof(float) * 10);
    functions.glEnableVertexAttribArray(0U);
    functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    functions.glEnableVertexAttribArray(1U);
    functions.glVertexAttribPointer(
        1U, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(sizeof(float) * 3));
    functions.glEnableVertexAttribArray(2U);
    functions.glVertexAttribPointer(
        2U, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(sizeof(float) * 6));
  });
  vao.unbind();
}

}  // namespace

MarkerRenderPass::MarkerRenderPass() = default;
MarkerRenderPass::~MarkerRenderPass() = default;

void MarkerRenderPass::setActive(std::shared_ptr<const DecodedSceneEntities> markers) {
  markers_ = std::move(markers);
}

void MarkerRenderPass::initializeGL() {
  initialized_ = false;

  auto result = gl::Program::fromSources(kSolidVert, kFlatColorFrag);
  if (auto* program = std::get_if<gl::Program>(&result); program != nullptr) {
    solid_program_ = std::make_unique<gl::Program>(std::move(*program));
  } else {
    fmt::print(stderr, "MarkerRenderPass solid shader error: {}\n", std::get<std::string>(result));
    solid_program_.reset();
    return;
  }

  setupSolidVao(
      cube_vao_, cube_vbo_, cube_ebo_, kCubeVerts.data(), static_cast<GLsizeiptr>(sizeof(kCubeVerts)),
      kCubeIndices.data(), static_cast<GLsizeiptr>(sizeof(kCubeIndices)), instance_vbo_);
  cube_index_count_ = static_cast<int>(kCubeIndices.size());

  const Mesh sphere = makeSphere(16, 32, 0.5F);
  setupSolidVao(
      sphere_vao_, sphere_vbo_, sphere_ebo_, sphere.verts.data(),
      static_cast<GLsizeiptr>(sphere.verts.size() * sizeof(float)), sphere.indices.data(),
      static_cast<GLsizeiptr>(sphere.indices.size() * sizeof(std::uint32_t)), instance_vbo_);
  sphere_index_count_ = static_cast<int>(sphere.indices.size());

  auto edge_result = gl::Program::fromSources(kEdgeVert, kLineFrag);
  if (auto* program = std::get_if<gl::Program>(&edge_result); program != nullptr) {
    edge_program_ = std::make_unique<gl::Program>(std::move(*program));
    setupEdgeVao(edge_vao_, edge_vbo_, instance_vbo_);
  } else {
    fmt::print(stderr, "MarkerRenderPass edge shader error: {}\n", std::get<std::string>(edge_result));
    edge_program_.reset();
  }

  auto cyl_result = gl::Program::fromSources(kCylVert, kFlatColorFrag);
  if (auto* program = std::get_if<gl::Program>(&cyl_result); program != nullptr) {
    cyl_program_ = std::make_unique<gl::Program>(std::move(*program));
    const Mesh cylinder = makeCylinder(24, 0.5F, 0.5F);
    setupCylinderVao(cyl_vao_, cyl_vbo_, cyl_ebo_, cylinder, cyl_instance_vbo_);
    cyl_index_count_ = static_cast<int>(cylinder.indices.size());
  } else {
    fmt::print(stderr, "MarkerRenderPass cylinder shader error: {}\n", std::get<std::string>(cyl_result));
    cyl_program_.reset();
  }

  // One unit arrow (points +X, tail at origin); per-marker dims baked into the
  // model matrix. Unit head diameter = 1 so a y/z scale maps directly to diameter.
  marker_arrow_.initializeGL(
      ArrowGizmo::Params{
          .length = 1.0F, .shaft_radius = 0.2F, .head_length = 0.3F, .head_radius = 0.5F, .segments = 16});

  auto line_result = gl::Program::fromSources(kLineVert, kLineFrag);
  if (auto* program = std::get_if<gl::Program>(&line_result); program != nullptr) {
    line_program_ = std::make_unique<gl::Program>(std::move(*program));
    setupLineVao(line_vao_, line_vbo_);
  } else {
    fmt::print(stderr, "MarkerRenderPass line shader error: {}\n", std::get<std::string>(line_result));
    line_program_.reset();
  }

  auto tri_result = gl::Program::fromSources(kTriVert, kFlatColorFrag);
  if (auto* program = std::get_if<gl::Program>(&tri_result); program != nullptr) {
    tri_program_ = std::make_unique<gl::Program>(std::move(*program));
    setupTriVao(tri_vao_, tri_vbo_);
  } else {
    fmt::print(stderr, "MarkerRenderPass triangle shader error: {}\n", std::get<std::string>(tri_result));
    tri_program_.reset();
  }

  initialized_ = true;
}

void MarkerRenderPass::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!visible_ || !initialized_ || solid_program_ == nullptr || markers_ == nullptr) {
    return;
  }
  const DecodedSceneEntities& batch = *markers_;
  const glm::mat4& view = view_params.view;
  const glm::mat4& proj = view_params.proj;

  // Resolve each interned frame ONCE (O(frames)). nullopt => orphan, skip its primitives.
  std::vector<std::optional<glm::mat4>> frame_world(batch.frames.size());
  for (std::size_t i = 0; i < batch.frames.size(); ++i) {
    if (auto t = frame_ctx.lookup(batch.frames[i]); t.has_value()) {
      frame_world[i] = glm::mat4(t->matrix());  // narrow dmat4 -> mat4 explicitly
    }
  }

  // Viewer overrides, applied CPU-side into the baked colors: replace rgb when
  // color-override is on, and multiply alpha by the opacity slider.
  const auto applyOverride = [this](const glm::vec4& c) {
    glm::vec4 out = overrides_.color_override ? overrides_.override_color : c;
    out.a *= overrides_.opacity;
    return out;
  };

  // Transparency: blend + drop depth writes when a viewer override makes the
  // whole batch translucent. Per-instance alpha (e.g. foxglove CubePrimitives)
  // is handled separately at the cube fill draw below.
  // Wireframe: glPolygonMode affects only filled prims (triangles/solids); GL_LINES
  // ignore it, so set it once for the whole pass and restore at the end.
  const bool translucent =
      overrides_.opacity < 0.999F || (overrides_.color_override && overrides_.override_color.a < 0.999F);
  withGlFunctions([translucent](auto& functions) {
    functions.glEnable(GL_BLEND);
    functions.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (translucent) {
      functions.glDepthMask(GL_FALSE);
    }
    // Closed CCW-outward solids (cube/sphere/cylinder) cull back faces so a
    // translucent solid tints each pixel through exactly ONE face instead of
    // order-dependent front+back alpha stacking. GL_CULL_FACE is enabled only
    // around those instanced draws — triangle lists (arbitrary user winding),
    // lines, and the ArrowGizmo are never culled — and the engine otherwise
    // assumes culling off, so every enable below is paired with a disable.
    functions.glCullFace(GL_BACK);
    functions.glFrontFace(GL_CCW);
  });
  setPolygonMode(overrides_.wireframe ? GL_LINE : GL_FILL);

  const auto buildSolids = [&](const std::vector<MarkerSolid>& list) {
    std::vector<InstanceData> out;
    out.reserve(list.size());
    for (const auto& prim : list) {
      if (prim.frame_index >= frame_world.size()) {
        continue;
      }
      const auto& fw = frame_world[prim.frame_index];
      if (!fw.has_value()) {
        continue;  // unresolved frame
      }
      out.push_back({*fw * prim.model, applyOverride(prim.color)});
    }
    return out;
  };

  const auto drawSolid = [&](gl::VertexArray& vao, int index_count, const std::vector<InstanceData>& instances) {
    if (instances.empty()) {
      return;
    }
    instance_vbo_.uploadStatic(
        GL_ARRAY_BUFFER, instances.data(), static_cast<GLsizeiptr>(instances.size() * sizeof(InstanceData)));
    vao.bind();
    withGlFunctions([index_count, &instances](auto& functions) {
      functions.glDrawElementsInstanced(
          GL_TRIANGLES, index_count, GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(instances.size()));
    });
    vao.unbind();
  };

  // --- Solids: cube + sphere (shared instanced program) ---
  const std::vector<InstanceData> cubes = buildSolids(batch.cubes);
  const std::vector<InstanceData> spheres = buildSolids(batch.spheres);

  // Foxglove CubePrimitives carry translucency as per-instance alpha (already
  // override-baked into the instance colors above), NOT via viewer overrides —
  // the pass-wide `translucent` flag alone misses the common LiDAR-boxes case.
  const bool cubes_translucent = translucent || std::any_of(cubes.begin(), cubes.end(), [](const InstanceData& inst) {
                                   return inst.color.a < 0.999F;
                                 });
  if (!cubes.empty() || !spheres.empty()) {
    solid_program_->use();
    solid_program_->setMat4("u_view", view);
    solid_program_->setMat4("u_proj", proj);
    withGlFunctions([](auto& functions) { functions.glEnable(GL_CULL_FACE); });

    // The cube FILL is polygon-offset away from the camera so the edge lines
    // (drawn at the exact, un-offset depth) win the depth test instead of
    // z-fighting the box's own faces. Offsetting the fill rather than biasing
    // the lines keeps the line depth exact against the rest of the scene, and
    // GL_POLYGON_OFFSET_FILL never perturbs line rasterization.
    //
    // A TRANSLUCENT fill additionally writes no depth: the offset only settles
    // the fight at a shared surface — it cannot bridge the box's own depth
    // extent — so a depth-written front face would occlude the rear edges and
    // break the "all 12 edges visible" contract exactly when the box is
    // see-through. Opaque fills keep writing depth: rear edges hidden by an
    // opaque box is correct occlusion, not an artifact.
    if (!cubes.empty()) {
      withGlFunctions([cubes_translucent](auto& functions) {
        functions.glEnable(GL_POLYGON_OFFSET_FILL);
        functions.glPolygonOffset(1.0F, 1.0F);
        if (cubes_translucent) {
          functions.glDepthMask(GL_FALSE);
        }
      });
      drawSolid(cube_vao_, cube_index_count_, cubes);
      withGlFunctions([translucent, cubes_translucent](auto& functions) {
        functions.glDisable(GL_POLYGON_OFFSET_FILL);
        if (cubes_translucent && !translucent) {
          functions.glDepthMask(GL_TRUE);  // back to the pass-wide mask (edges DO write depth)
        }
      });
    }

    // Cube edge overlay: replays the cube instances still in instance_vbo_, so
    // it MUST run after the cube drawSolid and before the sphere one re-uploads
    // that buffer. Edges are opaque (blend off) and never culled; the shader
    // gives self-occluded/rear edges a softer color.
    if (!cubes.empty() && edge_program_ != nullptr) {
      withGlFunctions([](auto& functions) {
        functions.glDisable(GL_BLEND);
        functions.glDisable(GL_CULL_FACE);
      });
      edge_program_->use();
      edge_program_->setMat4("u_view", view);
      edge_program_->setMat4("u_proj", proj);
      edge_program_->setFloat("u_edge_darken", kEdgeDarken);
      edge_program_->setFloat("u_hidden_edge_mix", kHiddenEdgeMix);
      edge_program_->setFloat("u_edge_visible_epsilon", kEdgeVisibleEpsilon);
      edge_vao_.bind();
      withGlFunctions([&cubes](auto& functions) {
        functions.glDrawArraysInstanced(GL_LINES, 0, kCubeEdgeVertexCount, static_cast<GLsizei>(cubes.size()));
      });
      edge_vao_.unbind();
      withGlFunctions([](auto& functions) {
        functions.glEnable(GL_BLEND);
        functions.glEnable(GL_CULL_FACE);
      });
      solid_program_->use();  // back to the solid program for the sphere draw
    }

    drawSolid(sphere_vao_, sphere_index_count_, spheres);
    withGlFunctions([](auto& functions) { functions.glDisable(GL_CULL_FACE); });
    unuseProgram();
  }

  // --- Cylinders / cones (own program, per-instance taper) ---
  if (cyl_program_ != nullptr && !batch.cylinders.empty()) {
    std::vector<CylinderInstance> cyls;
    cyls.reserve(batch.cylinders.size());
    for (const auto& cyl : batch.cylinders) {
      if (cyl.frame_index >= frame_world.size()) {
        continue;
      }
      const auto& fw = frame_world[cyl.frame_index];
      if (!fw.has_value()) {
        continue;
      }
      cyls.push_back({*fw * cyl.model, applyOverride(cyl.color), {cyl.bottom_scale, cyl.top_scale}});
    }
    if (!cyls.empty()) {
      cyl_instance_vbo_.uploadStatic(
          GL_ARRAY_BUFFER, cyls.data(), static_cast<GLsizeiptr>(cyls.size() * sizeof(CylinderInstance)));
      cyl_program_->use();
      cyl_program_->setMat4("u_view", view);
      cyl_program_->setMat4("u_proj", proj);
      cyl_vao_.bind();
      withGlFunctions([this, &cyls](auto& functions) {
        functions.glEnable(GL_CULL_FACE);  // closed CCW-outward mesh, same as cube/sphere
        functions.glDrawElementsInstanced(
            GL_TRIANGLES, cyl_index_count_, GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(cyls.size()));
        functions.glDisable(GL_CULL_FACE);
      });
      cyl_vao_.unbind();
      unuseProgram();
    }
  }

  // --- Arrows + axes (one unit ArrowGizmo; dims baked into the model matrix) ---
  if (!batch.arrows.empty() || !batch.axes.empty()) {
    const auto drawArrow = [&](const glm::mat4& world, const glm::vec4& color) {
      marker_arrow_.render(
          proj * view * world, glm::mat3(view * world), applyOverride(color), ArrowGizmo::Shading::kFlat);
    };

    for (const auto& arrow : batch.arrows) {
      if (arrow.frame_index >= frame_world.size()) {
        continue;
      }
      const auto& fw = frame_world[arrow.frame_index];
      if (!fw.has_value()) {
        continue;
      }
      const float total = std::max(arrow.shaft_length + arrow.head_length, 1e-4F);
      const float diam = std::max({arrow.head_diameter, arrow.shaft_diameter, 1e-4F});
      drawArrow(*fw * arrow.model * glm::scale(glm::mat4(1.0F), glm::vec3(total, diam, diam)), arrow.color);
    }

    if (!batch.axes.empty()) {
      const glm::mat4 to_y = glm::rotate(glm::mat4(1.0F), glm::radians(90.0F), glm::vec3(0, 0, 1));
      const glm::mat4 to_z = glm::rotate(glm::mat4(1.0F), glm::radians(-90.0F), glm::vec3(0, 1, 0));
      for (const auto& ax : batch.axes) {
        if (ax.frame_index >= frame_world.size()) {
          continue;
        }
        const auto& fw = frame_world[ax.frame_index];
        if (!fw.has_value()) {
          continue;
        }
        const glm::mat4 base = *fw * ax.model;
        const glm::mat4 s = glm::scale(
            glm::mat4(1.0F),
            glm::vec3(std::max(ax.length, 1e-3F), std::max(ax.thickness, 1e-3F), std::max(ax.thickness, 1e-3F)));
        drawArrow(base * s, {1, 0, 0, 1});         // X red
        drawArrow(base * to_y * s, {0, 1, 0, 1});  // Y green
        drawArrow(base * to_z * s, {0, 0, 1, 1});  // Z blue
      }
    }
  }

  // --- Lines (unlit; one streamed draw per batch) ---
  if (line_program_ != nullptr && !batch.lines.empty()) {
    line_program_->use();
    line_vao_.bind();
    for (const auto& lb : batch.lines) {
      if (lb.frame_index >= frame_world.size() || lb.vertices.empty()) {
        continue;
      }
      const auto& fw = frame_world[lb.frame_index];
      if (!fw.has_value()) {
        continue;
      }
      std::vector<float> buf;
      buf.reserve(lb.vertices.size() * 7);
      for (std::size_t i = 0; i < lb.vertices.size(); ++i) {
        const glm::vec4 col = applyOverride(lb.colors.empty() ? lb.color : lb.colors[i]);
        const glm::vec3& p = lb.vertices[i];
        buf.insert(buf.end(), {p.x, p.y, p.z, col.r, col.g, col.b, col.a});
      }
      line_vbo_.uploadStatic(GL_ARRAY_BUFFER, buf.data(), static_cast<GLsizeiptr>(buf.size() * sizeof(float)));
      line_program_->setMat4("u_mvp", proj * view * (*fw * lb.model));
      // Note: glLineWidth > 1 is GL_INVALID_VALUE in a core profile — thickness is
      // best-effort (1px). Real thick lines are quad-expansion (v2).
      const GLsizei count = static_cast<GLsizei>(lb.vertices.size());
      withGlFunctions([count](auto& functions) { functions.glDrawArrays(GL_LINES, 0, count); });
    }
    line_vao_.unbind();
    unuseProgram();
  }

  // --- Triangles (flat color; one streamed draw per batch) ---
  if (tri_program_ != nullptr && !batch.triangles.empty()) {
    tri_program_->use();
    tri_vao_.bind();
    for (const auto& tb : batch.triangles) {
      if (tb.frame_index >= frame_world.size() || tb.vertices.empty()) {
        continue;
      }
      const auto& fw = frame_world[tb.frame_index];
      if (!fw.has_value()) {
        continue;
      }
      std::vector<float> buf;
      buf.reserve(tb.vertices.size() * 10);
      for (std::size_t i = 0; i < tb.vertices.size(); ++i) {
        const glm::vec4 col = applyOverride(tb.colors.empty() ? tb.color : tb.colors[i]);
        const glm::vec3& p = tb.vertices[i];
        const glm::vec3& n = tb.normals[i];
        buf.insert(buf.end(), {p.x, p.y, p.z, n.x, n.y, n.z, col.r, col.g, col.b, col.a});
      }
      tri_vbo_.uploadStatic(GL_ARRAY_BUFFER, buf.data(), static_cast<GLsizeiptr>(buf.size() * sizeof(float)));
      const glm::mat4 world = *fw * tb.model;
      tri_program_->setMat4("u_mvp", proj * view * world);
      const GLsizei count = static_cast<GLsizei>(tb.vertices.size());
      withGlFunctions([count](auto& functions) { functions.glDrawArrays(GL_TRIANGLES, 0, count); });
    }
    tri_vao_.unbind();
    unuseProgram();
  }

  // Restore GL state for sibling passes (the rest of the engine assumes
  // culling off; the per-draw disables above already guarantee it, this is
  // the pass-exit invariant made explicit).
  setPolygonMode(GL_FILL);
  withGlFunctions([](auto& functions) {
    functions.glDepthMask(GL_TRUE);
    functions.glDisable(GL_CULL_FACE);
  });
}

void MarkerRenderPass::releaseGL() {
  solid_program_.reset();
  instance_vbo_ = gl::Buffer{};
  cube_vao_ = gl::VertexArray{};
  cube_vbo_ = gl::Buffer{};
  cube_ebo_ = gl::Buffer{};
  cube_index_count_ = 0;
  sphere_vao_ = gl::VertexArray{};
  sphere_vbo_ = gl::Buffer{};
  sphere_ebo_ = gl::Buffer{};
  sphere_index_count_ = 0;
  edge_program_.reset();
  edge_vao_ = gl::VertexArray{};
  edge_vbo_ = gl::Buffer{};
  cyl_program_.reset();
  cyl_vao_ = gl::VertexArray{};
  cyl_vbo_ = gl::Buffer{};
  cyl_ebo_ = gl::Buffer{};
  cyl_instance_vbo_ = gl::Buffer{};
  cyl_index_count_ = 0;
  marker_arrow_.releaseGL();
  line_program_.reset();
  line_vao_ = gl::VertexArray{};
  line_vbo_ = gl::Buffer{};
  tri_program_.reset();
  tri_vao_ = gl::VertexArray{};
  tri_vbo_ = gl::Buffer{};
  initialized_ = false;
}

}  // namespace pj::scene3d
