// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"

#include <fmt/core.h>

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLVersionFunctionsFactory>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "pj_scene3d_core/pointcloud.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d {
namespace {

constexpr std::string_view kPointcloudVertSrc = R"(#version 450 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in float in_scalar;
uniform mat4 u_view_model;
uniform mat4 u_proj;
uniform float u_range_min;
uniform float u_range_max;
uniform float u_world_radius;     // metres — used when u_use_perspective_size
uniform float u_pixel_size;       // px    — used otherwise
uniform float u_viewport_height;  // pixels
uniform float u_min_size_px;
uniform float u_max_size_px;
uniform bool u_use_perspective_size;
// Piecewise perceptual decay: depths <= u_depth_threshold use the
// physically-correct 1/depth scaling so near-field perspective cues
// remain intact; beyond the threshold, scaling falls off as
// 1/sqrt(depth * u_depth_threshold) so distant spheres stay
// perceptible instead of vanishing into a 1-pixel dot. The two
// branches meet continuously at depth = u_depth_threshold.
uniform float u_depth_threshold;  // metres
out float v_normalized;
void main() {
  vec4 view_pos = u_view_model * vec4(in_pos, 1.0);
  gl_Position = u_proj * view_pos;
  if (u_use_perspective_size) {
    // OpenGL view-space looks down -Z, so depth is -view_pos.z (positive forward).
    float depth = max(-view_pos.z, 0.01);
    float depth_eff = depth <= u_depth_threshold ? depth : sqrt(depth * u_depth_threshold);
    // Project the world-space radius to a pixel diameter:
    //   gl_PointSize == 2 * R * focal_pixels / depth,
    // with focal_pixels = viewport_height * proj[1][1] / 2.
    // Simplifies to: R * proj[1][1] * viewport_height / depth.
    gl_PointSize = clamp(u_world_radius * u_proj[1][1] * u_viewport_height / depth_eff, u_min_size_px, u_max_size_px);
  } else {
    gl_PointSize = clamp(u_pixel_size, u_min_size_px, u_max_size_px);
  }
  float span = max(u_range_max - u_range_min, 1e-9);
  v_normalized = clamp((in_scalar - u_range_min) / span, 0.0, 1.0);
}
)";

constexpr std::string_view kPointcloudFragSrc = R"(#version 450 core
in float v_normalized;
out vec4 frag_color;

uniform int  u_color_mode;     // 0 = field-from-LUT, 1 = solid
uniform vec3 u_solid_color;
uniform int  u_colormap_id;    // 0=turbo, 1=viridis, 2=plasma, 3=grayscale
uniform bool u_invert;
uniform bool u_shape_is_sphere;  // sphere imposter shading vs flat point

vec3 turbo(float t) {
  const vec4 kRedVec4   = vec4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
  const vec4 kGreenVec4 = vec4(0.09140261, 2.19418839,   4.84296658, -14.18503333);
  const vec4 kBlueVec4  = vec4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
  const vec2 kRedVec2   = vec2(-152.94239396,  59.28637943);
  const vec2 kGreenVec2 = vec2(  4.27729857,   2.82956604);
  const vec2 kBlueVec2  = vec2(-89.90310912,  27.34824973);

  t = clamp(t, 0.0, 1.0);
  vec4 v4 = vec4(1.0, t, t * t, t * t * t);
  vec2 v2 = v4.zw * v4.z;
  return vec3(
    dot(v4, kRedVec4)   + dot(v2, kRedVec2),
    dot(v4, kGreenVec4) + dot(v2, kGreenVec2),
    dot(v4, kBlueVec4)  + dot(v2, kBlueVec2)
  );
}

// Matt Zucker's polynomial approximation of matplotlib's viridis.
vec3 viridis(float t) {
  const vec3 c0 = vec3(0.2777273272234177, 0.005407344544966578, 0.3340998053353061);
  const vec3 c1 = vec3(0.1050930431085774, 1.404613529898575,    1.384590162594685);
  const vec3 c2 = vec3(-0.3308618287255563, 0.214847559468213,   0.09509516302823659);
  const vec3 c3 = vec3(-4.634230498983486, -5.799100973351585, -19.33244095627987);
  const vec3 c4 = vec3(6.228269936347081,  14.17993336680509,   56.69055260068105);
  const vec3 c5 = vec3(4.776384997670288, -13.74514537774601,  -65.35303263337234);
  const vec3 c6 = vec3(-5.435455855934631,  4.645852612178535,  26.3124352495832);
  return c0 + t * (c1 + t * (c2 + t * (c3 + t * (c4 + t * (c5 + t * c6)))));
}

// Matt Zucker's polynomial approximation of matplotlib's plasma.
vec3 plasma(float t) {
  const vec3 c0 = vec3(0.05873234392399702, 0.02333670892565664, 0.5433401826748754);
  const vec3 c1 = vec3(2.176514634195958,   0.2383834171260182,  0.7539604599784036);
  const vec3 c2 = vec3(-2.689460476458034, -7.455851135738909,   3.110799939717086);
  const vec3 c3 = vec3(6.130348345893603,  42.3461881477227,   -28.51885465332158);
  const vec3 c4 = vec3(-11.10743619062271, -82.66631109428045,  60.13984767418263);
  const vec3 c5 = vec3(10.02306557647065,  71.41361770095349, -54.07218655560067);
  const vec3 c6 = vec3(-3.658713842777788, -22.93153465461149, 18.19190778539828);
  return c0 + t * (c1 + t * (c2 + t * (c3 + t * (c4 + t * (c5 + t * c6)))));
}

vec3 sampleColormap(int id, float t) {
  t = clamp(t, 0.0, 1.0);
  if (id == 0) return turbo(t);
  if (id == 1) return viridis(t);
  if (id == 2) return plasma(t);
  return vec3(t);  // grayscale
}

void main() {
  // Sphere-imposter alpha discard. In kPoint mode (u_shape_is_sphere == false)
  // the whole sprite is opaque and unlit — what the user asked for.
  float shading = 1.0;
  if (u_shape_is_sphere) {
    // Treat the quad as the silhouette of a sphere and compute a per-fragment
    // normal so Lambert shading gives a 3D look. coord is the fragment
    // position within the sprite, remapped from gl_PointCoord ([0,1]) to
    // [-1,1] with the origin at the sprite center.
    vec2 coord = 2.0 * gl_PointCoord - 1.0;
    float r2 = dot(coord, coord);
    if (r2 > 1.0) {
      discard;
    }
    // Sphere normal in eye space. gl_PointCoord.y runs top-to-bottom by
    // default, so we negate the y component.
    vec3 normal = vec3(coord.x, -coord.y, sqrt(1.0 - r2));
    // Fixed head-light from upper-right-front. Cheap, no extra uniforms.
    vec3 light_dir = normalize(vec3(0.4, 0.5, 0.8));
    shading = 0.35 + 0.65 * max(dot(normal, light_dir), 0.0);
  }

  vec3 base;
  if (u_color_mode == 1) {
    base = u_solid_color;
  } else {
    float t = v_normalized;
    if (u_invert) {
      t = 1.0 - t;
    }
    base = sampleColormap(u_colormap_id, t);
  }
  frag_color = vec4(base * shading, 1.0);
}
)";

constexpr std::string_view kCubeVertSrc = R"(#version 450 core
layout(location = 0) in vec3 in_corner_pos;       // per-vertex (24)
layout(location = 1) in vec3 in_corner_normal;    // per-vertex (24)
layout(location = 2) in vec3 in_instance_pos;     // per-instance, divisor=1
layout(location = 3) in float in_instance_scalar; // per-instance, divisor=1

uniform mat4 u_model;          // source-frame -> fixed-frame
uniform mat4 u_view;           // fixed-frame -> view space
uniform mat4 u_proj;
uniform float u_size_meters;
uniform float u_range_min;
uniform float u_range_max;

out vec3 v_view_normal;
out float v_normalized;

void main() {
  // Cubes are axis-aligned in the fixed frame: transform the instance
  // position into fixed-frame coordinates, then add the corner offset
  // unchanged. No per-instance rotation matrix is needed.
  vec3 instance_in_fixed = (u_model * vec4(in_instance_pos, 1.0)).xyz;
  vec3 vertex_in_fixed = instance_in_fixed + in_corner_pos * u_size_meters;
  gl_Position = u_proj * u_view * vec4(vertex_in_fixed, 1.0);
  // Corner normals are in fixed-frame axes; bring them into view space for
  // the camera-relative key light in the fragment shader.
  v_view_normal = mat3(u_view) * in_corner_normal;
  float span = max(u_range_max - u_range_min, 1e-9);
  v_normalized = clamp((in_instance_scalar - u_range_min) / span, 0.0, 1.0);
}
)";

constexpr std::string_view kCubeFragSrc = R"(#version 450 core
in vec3 v_view_normal;
in float v_normalized;
out vec4 frag_color;

uniform int  u_color_mode;     // 0 = field-from-LUT, 1 = solid
uniform vec3 u_solid_color;
uniform int  u_colormap_id;    // 0=turbo, 1=viridis, 2=plasma, 3=grayscale
uniform bool u_invert;

vec3 turbo(float t) {
  const vec4 kRedVec4   = vec4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
  const vec4 kGreenVec4 = vec4(0.09140261, 2.19418839,   4.84296658, -14.18503333);
  const vec4 kBlueVec4  = vec4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
  const vec2 kRedVec2   = vec2(-152.94239396,  59.28637943);
  const vec2 kGreenVec2 = vec2(  4.27729857,   2.82956604);
  const vec2 kBlueVec2  = vec2(-89.90310912,  27.34824973);
  t = clamp(t, 0.0, 1.0);
  vec4 v4 = vec4(1.0, t, t * t, t * t * t);
  vec2 v2 = v4.zw * v4.z;
  return vec3(
    dot(v4, kRedVec4)   + dot(v2, kRedVec2),
    dot(v4, kGreenVec4) + dot(v2, kGreenVec2),
    dot(v4, kBlueVec4)  + dot(v2, kBlueVec2)
  );
}

vec3 viridis(float t) {
  const vec3 c0 = vec3(0.2777273272234177, 0.005407344544966578, 0.3340998053353061);
  const vec3 c1 = vec3(0.1050930431085774, 1.404613529898575,    1.384590162594685);
  const vec3 c2 = vec3(-0.3308618287255563, 0.214847559468213,   0.09509516302823659);
  const vec3 c3 = vec3(-4.634230498983486, -5.799100973351585, -19.33244095627987);
  const vec3 c4 = vec3(6.228269936347081,  14.17993336680509,   56.69055260068105);
  const vec3 c5 = vec3(4.776384997670288, -13.74514537774601,  -65.35303263337234);
  const vec3 c6 = vec3(-5.435455855934631,  4.645852612178535,  26.3124352495832);
  return c0 + t * (c1 + t * (c2 + t * (c3 + t * (c4 + t * (c5 + t * c6)))));
}

vec3 plasma(float t) {
  const vec3 c0 = vec3(0.05873234392399702, 0.02333670892565664, 0.5433401826748754);
  const vec3 c1 = vec3(2.176514634195958,   0.2383834171260182,  0.7539604599784036);
  const vec3 c2 = vec3(-2.689460476458034, -7.455851135738909,   3.110799939717086);
  const vec3 c3 = vec3(6.130348345893603,  42.3461881477227,   -28.51885465332158);
  const vec3 c4 = vec3(-11.10743619062271, -82.66631109428045,  60.13984767418263);
  const vec3 c5 = vec3(10.02306557647065,  71.41361770095349, -54.07218655560067);
  const vec3 c6 = vec3(-3.658713842777788, -22.93153465461149, 18.19190778539828);
  return c0 + t * (c1 + t * (c2 + t * (c3 + t * (c4 + t * (c5 + t * c6)))));
}

vec3 sampleColormap(int id, float t) {
  t = clamp(t, 0.0, 1.0);
  if (id == 0) return turbo(t);
  if (id == 1) return viridis(t);
  if (id == 2) return plasma(t);
  return vec3(t);
}

void main() {
  // View-space Lambertian. Light direction in view space — fixed
  // upper-right-front, same recipe as the sphere imposter for consistency.
  vec3 n = normalize(v_view_normal);
  vec3 light_dir = normalize(vec3(0.4, 0.5, 0.8));
  float shading = 0.35 + 0.65 * max(dot(n, light_dir), 0.0);

  vec3 base;
  if (u_color_mode == 1) {
    base = u_solid_color;
  } else {
    float t = v_normalized;
    if (u_invert) {
      t = 1.0 - t;
    }
    base = sampleColormap(u_colormap_id, t);
  }
  frag_color = vec4(base * shading, 1.0);
}
)";

struct CubeVertex {
  float px, py, pz;
  float nx, ny, nz;
};

// 24-vertex unit cube (±0.5 along each axis), 4 verts per face × 6 faces,
// each carrying its outward face normal. Winding is CCW when viewed from
// outside the face — matches OpenGL's default front-face convention.
constexpr std::array<CubeVertex, 24> kCubeVertices = {{
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

constexpr std::array<uint8_t, 36> kCubeIndices = {{
    0,  1,  2,  0,  2,  3,   // +X
    4,  5,  6,  4,  6,  7,   // -X
    8,  9,  10, 8,  10, 11,  // +Y
    12, 13, 14, 12, 14, 15,  // -Y
    16, 17, 18, 16, 18, 19,  // +Z
    20, 21, 22, 20, 22, 23,  // -Z
}};

struct CloudVertex {
  float x;
  float y;
  float z;
  float scalar;
};

}  // namespace

PointcloudRenderPass::PointcloudRenderPass() = default;

PointcloudRenderPass::~PointcloudRenderPass() = default;

void PointcloudRenderPass::releaseGL() {
  // Drop both programs and every buffer from the dying context. The CPU-side
  // cloud_ (a shared_ptr) is retained; initializeGL() re-sets cloud_dirty_ so
  // render() re-uploads the point VBO and re-wires the attribs in the new
  // context. cube_instance_bindings_dirty_ forces the cube path to rebind too.
  program_.reset();
  cube_program_.reset();
  vao_ = gl::VertexArray{};
  vbo_ = gl::Buffer{};
  cube_vao_ = gl::VertexArray{};
  cube_vbo_ = gl::Buffer{};
  cube_ebo_ = gl::Buffer{};
  vbo_point_count_ = 0U;
  cube_instance_bindings_dirty_ = true;
  cloud_dirty_ = (cloud_ != nullptr);
  initialized_ = false;
}

void PointcloudRenderPass::initializeGL() {
  // Idempotent: SceneViewWidget::paintGL calls the owning layer's
  // initializeGL() every frame (so a layer added after the widget is
  // realised initialises on its first paint). Without this guard we
  // recompiled both shader programs, re-uploaded the static cube mesh, and
  // re-flagged cloud_dirty_ (forcing a full cloud VBO re-upload) on EVERY
  // frame — an apitrace-confirmed perf bug (~3 compiles + ~3.4 buffer
  // uploads per frame, 6.9 GB trace). Mirror the guard AxisOverlayPass uses.
  if (initialized_) {
    return;
  }
  auto result = gl::Program::fromSources(kPointcloudVertSrc, kPointcloudFragSrc);
  if (auto* program = std::get_if<gl::Program>(&result); program != nullptr) {
    program_ = std::make_unique<gl::Program>(std::move(*program));
  } else {
    fmt::print(stderr, "PointcloudRenderPass shader error: {}\n", std::get<std::string>(result));
    program_.reset();
    return;
  }

  auto cube_result = gl::Program::fromSources(kCubeVertSrc, kCubeFragSrc);
  if (auto* program = std::get_if<gl::Program>(&cube_result); program != nullptr) {
    cube_program_ = std::make_unique<gl::Program>(std::move(*program));
  } else {
    fmt::print(stderr, "PointcloudRenderPass cube shader error: {}\n", std::get<std::string>(cube_result));
    cube_program_.reset();
    // Sphere/point still works; cube draws fall back to sphere via shape_
    // check in render().
  }

  // Upload the static cube mesh once. Per-instance attribs in cube_vao_
  // are wired up lazily on first cube draw (vbo_ must have a real ID by
  // then), so don't touch cube_vao_ here.
  cube_vbo_.uploadStatic(
      GL_ARRAY_BUFFER, kCubeVertices.data(), static_cast<GLsizeiptr>(kCubeVertices.size() * sizeof(CubeVertex)));
  cube_ebo_.uploadStatic(
      GL_ELEMENT_ARRAY_BUFFER, kCubeIndices.data(), static_cast<GLsizeiptr>(kCubeIndices.size() * sizeof(uint8_t)));
  cube_instance_bindings_dirty_ = true;

  withGlFunctions([](auto& functions) { functions.glEnable(GL_PROGRAM_POINT_SIZE); });

  initialized_ = true;
  cloud_dirty_ = true;
}

void PointcloudRenderPass::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!visible_) {
    return;
  }
  if (!initialized_ || program_ == nullptr) {
    return;
  }

  if (cloud_dirty_) {
    if (!cloud_ || cloud_->positions.empty()) {
      vbo_point_count_ = 0U;
      cloud_dirty_ = false;
      return;
    }

    std::vector<CloudVertex> vertices;
    vertices.reserve(cloud_->positions.size());
    const bool has_scalars = cloud_->scalar.size() == cloud_->positions.size();
    for (std::size_t i = 0U; i < cloud_->positions.size(); ++i) {
      const glm::vec3& p = cloud_->positions[i];
      const float scalar = has_scalars ? cloud_->scalar[i] : 0.0f;
      vertices.push_back(CloudVertex{p.x, p.y, p.z, scalar});
    }

    vbo_.uploadStatic(GL_ARRAY_BUFFER, vertices.data(), static_cast<GLsizeiptr>(vertices.size() * sizeof(CloudVertex)));
    vao_.bind();
    withGlFunctions([](auto& functions) {
      functions.glEnableVertexAttribArray(0U);
      functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(CloudVertex)), nullptr);
      functions.glEnableVertexAttribArray(1U);
      functions.glVertexAttribPointer(
          1U, 1, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(CloudVertex)), reinterpret_cast<const void*>(12));
    });
    vao_.unbind();

    vbo_point_count_ = cloud_->positions.size();
    cloud_dirty_ = false;
  }

  if (vbo_point_count_ == 0U) {
    return;
  }

  const auto transform = frame_ctx.lookup(cloud_->frame_id);
  if (!transform.has_value()) {
    return;
  }

  const glm::mat4 model = glm::mat4(transform->matrix());

  if (shape_ == Shape::kCube && cube_program_ != nullptr) {
    // Lazy one-time wiring of the cube VAO's per-instance attribs to vbo_.
    // The buffer ID is stable across cloud swaps, so once set this stays
    // valid for the lifetime of the pass.
    if (cube_instance_bindings_dirty_) {
      cube_vao_.bind();
      cube_vbo_.bind(GL_ARRAY_BUFFER);
      withGlFunctions([](auto& functions) {
        functions.glEnableVertexAttribArray(0U);
        functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(CubeVertex)), nullptr);
        functions.glEnableVertexAttribArray(1U);
        functions.glVertexAttribPointer(
            1U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(CubeVertex)), reinterpret_cast<const void*>(12));
      });
      vbo_.bind(GL_ARRAY_BUFFER);
      withGlFunctions([](auto& functions) {
        functions.glEnableVertexAttribArray(2U);
        functions.glVertexAttribPointer(2U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(CloudVertex)), nullptr);
        functions.glVertexAttribDivisor(2U, 1U);
        functions.glEnableVertexAttribArray(3U);
        functions.glVertexAttribPointer(
            3U, 1, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(CloudVertex)), reinterpret_cast<const void*>(12));
        functions.glVertexAttribDivisor(3U, 1U);
      });
      cube_ebo_.bind(GL_ELEMENT_ARRAY_BUFFER);
      cube_vao_.unbind();
      cube_instance_bindings_dirty_ = false;
    }

    cube_program_->use();
    cube_program_->setMat4("u_model", model);
    cube_program_->setMat4("u_view", view_params.view);
    cube_program_->setMat4("u_proj", view_params.proj);
    cube_program_->setFloat("u_size_meters", size_meters_);
    cube_program_->setFloat("u_range_min", range_min_);
    cube_program_->setFloat("u_range_max", range_max_);
    cube_program_->setInt("u_color_mode", color_type_ == ColorType::kSolid ? 1 : 0);
    cube_program_->setVec3("u_solid_color", solid_color_);
    cube_program_->setInt("u_colormap_id", static_cast<int>(colormap_));
    cube_program_->setInt("u_invert", invert_lut_ ? 1 : 0);
    cube_vao_.bind();
    withGlFunctions([this](auto& functions) {
      functions.glDrawElementsInstanced(
          GL_TRIANGLES, static_cast<GLsizei>(kCubeIndices.size()), GL_UNSIGNED_BYTE, nullptr,
          static_cast<GLsizei>(vbo_point_count_));
    });
    cube_vao_.unbind();
    unuseProgram();
    return;
  }

  // Sphere / point path (the cube setter falls back here while cube_program_
  // is unavailable, e.g. shader compile failure).
  const glm::mat4 view_model = view_params.view * model;
  const bool shape_is_sphere = shape_ != Shape::kPoint;
  const bool use_perspective_size = shape_ != Shape::kPoint;
  program_->use();
  program_->setMat4("u_view_model", view_model);
  program_->setMat4("u_proj", view_params.proj);
  program_->setFloat("u_range_min", range_min_);
  program_->setFloat("u_range_max", range_max_);
  // size_meters_ is user-facing as the sphere DIAMETER; the shader formula
  // is parameterised on radius (gl_PointSize ≈ 2*R*focal/depth). Halve here
  // so a "0.01 m" input renders as a 1 cm sphere, not a 2 cm one.
  program_->setFloat("u_world_radius", size_meters_ * 0.5f);
  program_->setFloat("u_pixel_size", static_cast<float>(size_pixels_));
  program_->setFloat("u_viewport_height", static_cast<float>(std::max(view_params.viewport_height_px, 1)));
  program_->setFloat("u_min_size_px", 0.5f);
  program_->setFloat("u_max_size_px", 32.0f);
  program_->setFloat("u_depth_threshold", 5.0f);
  program_->setInt("u_use_perspective_size", use_perspective_size ? 1 : 0);
  program_->setInt("u_color_mode", color_type_ == ColorType::kSolid ? 1 : 0);
  program_->setVec3("u_solid_color", solid_color_);
  program_->setInt("u_colormap_id", static_cast<int>(colormap_));
  program_->setInt("u_invert", invert_lut_ ? 1 : 0);
  program_->setInt("u_shape_is_sphere", shape_is_sphere ? 1 : 0);
  vao_.bind();
  withGlFunctions(
      [this](auto& functions) { functions.glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(vbo_point_count_)); });
  vao_.unbind();
  unuseProgram();
}

void PointcloudRenderPass::setActiveCloud(std::shared_ptr<const DecodedPointCloud> cloud) {
  cloud_ = std::move(cloud);
  cloud_dirty_ = true;
}

void PointcloudRenderPass::setColormapRange(float min_value, float max_value) {
  range_min_ = min_value;
  range_max_ = max_value;
}

void PointcloudRenderPass::setSizeMeters(float meters) {
  size_meters_ = std::max(0.0f, meters);
}

void PointcloudRenderPass::setSizePixels(int pixels) {
  size_pixels_ = std::max(1, pixels);
}

void PointcloudRenderPass::setShape(Shape shape) {
  shape_ = shape;
}

void PointcloudRenderPass::setColorType(ColorType type) {
  color_type_ = type;
}

void PointcloudRenderPass::setSolidColor(glm::vec3 rgb) {
  solid_color_ = glm::clamp(rgb, glm::vec3(0.0f), glm::vec3(1.0f));
}

void PointcloudRenderPass::setColormap(Colormap cm) {
  colormap_ = cm;
}

void PointcloudRenderPass::setInvertLut(bool invert) {
  invert_lut_ = invert;
}

}  // namespace pj::scene3d
