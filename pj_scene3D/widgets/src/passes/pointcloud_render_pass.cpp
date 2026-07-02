// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"

#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "pj_scene3d_core/pointcloud.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/cube_mesh.h"  // shared CubeVertex / kCubeVertices / kCubeIndices / kCubeEdgeGlsl
#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d {

static_assert(
    kGlFloat == GL_FLOAT && kGlUnsignedInt == GL_UNSIGNED_INT && kGlShort == GL_SHORT &&
        kGlUnsignedShort == GL_UNSIGNED_SHORT && kGlByte == GL_BYTE && kGlUnsignedByte == GL_UNSIGNED_BYTE &&
        kGlInt == GL_INT,
    "core GL enum mirror drift");

namespace {

constexpr std::string_view kPointcloudVertSrc = R"(#version 410 core
layout(location = 0) in vec3 in_pos;
// Integer attribs bound with normalized=GL_FALSE are widened to float by GL,
// matching readScalarAt (signed types sign-extend).
layout(location = 1) in float in_scalar;
layout(location = 2) in vec4 in_color;  // per-point RGBA in [0,1] (kRgb mode)
out vec4 v_color;
uniform mat4 u_view_model;
uniform mat4 u_proj;
uniform mat4 u_model;          // source-frame -> fixed-frame (RENDER space); only for fixed-frame axis colour
uniform int u_scalar_axis;     // -1 = colour by in_scalar; 0/1/2 = fixed-frame x/y/z
// Camera-relative render origin: u_model places points in render space (origin
// subtracted for float precision), so add it back along the coloured axis to recover
// the ABSOLUTE world coordinate the colormap range is expressed in.
uniform vec3 u_color_axis_offset;
uniform float u_range_min;
uniform float u_range_max;
uniform float u_world_radius;     // metres — used when u_use_perspective_size
uniform float u_pixel_size;       // px    — used otherwise
uniform float u_viewport_height;  // pixels
uniform float u_min_size_px;
uniform float u_max_size_px;
uniform bool u_use_perspective_size;
// Piecewise perceptual decay applied ONLY under a perspective projection: depths
// <= u_depth_threshold use the physically-correct 1/depth scaling so near-field
// perspective cues remain intact; beyond the threshold, scaling falls off as
// 1/sqrt(depth * u_depth_threshold) so distant spheres stay perceptible instead of
// vanishing into a 1-pixel dot. The two branches meet continuously at
// depth = u_depth_threshold. An orthographic projection has no perspective divide.
uniform float u_depth_threshold;  // metres
out float v_normalized;
void main() {
  vec4 view_pos = u_view_model * vec4(in_pos, 1.0);
  gl_Position = u_proj * view_pos;
  if (u_use_perspective_size) {
    // Project the world-space radius to a pixel diameter:
    //   gl_PointSize == 2 * R * focal_pixels / d,
    // with focal_pixels = viewport_height * proj[1][1] / 2,
    // which simplifies to R * proj[1][1] * viewport_height / d.
    float size_px = u_world_radius * u_proj[1][1] * u_viewport_height;
    // The 1/d perspective foreshortening is correct ONLY for a perspective camera;
    // an orthographic camera has a constant world->pixel scale (d == 1). proj[3][3]
    // is 0 for perspective and 1 for orthographic — a cheap, uniform-free
    // discriminator (the same matrix-derived idiom the SSAO pass uses).
    if (u_proj[3][3] == 0.0) {
      // OpenGL view-space looks down -Z, so depth is -view_pos.z (positive forward).
      float depth = max(-view_pos.z, 0.01);
      float depth_eff = depth <= u_depth_threshold ? depth : sqrt(depth * u_depth_threshold);
      size_px /= depth_eff;
    }
    gl_PointSize = clamp(size_px, u_min_size_px, u_max_size_px);
  } else {
    gl_PointSize = clamp(u_pixel_size, u_min_size_px, u_max_size_px);
  }
  v_color = in_color;
  // Fixed-frame axis colouring: derive the colormap input from the GPU-transformed
  // position (u_model * in_pos) so sensors at different mounts agree on world height.
  // u_model is render-relative, so add the origin back along the axis to colour by the
  // absolute world coordinate (stable as the camera moves; matches the colormap range).
  float scalar = u_scalar_axis < 0 ? in_scalar
                                   : (u_model * vec4(in_pos, 1.0))[u_scalar_axis] + u_color_axis_offset[u_scalar_axis];
  float span = max(u_range_max - u_range_min, 1e-9);
  v_normalized = clamp((scalar - u_range_min) / span, 0.0, 1.0);
}
)";

// Shared colormap GLSL (turbo / viridis / plasma + sampleColormap dispatcher)
// comes from pj_widgets/Colormap.h::colormapGlsl(), so the hand-tuned polynomials
// live in ONE place shared with the 2D depth LUT (PJ::buildColormapLut). It is
// concatenated into BOTH the point/sphere and cube fragment sources at the
// makeXxxFragSrc() call sites below.

// The point/sphere fragment shader split around the colormap GLSL: head declares the
// inputs/uniforms, the LUTs slot in, then this tail's main() consumes them.
constexpr std::string_view kPointcloudFragHead = R"(#version 410 core
in float v_normalized;
in vec4 v_color;               // per-point RGBA (kRgb mode)
out vec4 frag_color;

uniform int  u_color_mode;     // 0 = field-from-LUT, 1 = solid, 2 = per-point rgb
uniform vec3 u_solid_color;
uniform int  u_colormap_id;    // 0=turbo, 1=viridis, 2=plasma, 3=grayscale
uniform bool u_invert;
uniform bool u_shape_is_sphere;  // sphere imposter shading vs flat point
)";

constexpr std::string_view kPointcloudFragTail = R"(
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
  } else if (u_color_mode == 2) {
    base = v_color.rgb;  // per-point colour used directly (no colormap)
  } else {
    float t = v_normalized;
    if (u_invert) {
      t = 1.0 - t;
    }
    base = sampleColormap(u_colormap_id, t);
  }
  // Colormaps/solid/per-point colors are display-referred sRGB; the scene FBO is
  // linear (Phase 0B: the composite present re-encodes to sRGB). Linearize on write.
  base = pow(max(base, vec3(0.0)), vec3(2.2));
  frag_color = vec4(base * shading, 1.0);
}
)";

constexpr std::string_view kCubeVertSrc = R"(#version 410 core
layout(location = 0) in vec3 in_corner_pos;       // per-vertex (24)
layout(location = 1) in vec3 in_corner_normal;    // per-vertex (24)
layout(location = 2) in vec3 in_instance_pos;     // per-instance, divisor=1
// Integer attribs bound with normalized=GL_FALSE are widened to float by GL,
// matching readScalarAt (signed types sign-extend).
layout(location = 3) in float in_instance_scalar; // per-instance, divisor=1
layout(location = 4) in vec4 in_instance_color;   // per-instance RGBA, divisor=1 (kRgb mode)

uniform mat4 u_model;          // source-frame -> fixed-frame (RENDER space)
uniform mat4 u_view;           // fixed-frame (RENDER space) -> view space
uniform mat4 u_proj;
uniform float u_size_meters;
uniform float u_range_min;
uniform float u_range_max;
uniform int u_scalar_axis;     // -1 = colour by in_instance_scalar; 0/1/2 = fixed-frame x/y/z
// Render origin added back along the coloured axis to recover the ABSOLUTE world
// coordinate (u_model/u_view are render-relative for float precision); see point shader.
uniform vec3 u_color_axis_offset;

out vec3 v_view_normal;
out float v_normalized;
out vec3 v_local;  // unit-cube corner, for the fragment-shader edge outline
out vec4 v_color;

void main() {
  v_local = in_corner_pos;
  // Cubes are axis-aligned in the fixed frame: transform the instance
  // position into fixed-frame coordinates, then add the corner offset
  // unchanged. No per-instance rotation matrix is needed.
  vec3 instance_in_fixed = (u_model * vec4(in_instance_pos, 1.0)).xyz;
  vec3 vertex_in_fixed = instance_in_fixed + in_corner_pos * u_size_meters;
  gl_Position = u_proj * u_view * vec4(vertex_in_fixed, 1.0);
  // Corner normals are in fixed-frame axes; bring them into view space for
  // the camera-relative key light in the fragment shader.
  v_view_normal = mat3(u_view) * in_corner_normal;
  // Fixed-frame axis colouring reuses instance_in_fixed (already computed above).
  // instance_in_fixed is render-relative, so add the origin back to colour by the
  // absolute world coordinate (stable as the camera moves; matches the range).
  float scalar = u_scalar_axis < 0 ? in_instance_scalar
                                   : instance_in_fixed[u_scalar_axis] + u_color_axis_offset[u_scalar_axis];
  float span = max(u_range_max - u_range_min, 1e-9);
  v_normalized = clamp((scalar - u_range_min) / span, 0.0, 1.0);
  v_color = in_instance_color;
}
)";

// The cube fragment shader split around the colormap GLSL (same shared source as
// the point shader). Head declares inputs/uniforms; tail's main() shades.
constexpr std::string_view kCubeFragHead = R"(#version 410 core
in vec3 v_view_normal;
in float v_normalized;
in vec3 v_local;
in vec4 v_color;               // per-instance RGBA (kRgb mode)
out vec4 frag_color;

uniform int  u_color_mode;     // 0 = field-from-LUT, 1 = solid, 2 = per-point rgb
uniform vec3 u_solid_color;
uniform int  u_colormap_id;    // 0=turbo, 1=viridis, 2=plasma, 3=grayscale
uniform bool u_invert;
)";

constexpr std::string_view kCubeFragTail = R"(
void main() {
  // View-space Lambertian. Light direction in view space — fixed
  // upper-right-front, same recipe as the sphere imposter for consistency.
  vec3 n = normalize(v_view_normal);
  vec3 light_dir = normalize(vec3(0.4, 0.5, 0.8));
  float shading = 0.35 + 0.65 * max(dot(n, light_dir), 0.0);

  vec3 base;
  if (u_color_mode == 1) {
    base = u_solid_color;
  } else if (u_color_mode == 2) {
    base = v_color.rgb;  // per-point colour used directly (no colormap)
  } else {
    float t = v_normalized;
    if (u_invert) {
      t = 1.0 - t;
    }
    base = sampleColormap(u_colormap_id, t);
  }
  // Darker outline on each cube's faces (same hue), so adjacent cubes stay legible.
  base = mix(base, base * 0.4, cubeEdgeFactor(v_local));
  // Colormaps/solid/per-point colors are display-referred sRGB; the scene FBO is
  // linear (Phase 0B: the composite present re-encodes to sRGB). Linearize on write.
  base = pow(max(base, vec3(0.0)), vec3(2.2));
  frag_color = vec4(base * shading, 1.0);
}
)";

// Compose each fragment source once (head + shared LUTs + tail). string_view
// concatenation needs std::string; fromSources takes string_view so the static
// strings convert implicitly. function-local statics build them on first use.
std::string makePointcloudFragSrc() {
  return std::string(kPointcloudFragHead) + std::string(PJ::colormapGlsl()) + std::string(kPointcloudFragTail);
}

std::string makeCubeFragSrc() {
  return std::string(kCubeFragHead) + std::string(PJ::colormapGlsl()) + std::string(kCubeEdgeGlsl) +
         std::string(kCubeFragTail);
}

struct CloudVertex {
  float x;
  float y;
  float z;
  float scalar;
  uint32_t rgba;  // packed per-point color (byte0=R..byte3=A); uploaded as 4 normalized bytes
};

// Shader u_color_mode selector: 0 = colormap-from-scalar, 1 = solid, 2 = per-point rgb.
int colorModeUniform(PointcloudRenderPass::ColorType type) {
  switch (type) {
    case PointcloudRenderPass::ColorType::kSolid:
      return 1;
    case PointcloudRenderPass::ColorType::kRgb:
      return 2;
    case PointcloudRenderPass::ColorType::kField:
      break;
  }
  return 0;
}

}  // namespace

PointcloudRenderPass::PointcloudRenderPass() = default;

PointcloudRenderPass::~PointcloudRenderPass() = default;

const std::string& PointcloudRenderPass::activeFrameId() const {
  if (const auto* fast = std::get_if<FastCloudData>(&cloud_); fast != nullptr) {
    return fast->wire.frame_id;
  }
  if (const auto* fallback = std::get_if<std::shared_ptr<const DecodedPointCloud>>(&cloud_);
      fallback != nullptr && *fallback) {
    return (*fallback)->frame_id;
  }
  static const std::string kEmpty;
  return kEmpty;
}

bool PointcloudRenderPass::hasRetainedCloud() const {
  if (const auto* fast = std::get_if<FastCloudData>(&cloud_); fast != nullptr) {
    return fast->point_count > 0U;
  }
  if (const auto* fallback = std::get_if<std::shared_ptr<const DecodedPointCloud>>(&cloud_);
      fallback != nullptr && *fallback) {
    return !(*fallback)->positions.empty();
  }
  return false;
}

void PointcloudRenderPass::releaseGL() {
  // Drop both programs and every buffer from the dying context. The CPU-side
  // retained cloud variant (fast wire anchor or fallback shared_ptr) survives;
  // initializeGL() re-sets cloud_dirty_ so render() re-uploads the point VBO
  // and re-wires the attribs in the new context. cube_instance_bindings_dirty_
  // forces the cube path to rebind too.
  program_.reset();
  cube_program_.reset();
  vao_ = gl::VertexArray{};
  vbo_ = gl::Buffer{};
  cube_vao_ = gl::VertexArray{};
  cube_vbo_ = gl::Buffer{};
  cube_ebo_ = gl::Buffer{};
  aabb_reducer_.releaseGL();  // re-probes lazily on the next dispatch in the new context
  vbo_point_count_ = 0U;
  cube_instance_bindings_dirty_ = true;
  cloud_dirty_ = hasRetainedCloud();
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
  static const std::string k_pointcloud_frag_src = makePointcloudFragSrc();
  auto result = gl::Program::fromSources(kPointcloudVertSrc, k_pointcloud_frag_src);
  if (auto* program = std::get_if<gl::Program>(&result); program != nullptr) {
    program_ = std::make_unique<gl::Program>(std::move(*program));
  } else {
    fmt::print(stderr, "PointcloudRenderPass shader error: {}\n", std::get<std::string>(result));
    program_.reset();
    return;
  }

  static const std::string k_cube_frag_src = makeCubeFragSrc();
  auto cube_result = gl::Program::fromSources(kCubeVertSrc, k_cube_frag_src);
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

  // Drain a completed GPU AABB from a PRIOR frame's dispatch before this frame's
  // draw, so spatial_auto_bounds_ (set by the callback in spatial-axis modes) and
  // the camera see the fresh extent this frame. Non-blocking — nullopt until the
  // fence signals. Always polled (even if disabled now) so a late result drains.
  if (auto box = aabb_reducer_.poll(); box.has_value() && bounds_callback_) {
    bounds_callback_(*box);
  }

  if (cloud_dirty_) {
    if (std::holds_alternative<std::monostate>(cloud_)) {
      vbo_point_count_ = 0U;
      cloud_dirty_ = false;
      return;
    }
    if (const auto* fallback = std::get_if<std::shared_ptr<const DecodedPointCloud>>(&cloud_);
        fallback != nullptr && (!*fallback || (*fallback)->positions.empty())) {
      vbo_point_count_ = 0U;
      cloud_dirty_ = false;
      return;
    }

    if (const auto* fast = std::get_if<FastCloudData>(&cloud_); fast != nullptr) {
      const GLsizeiptr bytes = static_cast<GLsizeiptr>(fast->point_count) * fast->layout.stride;
      vbo_.uploadStatic(GL_ARRAY_BUFFER, fast->wire.data.data(), bytes);
      vao_.bind();
      withGlFunctions([&](auto& functions) {
        functions.glEnableVertexAttribArray(0U);
        functions.glVertexAttribPointer(
            0U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(fast->layout.stride),
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(fast->layout.xyz_offset)));
        if (fast->layout.has_scalar) {
          functions.glEnableVertexAttribArray(1U);
          functions.glVertexAttribPointer(
              1U, 1, static_cast<GLenum>(fast->layout.scalar_gl_type), GL_FALSE,
              static_cast<GLsizei>(fast->layout.stride),
              reinterpret_cast<const void*>(static_cast<std::uintptr_t>(fast->layout.scalar_offset)));
        } else {
          functions.glDisableVertexAttribArray(1U);
          functions.glVertexAttrib1f(1U, 0.0f);
        }
        if (fast->layout.has_color) {
          // Packed RGBA straight from the wire buffer: 4 normalized bytes -> vec4 in [0,1]
          // (the shader paints .rgb), pixel-identical to the CPU rgba extraction.
          functions.glEnableVertexAttribArray(2U);
          functions.glVertexAttribPointer(
              2U, 4, GL_UNSIGNED_BYTE, GL_TRUE, static_cast<GLsizei>(fast->layout.stride),
              reinterpret_cast<const void*>(static_cast<std::uintptr_t>(fast->layout.color_offset)));
        } else {
          functions.glDisableVertexAttribArray(2U);
        }
      });
      vao_.unbind();

      vbo_point_count_ = fast->point_count;

      // Reduce the just-uploaded geometry on the GPU (async). The result lands in
      // a later frame's poll() above. Eligibility (4-byte-aligned float32 xyz) is
      // guaranteed by the layer before it enables this.
      if (gpu_aabb_enabled_ && fast->point_count > 0U) {
        aabb_reducer_.dispatch(vbo_.id(), fast->point_count, fast->layout.stride, fast->layout.xyz_offset);
      }
    } else if (
        const auto* fallback = std::get_if<std::shared_ptr<const DecodedPointCloud>>(&cloud_);
        fallback != nullptr && *fallback) {
      const auto& decoded = **fallback;
      std::vector<CloudVertex> vertices;
      vertices.reserve(decoded.positions.size());
      const bool has_scalars = decoded.scalar.size() == decoded.positions.size();
      const bool has_colors = decoded.rgba.size() == decoded.positions.size();
      for (std::size_t i = 0U; i < decoded.positions.size(); ++i) {
        const glm::vec3& p = decoded.positions[i];
        const float scalar = has_scalars ? decoded.scalar[i] : 0.0f;
        const uint32_t color = has_colors ? decoded.rgba[i] : 0xFFFFFFFFu;  // opaque white when no color
        vertices.push_back(CloudVertex{p.x, p.y, p.z, scalar, color});
      }

      vbo_.uploadStatic(
          GL_ARRAY_BUFFER, vertices.data(), static_cast<GLsizeiptr>(vertices.size() * sizeof(CloudVertex)));
      vao_.bind();
      withGlFunctions([](auto& functions) {
        functions.glEnableVertexAttribArray(0U);
        functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(CloudVertex)), nullptr);
        functions.glEnableVertexAttribArray(1U);
        functions.glVertexAttribPointer(
            1U, 1, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(CloudVertex)), reinterpret_cast<const void*>(12));
        // Packed RGBA at byte offset 16, fed as 4 normalized unsigned bytes -> vec4 in [0,1],
        // with .r = byte0 = Red (matches the canonical 'rgba' little-endian layout).
        functions.glEnableVertexAttribArray(2U);
        functions.glVertexAttribPointer(
            2U, 4, GL_UNSIGNED_BYTE, GL_TRUE, static_cast<GLsizei>(sizeof(CloudVertex)),
            reinterpret_cast<const void*>(16));
      });
      vao_.unbind();

      vbo_point_count_ = decoded.positions.size();
    }
    cloud_dirty_ = false;
  }

  if (vbo_point_count_ == 0U) {
    return;
  }

  const auto transform = frame_ctx.lookup(activeFrameId());
  if (!transform.has_value()) {
    return;
  }

  // Point/cube clouds are opaque data. Draw them without blending so covered
  // samples reset the scene FBO alpha marker to "grade me" instead of inheriting
  // alpha=0 from TF/HUD annotations rendered earlier in the frame.
  withGlFunctions([](auto& functions) { functions.glDisable(GL_BLEND); });

  // Camera-relative model (frame_ctx.lookup already subtracted render_origin in
  // double): geometry is placed in render space, so the eye→point delta survives
  // float32 even when following a frame at large world coordinates. The colour axis
  // is lifted back to absolute world below; positions stay relative.
  const glm::mat4 model = glm::mat4(transform->matrix());
  const glm::vec3 color_axis_offset(frame_ctx.render_origin);

  // Fixed-frame axis auto-range: derive the colormap [min,max] from the source
  // bounds transformed by THIS frame's model, so colour (computed per-point in the
  // shader from the same model) and range stay consistent as the TF moves. The model
  // is render-relative but the shader colours by the ABSOLUTE coordinate (it adds the
  // axis offset back), so lift the range by the same offset. Falls back to the
  // explicit range (manual, or a non-spatial field's scalar range).
  float effective_range_min = range_min_;
  float effective_range_max = range_max_;
  if (scalar_axis_ >= 0 && spatial_auto_bounds_.has_value()) {
    const auto [axis_min, axis_max] = transformedAabbAxisRange(*spatial_auto_bounds_, model, scalar_axis_);
    const float axis_offset = color_axis_offset[scalar_axis_];
    effective_range_min = axis_min + axis_offset;
    effective_range_max = axis_max + axis_offset;
  }

  if (shape_ == Shape::kCube && cube_program_ != nullptr) {
    // Lazy re-wiring of the cube VAO's per-instance attribs to vbo_. The
    // buffer ID is stable across cloud swaps, but stride/offset/type can vary.
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
      if (const auto* fast = std::get_if<FastCloudData>(&cloud_); fast != nullptr) {
        withGlFunctions([&](auto& functions) {
          functions.glEnableVertexAttribArray(2U);
          functions.glVertexAttribPointer(
              2U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(fast->layout.stride),
              reinterpret_cast<const void*>(static_cast<std::uintptr_t>(fast->layout.xyz_offset)));
          functions.glVertexAttribDivisor(2U, 1U);
          if (fast->layout.has_scalar) {
            functions.glEnableVertexAttribArray(3U);
            functions.glVertexAttribPointer(
                3U, 1, static_cast<GLenum>(fast->layout.scalar_gl_type), GL_FALSE,
                static_cast<GLsizei>(fast->layout.stride),
                reinterpret_cast<const void*>(static_cast<std::uintptr_t>(fast->layout.scalar_offset)));
            functions.glVertexAttribDivisor(3U, 1U);
          } else {
            functions.glDisableVertexAttribArray(3U);
            functions.glVertexAttrib1f(3U, 0.0f);
          }
          if (fast->layout.has_color) {
            functions.glEnableVertexAttribArray(4U);
            functions.glVertexAttribPointer(
                4U, 4, GL_UNSIGNED_BYTE, GL_TRUE, static_cast<GLsizei>(fast->layout.stride),
                reinterpret_cast<const void*>(static_cast<std::uintptr_t>(fast->layout.color_offset)));
            functions.glVertexAttribDivisor(4U, 1U);
          } else {
            functions.glDisableVertexAttribArray(4U);
          }
        });
      } else {
        withGlFunctions([](auto& functions) {
          functions.glEnableVertexAttribArray(2U);
          functions.glVertexAttribPointer(
              2U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(CloudVertex)), nullptr);
          functions.glVertexAttribDivisor(2U, 1U);
          functions.glEnableVertexAttribArray(3U);
          functions.glVertexAttribPointer(
              3U, 1, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(CloudVertex)), reinterpret_cast<const void*>(12));
          functions.glVertexAttribDivisor(3U, 1U);
          // Per-instance packed RGBA (4 normalized bytes -> vec4), same layout as the point path.
          functions.glEnableVertexAttribArray(4U);
          functions.glVertexAttribPointer(
              4U, 4, GL_UNSIGNED_BYTE, GL_TRUE, static_cast<GLsizei>(sizeof(CloudVertex)),
              reinterpret_cast<const void*>(16));
          functions.glVertexAttribDivisor(4U, 1U);
        });
      }
      cube_ebo_.bind(GL_ELEMENT_ARRAY_BUFFER);
      cube_vao_.unbind();
      cube_instance_bindings_dirty_ = false;
    }

    cube_program_->use();
    cube_program_->setMat4("u_model", model);
    cube_program_->setMat4("u_view", view_params.view);
    cube_program_->setMat4("u_proj", view_params.proj);
    cube_program_->setFloat("u_size_meters", size_meters_);
    cube_program_->setFloat("u_range_min", effective_range_min);
    cube_program_->setFloat("u_range_max", effective_range_max);
    cube_program_->setInt("u_scalar_axis", scalar_axis_);
    cube_program_->setVec3("u_color_axis_offset", color_axis_offset);
    cube_program_->setInt("u_color_mode", colorModeUniform(color_type_));
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
    withGlFunctions([](auto& functions) { functions.glEnable(GL_BLEND); });
    return;
  }

  // Sphere / point path (the cube setter falls back here while cube_program_
  // is unavailable, e.g. shader compile failure).
  const glm::mat4 view_model = view_params.view * model;
  const bool shape_is_sphere = shape_ != Shape::kPoint;
  const bool use_perspective_size = shape_ != Shape::kPoint;
  program_->use();
  program_->setMat4("u_view_model", view_model);
  program_->setMat4("u_model", model);                          // for fixed-frame axis colour (u_scalar_axis >= 0)
  program_->setVec3("u_color_axis_offset", color_axis_offset);  // lifts axis colour back to absolute world
  program_->setMat4("u_proj", view_params.proj);
  program_->setFloat("u_range_min", effective_range_min);
  program_->setFloat("u_range_max", effective_range_max);
  // size_meters_ is user-facing as the sphere DIAMETER; the shader formula
  // is parameterised on radius (gl_PointSize ≈ 2*R*focal/depth). Halve here
  // so a "0.01 m" input renders as a 1 cm sphere, not a 2 cm one.
  program_->setFloat("u_world_radius", size_meters_ * 0.5f);
  program_->setFloat("u_pixel_size", size_pixels_);
  // gl_PointSize rasterizes in DEVICE (framebuffer) pixels, so the world-radius
  // formula needs the device viewport height — on HiDPI the logical height would
  // shrink every perspective-sized point by 1/DPR (M.32). Fall back to the
  // logical height when ViewParams carries no device size (hand-built callers).
  const int device_height =
      view_params.device_height_px > 0 ? view_params.device_height_px : view_params.viewport_height_px;
  program_->setFloat("u_viewport_height", static_cast<float>(std::max(device_height, 1)));
  // The min/max clamps are physical pixel sizes; scale them by DPR so 0.5/32 keep
  // the same on-screen meaning at any DPR. Derive DPR from device/logical height.
  const float dpr = std::max(
      static_cast<float>(device_height) / static_cast<float>(std::max(view_params.viewport_height_px, 1)), 1.0f);
  program_->setFloat("u_min_size_px", 0.5f * dpr);
  program_->setFloat("u_max_size_px", 32.0f * dpr);
  program_->setFloat("u_depth_threshold", 5.0f);
  program_->setInt("u_use_perspective_size", use_perspective_size ? 1 : 0);
  program_->setInt("u_scalar_axis", scalar_axis_);
  program_->setInt("u_color_mode", colorModeUniform(color_type_));
  program_->setVec3("u_solid_color", solid_color_);
  program_->setInt("u_colormap_id", static_cast<int>(colormap_));
  program_->setInt("u_invert", invert_lut_ ? 1 : 0);
  program_->setInt("u_shape_is_sphere", shape_is_sphere ? 1 : 0);
  vao_.bind();
  withGlFunctions(
      [this](auto& functions) { functions.glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(vbo_point_count_)); });
  vao_.unbind();
  unuseProgram();
  withGlFunctions([](auto& functions) { functions.glEnable(GL_BLEND); });
}

void PointcloudRenderPass::setActiveCloud(std::shared_ptr<const DecodedPointCloud> cloud) {
  if (cloud) {
    cloud_ = std::move(cloud);
  } else {
    cloud_ = std::monostate{};
  }
  cloud_dirty_ = true;
  cube_instance_bindings_dirty_ = true;
}

void PointcloudRenderPass::setActiveFastCloud(FastCloudData cloud) {
  cloud_ = std::move(cloud);
  cloud_dirty_ = true;
  cube_instance_bindings_dirty_ = true;
}

void PointcloudRenderPass::setColormapRange(float min_value, float max_value) {
  range_min_ = min_value;
  range_max_ = max_value;
}

void PointcloudRenderPass::setScalarAxis(int axis) {
  scalar_axis_ = (axis >= 0 && axis <= 2) ? axis : -1;
}

void PointcloudRenderPass::setSpatialAutoBounds(std::optional<AABB> source_bounds) {
  spatial_auto_bounds_ = std::move(source_bounds);
}

void PointcloudRenderPass::setSizeMeters(float meters) {
  size_meters_ = std::max(0.0f, meters);
}

void PointcloudRenderPass::setSizePixels(float pixels) {
  size_pixels_ = std::max(1.0f, pixels);
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
