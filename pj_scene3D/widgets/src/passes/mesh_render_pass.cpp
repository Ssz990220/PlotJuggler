// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/mesh_render_pass.h"

#include <fmt/core.h>

#include <QImage>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iterator>
#include <numbers>
#include <string_view>
#include <utility>
#include <variant>

#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d {
namespace {

struct GpuVertex {
  glm::vec3 position;
  glm::vec3 normal;
  glm::vec4 color;
  glm::vec2 uv;
  glm::vec4 tangent;  // .xyz tangent, .w bitangent handedness sign (normal mapping)
};
static_assert(sizeof(GpuVertex) == 64);
static_assert(offsetof(GpuVertex, uv) == 40);
static_assert(offsetof(GpuVertex, tangent) == 48);

constexpr std::string_view kVertSrc = R"(#version 450 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_tangent;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform mat3 u_normal_mat;

out vec3 v_world_pos;
out vec3 v_world_normal;
out vec4 v_vertex_color;
out vec2 v_uv;
out mat3 v_TBN;

void main() {
  vec4 world = u_model * vec4(in_pos, 1.0);
  v_world_pos = world.xyz;
  vec3 N = normalize(u_normal_mat * in_normal);
  v_world_normal = N;
  // Tangent-space basis for normal mapping. Re-orthonormalize T against N and
  // reconstruct B from the stored handedness; benign when no normal map is bound.
  vec3 T = u_normal_mat * in_tangent.xyz;
  T = normalize(T - N * dot(N, T));
  vec3 B = cross(N, T) * in_tangent.w;
  v_TBN = mat3(T, B, N);
  v_vertex_color = in_color;
  v_uv = in_uv;
  gl_Position = u_proj * u_view * world;
}
)";

constexpr std::string_view kFragSrc = R"(#version 450 core
in vec3 v_world_pos;
in vec3 v_world_normal;
in vec4 v_vertex_color;
in vec2 v_uv;
in mat3 v_TBN;

uniform vec4 u_base_color_factor;  // material albedo factor
uniform vec4 u_object_tint;        // per-draw override tint (URDF/marker color)
uniform vec3 u_emissive_factor;
uniform float u_metallic;
uniform float u_roughness;
uniform float u_dielectric_f0;     // f0 for non-metals (view MeshShadingParams)
uniform float u_opacity;
uniform bool u_use_vertex_color;
uniform bool u_collision;
uniform int u_alpha_mode;          // 0 opaque, 1 mask, 2 blend
uniform float u_alpha_cutoff;

uniform sampler2D u_base_tex;
uniform sampler2D u_mr_tex;
uniform sampler2D u_normal_tex;
uniform sampler2D u_ao_tex;
uniform sampler2D u_emissive_tex;
uniform bool u_has_base_tex;
uniform bool u_has_mr_tex;
uniform bool u_has_normal_tex;
uniform bool u_has_ao_tex;
uniform bool u_has_emissive_tex;

uniform vec3 u_camera_pos;
uniform float u_ambient_scale;
uniform float u_direct_scale;

out vec4 frag;

const float PI = 3.14159265;

float D_GGX(float NoH, float a) {
  float a2 = a * a;
  float d = (NoH * NoH) * (a2 - 1.0) + 1.0;
  return a2 / max(PI * d * d, 1e-5);
}

float V_SmithGGXCorrelated(float NoV, float NoL, float a) {
  float a2 = a * a;
  float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
  float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
  return 0.5 / max(gv + gl, 1e-5);
}

vec3 F_Schlick(vec3 f0, float VoH) {
  return f0 + (1.0 - f0) * pow(clamp(1.0 - VoH, 0.0, 1.0), 5.0);
}

void main() {
  // Base color, two modes matching the draw-call contract:
  //  - material-driven (u_use_vertex_color): glTF baseColorFactor is LINEAR, times
  //    the genuine per-vertex COLOR_0 (display-sRGB, linearized; white when absent).
  //  - override (else): the per-draw color (URDF link / marker override /
  //    placeholder) is display-sRGB and REPLACES the material factor.
  // The base texture is linear via GL_SRGB8_ALPHA8 and multiplies either mode.
  vec3 base_rgb;
  float base_a;
  if (u_use_vertex_color) {
    base_rgb = u_base_color_factor.rgb * pow(max(v_vertex_color.rgb, vec3(0.0)), vec3(2.2));
    base_a = u_base_color_factor.a * v_vertex_color.a;
  } else {
    base_rgb = pow(max(u_object_tint.rgb, vec3(0.0)), vec3(2.2));
    base_a = u_object_tint.a;
  }
  if (u_has_base_tex) {
    vec4 t = texture(u_base_tex, v_uv);
    base_rgb *= t.rgb;
    base_a *= t.a;
  }
  if (u_alpha_mode == 1 && base_a < u_alpha_cutoff) {
    discard;
  }
  vec3 base = max(base_rgb, vec3(0.0));
  float alpha = clamp(base_a * u_opacity, 0.0, 1.0);

  // Metallic-roughness (glTF packing: G = roughness, B = metallic).
  float metal = clamp(u_metallic, 0.0, 1.0);
  float roughness = u_collision ? 0.85 : u_roughness;
  if (u_has_mr_tex) {
    vec3 mr = texture(u_mr_tex, v_uv).rgb;
    roughness *= mr.g;
    metal *= mr.b;
  }
  roughness = clamp(roughness, 0.045, 1.0);
  float a = roughness * roughness;

  vec3 N = normalize(v_world_normal);
  if (u_has_normal_tex) {
    vec3 n = texture(u_normal_tex, v_uv).xyz * 2.0 - 1.0;
    N = normalize(v_TBN * n);
  }
  vec3 V = normalize(u_camera_pos - v_world_pos);
  // Camera headlight: the key light follows the view so the side facing the
  // viewer is always lit (RViz-style), with a small world-up bias for soft shape.
  vec3 L = normalize(V + vec3(0.0, 0.0, 0.25));
  vec3 H = normalize(V + L);
  float NoL = max(dot(N, L), 0.0);
  float NoV = max(dot(N, V), 0.0);
  float NoH = max(dot(N, H), 0.0);
  float VoH = max(dot(V, H), 0.0);

  // Metalness workflow: metals take their f0 from albedo and have no diffuse.
  vec3 f0 = mix(vec3(u_dielectric_f0), base, metal);
  vec3 diffuse_color = base * (1.0 - metal);
  vec3 F = F_Schlick(f0, VoH);
  float spec = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoL, a);
  vec3 direct = (diffuse_color / PI * (1.0 - F) + F * spec) * NoL;

  float hemi = dot(N, vec3(0.0, 0.0, 1.0)) * 0.5 + 0.5;
  vec3 ambient = mix(vec3(0.28, 0.27, 0.25), vec3(0.50, 0.52, 0.55), hemi) * diffuse_color;
  float ao = u_has_ao_tex ? texture(u_ao_tex, v_uv).r : 1.0;
  vec3 color = ambient * ao * u_ambient_scale + direct * u_direct_scale;
  if (u_collision) {
    color = mix(color, base, 0.35);
  }

  vec3 emissive = u_emissive_factor;
  if (u_has_emissive_tex) {
    emissive *= texture(u_emissive_tex, v_uv).rgb;
  }
  color += emissive;

  frag = vec4(color, alpha);
}
)";

// Alpha at or above this renders in the opaque bucket; below it, the draw needs
// blending. Strictly-below-1 comparison with a float epsilon margin.
constexpr float kOpaqueAlphaThreshold = 0.999f;

// Renderer-side guard for SubMesh::material: the loader always populates it, but
// a default-constructed SubMesh carries nullptr — shade it neutrally instead of
// dereferencing null (see the invariant documented on SubMesh in mesh_data.h).
const Material& fallbackMaterial() {
  static const Material material;
  return material;
}

// A solid, untextured material for the procedural URDF primitives (box/cylinder/
// sphere). has_pbr stays false so they inherit the scene-wide shading defaults.
Material solidColorMaterial(glm::vec4 color) {
  Material material;
  material.base_color_factor = color;
  return material;
}

MeshData makeCube(glm::vec4 color) {
  constexpr glm::vec3 positions[] = {
      {0.5f, -0.5f, -0.5f},  {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},   {0.5f, 0.5f, -0.5f},   {-0.5f, -0.5f, 0.5f},
      {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, -0.5f},  {0.5f, 0.5f, -0.5f},
      {0.5f, 0.5f, 0.5f},    {-0.5f, 0.5f, 0.5f},  {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f},   {0.5f, -0.5f, -0.5f},
      {-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},    {0.5f, -0.5f, 0.5f},
      {0.5f, -0.5f, -0.5f},  {0.5f, 0.5f, -0.5f},  {-0.5f, 0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f},
  };
  constexpr glm::vec3 normals[] = {
      {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0},
      {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0},
      {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1},
  };
  constexpr std::uint32_t indices[] = {
      0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
      12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
  };
  MeshData out;
  out.ok = true;
  out.vertices.reserve(std::size(positions));
  for (std::size_t i = 0; i < std::size(positions); ++i) {
    out.vertices.push_back(Vertex{positions[i], normals[i], color});
  }
  out.indices.assign(std::begin(indices), std::end(indices));
  out.submeshes.push_back(SubMesh{0, out.indices.size(), std::make_shared<const Material>(solidColorMaterial(color))});
  return out;
}

MeshData makeCylinder() {
  constexpr int kSegments = 40;
  MeshData out;
  out.ok = true;
  const glm::vec4 color{0.7f, 0.7f, 0.7f, 1.0f};
  for (int i = 0; i < kSegments; ++i) {
    const float a = static_cast<float>(i) * 2.0f * std::numbers::pi_v<float> / static_cast<float>(kSegments);
    const float x = std::cos(a);
    const float y = std::sin(a);
    out.vertices.push_back(Vertex{{x, y, -0.5f}, glm::normalize(glm::vec3{x, y, 0.0f}), color});
    out.vertices.push_back(Vertex{{x, y, 0.5f}, glm::normalize(glm::vec3{x, y, 0.0f}), color});
  }
  const std::uint32_t top_center = static_cast<std::uint32_t>(out.vertices.size());
  out.vertices.push_back(Vertex{{0, 0, 0.5f}, {0, 0, 1}, color});
  const std::uint32_t bottom_center = static_cast<std::uint32_t>(out.vertices.size());
  out.vertices.push_back(Vertex{{0, 0, -0.5f}, {0, 0, -1}, color});
  for (int i = 0; i < kSegments; ++i) {
    const auto a = static_cast<std::uint32_t>(2 * i);
    const auto b = static_cast<std::uint32_t>(2 * ((i + 1) % kSegments));
    out.indices.insert(out.indices.end(), {a, b, a + 1, b, b + 1, a + 1});
    out.indices.insert(out.indices.end(), {top_center, a + 1, b + 1, bottom_center, b, a});
  }
  out.submeshes.push_back(SubMesh{0, out.indices.size(), std::make_shared<const Material>(solidColorMaterial(color))});
  return out;
}

MeshData makeSphere() {
  constexpr int kLat = 16;
  constexpr int kLon = 32;
  MeshData out;
  out.ok = true;
  const glm::vec4 color{0.7f, 0.7f, 0.7f, 1.0f};
  for (int lat = 0; lat <= kLat; ++lat) {
    const float theta = static_cast<float>(lat) * std::numbers::pi_v<float> / static_cast<float>(kLat);
    const float z = std::cos(theta);
    const float r = std::sin(theta);
    for (int lon = 0; lon <= kLon; ++lon) {
      const float phi = static_cast<float>(lon) * 2.0f * std::numbers::pi_v<float> / static_cast<float>(kLon);
      const glm::vec3 p{r * std::cos(phi), r * std::sin(phi), z};
      out.vertices.push_back(Vertex{p, glm::normalize(p), color});
    }
  }
  for (int lat = 0; lat < kLat; ++lat) {
    for (int lon = 0; lon < kLon; ++lon) {
      const auto a = static_cast<std::uint32_t>(lat * (kLon + 1) + lon);
      const auto b = static_cast<std::uint32_t>((lat + 1) * (kLon + 1) + lon);
      out.indices.insert(out.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
    }
  }
  out.submeshes.push_back(SubMesh{0, out.indices.size(), std::make_shared<const Material>(solidColorMaterial(color))});
  return out;
}

}  // namespace

const MeshRenderPass::CachedTexture* MeshRenderPass::findCachedTexture(
    const std::vector<CachedTexture>& cache, std::string_view key, TextureColorSpace color_space) {
  const auto it = std::find_if(cache.begin(), cache.end(), [key, color_space](const CachedTexture& item) {
    return item.color_space == color_space && item.key == key;
  });
  return it == cache.end() ? nullptr : &(*it);
}

MeshRenderPass::MeshRenderPass() {
  cube_.data = makeCube({0.7f, 0.7f, 0.7f, 1.0f});
  cube_.has_blended_material = meshHasBlendedMaterial(cube_.data);
  cube_.dirty = true;
  cylinder_.data = makeCylinder();
  cylinder_.has_blended_material = meshHasBlendedMaterial(cylinder_.data);
  cylinder_.dirty = true;
  sphere_.data = makeSphere();
  sphere_.has_blended_material = meshHasBlendedMaterial(sphere_.data);
  sphere_.dirty = true;
}

MeshRenderPass::~MeshRenderPass() = default;

void MeshRenderPass::initializeGL() {
  if (initialized_) {
    return;
  }
  auto result = gl::Program::fromSources(kVertSrc, kFragSrc);
  if (auto* program = std::get_if<gl::Program>(&result); program != nullptr) {
    program_ = std::make_unique<gl::Program>(std::move(*program));
    initialized_ = true;
    const GLuint pid = program_->id();
    withGlFunctions([this, pid](auto& functions) {
      uniforms_.has_base = functions.glGetUniformLocation(pid, "u_has_base_tex");
      uniforms_.has_mr = functions.glGetUniformLocation(pid, "u_has_mr_tex");
      uniforms_.has_normal = functions.glGetUniformLocation(pid, "u_has_normal_tex");
      uniforms_.has_ao = functions.glGetUniformLocation(pid, "u_has_ao_tex");
      uniforms_.has_emissive = functions.glGetUniformLocation(pid, "u_has_emissive_tex");
      uniforms_.base_factor = functions.glGetUniformLocation(pid, "u_base_color_factor");
      uniforms_.metallic = functions.glGetUniformLocation(pid, "u_metallic");
      uniforms_.roughness = functions.glGetUniformLocation(pid, "u_roughness");
      uniforms_.emissive_factor = functions.glGetUniformLocation(pid, "u_emissive_factor");
      uniforms_.alpha_mode = functions.glGetUniformLocation(pid, "u_alpha_mode");
      uniforms_.alpha_cutoff = functions.glGetUniformLocation(pid, "u_alpha_cutoff");
    });
  } else {
    fmt::print(stderr, "MeshRenderPass shader error: {}\n", std::get<std::string>(result));
  }
}

void MeshRenderPass::render(const ViewParams& /*view_params*/, const FrameContext& /*frame_ctx*/) {}

void MeshRenderPass::releaseGL() {
  // Free anything retired by clearMeshes() first — the view calls releaseGL()
  // under the dying context's makeCurrent, the right place to delete its names.
  drainRetired();
  textures_.clear();
  program_.reset();
  initialized_ = false;
  auto release = [](MeshResource& resource) {
    resource.vao = gl::VertexArray{};
    resource.vbo = gl::Buffer{};
    resource.ebo = gl::Buffer{};
    resource.uploaded = false;
    resource.dirty = resource.data.ok;
    resource.index_count = 0;
  };
  release(cube_);
  release(cylinder_);
  release(sphere_);
  for (auto& [_, resource] : meshes_) {
    release(resource);
  }
}

void MeshRenderPass::clearMeshes() {
  // Context-free: move the GL wrappers onto the retirement lists instead of
  // destroying them here (this runs on the GUI thread from layer detach /
  // source swaps with no GL context current). drainRetired() frees them later
  // under the owning context. See the header for the deferred-teardown contract.
  retired_textures_.insert(
      retired_textures_.end(), std::make_move_iterator(textures_.begin()), std::make_move_iterator(textures_.end()));
  textures_.clear();
  retired_meshes_.insert(
      retired_meshes_.end(), std::make_move_iterator(meshes_.begin()), std::make_move_iterator(meshes_.end()));
  meshes_.clear();
}

void MeshRenderPass::drainRetired() {
  // The wrapper destructors glDelete* against the current context, so this MUST
  // be called under the owning context (drawBatch inside paintGL, or releaseGL
  // under SceneViewWidget's makeCurrent).
  retired_textures_.clear();
  retired_meshes_.clear();
}

void MeshRenderPass::setMeshData(const std::string& key, MeshData data) {
  auto* existing = meshResource(key);
  if (existing != nullptr) {
    existing->data = std::move(data);
    existing->has_blended_material = meshHasBlendedMaterial(existing->data);
    existing->dirty = true;
    existing->uploaded = false;
    return;
  }
  MeshResource resource;
  resource.data = std::move(data);
  resource.has_blended_material = meshHasBlendedMaterial(resource.data);
  resource.dirty = true;
  meshes_.emplace_back(key, std::move(resource));
}

void MeshRenderPass::renderVisuals(const ViewParams& view_params, const std::vector<DrawCall>& draws, float opacity) {
  drawBatch(view_params, draws, opacity, false);
}

void MeshRenderPass::renderCollisions(
    const ViewParams& view_params, const std::vector<DrawCall>& draws, float opacity) {
  drawBatch(view_params, draws, opacity, true);
}

MeshRenderPass::MeshResource& MeshRenderPass::resourceFor(GeometryKind kind) {
  if (kind == GeometryKind::kCylinder) {
    return cylinder_;
  }
  if (kind == GeometryKind::kSphere) {
    return sphere_;
  }
  return cube_;
}

const MeshRenderPass::MeshResource* MeshRenderPass::meshResource(const std::string& key) const {
  const auto it = std::find_if(meshes_.begin(), meshes_.end(), [&key](const auto& p) { return p.first == key; });
  return it == meshes_.end() ? nullptr : &it->second;
}

MeshRenderPass::MeshResource* MeshRenderPass::meshResource(const std::string& key) {
  auto it = std::find_if(meshes_.begin(), meshes_.end(), [&key](const auto& p) { return p.first == key; });
  return it == meshes_.end() ? nullptr : &it->second;
}

MeshRenderPass::MeshResource* MeshRenderPass::resourceForDraw(const DrawCall& draw) {
  MeshResource* resource = draw.kind == GeometryKind::kMesh ? meshResource(draw.mesh_key) : &resourceFor(draw.kind);
  if (resource == nullptr || !resource->data.ok) {
    resource = &resourceFor(GeometryKind::kPlaceholderCube);
  }
  return resource;
}

bool MeshRenderPass::meshHasBlendedMaterial(const MeshData& data) {
  for (const SubMesh& submesh : data.submeshes) {
    const Material& mat = submesh.material != nullptr ? *submesh.material : fallbackMaterial();
    if (mat.alpha_mode == AlphaMode::kBlend) {
      return true;
    }
  }
  return false;
}

void MeshRenderPass::uploadIfNeeded(MeshResource& resource) {
  if (!resource.dirty && resource.uploaded) {
    return;
  }
  if (!resource.data.ok || resource.data.vertices.empty() || resource.data.indices.empty()) {
    resource.index_count = 0;
    resource.dirty = false;
    resource.uploaded = false;
    return;
  }

  std::vector<GpuVertex> vertices;
  vertices.reserve(resource.data.vertices.size());
  for (const Vertex& v : resource.data.vertices) {
    vertices.push_back(GpuVertex{v.position, v.normal, v.color, v.uv, v.tangent});
  }
  resource.vbo.uploadStatic(
      GL_ARRAY_BUFFER, vertices.data(), static_cast<GLsizeiptr>(vertices.size() * sizeof(GpuVertex)));
  resource.ebo.uploadStatic(
      GL_ELEMENT_ARRAY_BUFFER, resource.data.indices.data(),
      static_cast<GLsizeiptr>(resource.data.indices.size() * sizeof(std::uint32_t)));

  resource.vao.bind();
  resource.vbo.bind(GL_ARRAY_BUFFER);
  withGlFunctions([](auto& functions) {
    functions.glEnableVertexAttribArray(0U);
    functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(GpuVertex)), nullptr);
    functions.glEnableVertexAttribArray(1U);
    functions.glVertexAttribPointer(
        1U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(GpuVertex)), reinterpret_cast<const void*>(12));
    functions.glEnableVertexAttribArray(2U);
    functions.glVertexAttribPointer(
        2U, 4, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(GpuVertex)), reinterpret_cast<const void*>(24));
    functions.glEnableVertexAttribArray(3U);
    functions.glVertexAttribPointer(
        3U, 2, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(GpuVertex)), reinterpret_cast<const void*>(40));
    functions.glEnableVertexAttribArray(4U);
    functions.glVertexAttribPointer(
        4U, 4, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(GpuVertex)), reinterpret_cast<const void*>(48));
  });
  resource.ebo.bind(GL_ELEMENT_ARRAY_BUFFER);
  resource.vao.unbind();

  resource.index_count = resource.data.indices.size();
  resource.uploaded = true;
  resource.dirty = false;
}

gl::Texture2D MeshRenderPass::uploadTexture(const QImage& source, TextureColorSpace color_space) {
  if (source.isNull()) {
    return {};
  }
  QImage image = source;
  constexpr int kMaxTextureEdge = 1024;
  const int longest_edge = std::max(image.width(), image.height());
  if (longest_edge > kMaxTextureEdge) {
    image = image.scaled(kMaxTextureEdge, kMaxTextureEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  }
  const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);

  GLuint id = 0U;
  withGlFunctions([&rgba, &id, color_space](auto& functions) {
    functions.glGenTextures(1, &id);
    if (id == 0U) {
      return;
    }
    functions.glActiveTexture(GL_TEXTURE0);
    functions.glBindTexture(GL_TEXTURE_2D, id);
    functions.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    // Color/emissive maps upload as sRGB (hardware linearizes on sample, matching
    // the linear-light BRDF); data maps (metallic-roughness, normal, occlusion)
    // upload as linear RGBA8 so their channels are read verbatim.
    const GLint internal_format = color_space == TextureColorSpace::kSrgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    functions.glTexImage2D(
        GL_TEXTURE_2D, 0, internal_format, static_cast<GLsizei>(rgba.width()), static_cast<GLsizei>(rgba.height()), 0,
        GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
    functions.glGenerateMipmap(GL_TEXTURE_2D);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    functions.glBindTexture(GL_TEXTURE_2D, 0U);
  });

  gl::Texture2D texture;
  texture.adopt(id);
  return texture;
}

GLuint MeshRenderPass::textureIdFor(const TextureSource& source, TextureColorSpace color_space) {
  if (source.empty()) {
    return 0U;
  }
  if (const CachedTexture* hit = findCachedTexture(textures_, source.key, color_space); hit != nullptr) {
    return hit->texture.id();
  }

  // External maps load from disk; embedded maps decode their inline bytes (the
  // glTF/GLB still-encoded PNG/JPEG). Failed loads cache id 0 so we don't retry.
  QImage image;
  if (!source.bytes.empty()) {
    image = QImage::fromData(source.bytes.data(), static_cast<int>(source.bytes.size()));
  } else {
    image = QImage(source.path);
  }
  if (image.isNull()) {
    fmt::print(stderr, "MeshRenderPass: failed to load texture '{}'\n", source.key);
  }
  textures_.push_back(CachedTexture{source.key, color_space, uploadTexture(image, color_space)});
  return textures_.back().texture.id();
}

bool MeshRenderPass::drawNeedsVisualBlending(const DrawCall& draw, bool mesh_has_blended_material, float opacity) {
  if (opacity < kOpaqueAlphaThreshold) {
    return true;
  }
  if (!draw.use_vertex_color && draw.color.a < kOpaqueAlphaThreshold) {
    return true;
  }
  return mesh_has_blended_material;
}

void MeshRenderPass::drawOne(
    const ViewParams& view_params, const DrawCall& draw, MeshResource& resource, float opacity) {
  uploadIfNeeded(resource);
  if (!resource.uploaded || resource.index_count == 0U || program_ == nullptr) {
    return;
  }
  const glm::mat3 normal_mat = glm::inverseTranspose(glm::mat3(draw.model));
  const MeshShadingParams& shading = view_params.shading;
  program_->setMat4("u_model", draw.model);
  program_->setMat4("u_view", view_params.view);
  program_->setMat4("u_proj", view_params.proj);
  // u_camera_pos is view-constant and set once per batch in drawBatch (L.56).
  program_->setMat3("u_normal_mat", normal_mat);
  program_->setFloat("u_opacity", opacity);
  program_->setInt("u_use_vertex_color", draw.use_vertex_color ? 1 : 0);
  program_->setVec4("u_object_tint", draw.color);  // per-draw override (URDF/marker color)
  program_->setFloat("u_dielectric_f0", shading.reflectivity);
  program_->setFloat("u_ambient_scale", shading.ambient_scale);
  program_->setFloat("u_direct_scale", shading.direct_scale);
  // Fixed sampler units: base=0, metallic-roughness=1, normal=2, occlusion=3, emissive=4.
  program_->setInt("u_base_tex", 0);
  program_->setInt("u_mr_tex", 1);
  program_->setInt("u_normal_tex", 2);
  program_->setInt("u_ao_tex", 3);
  program_->setInt("u_emissive_tex", 4);
  resource.vao.bind();
  withGlFunctions([this, &resource, &shading](auto& functions) {
    // Locations were cached once at link time (initializeGL), not re-queried here.
    const GLint loc_has_base = uniforms_.has_base;
    const GLint loc_has_mr = uniforms_.has_mr;
    const GLint loc_has_normal = uniforms_.has_normal;
    const GLint loc_has_ao = uniforms_.has_ao;
    const GLint loc_has_emissive = uniforms_.has_emissive;
    const GLint loc_base_factor = uniforms_.base_factor;
    const GLint loc_metallic = uniforms_.metallic;
    const GLint loc_roughness = uniforms_.roughness;
    const GLint loc_emissive_factor = uniforms_.emissive_factor;
    const GLint loc_alpha_mode = uniforms_.alpha_mode;
    const GLint loc_alpha_cutoff = uniforms_.alpha_cutoff;

    const auto bindUnit = [&functions](int unit, GLuint texture_id) {
      functions.glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
      functions.glBindTexture(GL_TEXTURE_2D, texture_id);
    };

    // glTF doubleSided drops back-face culling for those submeshes only; the
    // caller's cull state is restored after the loop (this pass never enables
    // culling itself).
    const bool cull_was_enabled = functions.glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    bool cull_enabled = cull_was_enabled;

    for (const SubMesh& submesh : resource.data.submeshes) {
      const Material& mat = submesh.material != nullptr ? *submesh.material : fallbackMaterial();
      const bool wants_cull = cull_was_enabled && !mat.double_sided;
      if (wants_cull != cull_enabled) {
        if (wants_cull) {
          functions.glEnable(GL_CULL_FACE);
        } else {
          functions.glDisable(GL_CULL_FACE);
        }
        cull_enabled = wants_cull;
      }
      const GLuint base_tex = textureIdFor(mat.base_color, textureColorSpaceForSlot(MaterialTextureSlot::kBaseColor));
      const GLuint mr_tex =
          textureIdFor(mat.metallic_roughness, textureColorSpaceForSlot(MaterialTextureSlot::kMetallicRoughness));
      const GLuint normal_tex = textureIdFor(mat.normal, textureColorSpaceForSlot(MaterialTextureSlot::kNormal));
      const GLuint ao_tex = textureIdFor(mat.occlusion, textureColorSpaceForSlot(MaterialTextureSlot::kOcclusion));
      const GLuint emissive_tex = textureIdFor(mat.emissive, textureColorSpaceForSlot(MaterialTextureSlot::kEmissive));
      bindUnit(0, base_tex);
      bindUnit(1, mr_tex);
      bindUnit(2, normal_tex);
      bindUnit(3, ao_tex);
      bindUnit(4, emissive_tex);
      if (loc_has_base >= 0) {
        functions.glUniform1i(loc_has_base, base_tex != 0U ? 1 : 0);
      }
      if (loc_has_mr >= 0) {
        functions.glUniform1i(loc_has_mr, mr_tex != 0U ? 1 : 0);
      }
      if (loc_has_normal >= 0) {
        functions.glUniform1i(loc_has_normal, normal_tex != 0U ? 1 : 0);
      }
      if (loc_has_ao >= 0) {
        functions.glUniform1i(loc_has_ao, ao_tex != 0U ? 1 : 0);
      }
      if (loc_has_emissive >= 0) {
        functions.glUniform1i(loc_has_emissive, emissive_tex != 0U ? 1 : 0);
      }
      if (loc_base_factor >= 0) {
        functions.glUniform4f(
            loc_base_factor, mat.base_color_factor.r, mat.base_color_factor.g, mat.base_color_factor.b,
            mat.base_color_factor.a);
      }
      // Factors fall back to the scene-wide shading defaults for sources without
      // PBR (STL/primitives), preserving their look; the shader applies the
      // collision roughness override on top.
      if (loc_metallic >= 0) {
        functions.glUniform1f(loc_metallic, mat.has_pbr ? mat.metallic_factor : 0.0f);
      }
      if (loc_roughness >= 0) {
        functions.glUniform1f(loc_roughness, mat.has_pbr ? mat.roughness_factor : shading.roughness);
      }
      if (loc_emissive_factor >= 0) {
        functions.glUniform3f(loc_emissive_factor, mat.emissive_factor.r, mat.emissive_factor.g, mat.emissive_factor.b);
      }
      if (loc_alpha_mode >= 0) {
        functions.glUniform1i(loc_alpha_mode, static_cast<int>(mat.alpha_mode));
      }
      if (loc_alpha_cutoff >= 0) {
        functions.glUniform1f(loc_alpha_cutoff, mat.alpha_cutoff);
      }
      functions.glDrawElements(
          GL_TRIANGLES, static_cast<GLsizei>(submesh.index_count), GL_UNSIGNED_INT,
          reinterpret_cast<const void*>(submesh.index_offset * sizeof(std::uint32_t)));
    }
    if (cull_enabled != cull_was_enabled) {
      if (cull_was_enabled) {
        functions.glEnable(GL_CULL_FACE);
      } else {
        functions.glDisable(GL_CULL_FACE);
      }
    }
    for (int unit = 4; unit >= 0; --unit) {
      functions.glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
      functions.glBindTexture(GL_TEXTURE_2D, 0U);
    }
  });
  resource.vao.unbind();
}

void MeshRenderPass::drawBatch(
    const ViewParams& view_params, const std::vector<DrawCall>& draws, float opacity, bool collision) {
  if (draws.empty()) {
    return;
  }
  initializeGL();
  if (!initialized_ || program_ == nullptr) {
    return;
  }

  // We are inside paintGL with the owning context current — the safe point to
  // free anything clearMeshes() retired since the last draw.
  drainRetired();

  // Save the caller's blend state: this pass toggles GL_BLEND per bucket and
  // must not leak the change to later passes (HUD/overlays render after meshes).
  bool blend_was_enabled = false;
  withGlFunctions(
      [&blend_was_enabled](auto& functions) { blend_was_enabled = functions.glIsEnabled(GL_BLEND) == GL_TRUE; });
  const auto restoreBlend = [blend_was_enabled](auto& functions) {
    if (blend_was_enabled) {
      functions.glEnable(GL_BLEND);
    } else {
      functions.glDisable(GL_BLEND);
    }
  };

  program_->use();
  program_->setInt("u_collision", collision ? 1 : 0);
  // View-constant across every draw in this batch, so set it once here instead
  // of inverting view per draw inside drawOne (L.56). SceneViewWidget already
  // fills camera_pos_world from camera_->position().
  program_->setVec3("u_camera_pos", view_params.camera_pos_world);

  if (collision) {
    withGlFunctions([](auto& functions) {
      functions.glEnable(GL_BLEND);
      // Coverage-union alpha raises the scene FBO's tonemap marker by the
      // hull's coverage instead of preserving stale annotation markers (see
      // SceneViewWidget::renderScene).
      functions.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      functions.glDepthMask(GL_FALSE);
    });
    for (const DrawCall& draw : draws) {
      drawOne(view_params, draw, *resourceForDraw(draw), opacity);
    }
    withGlFunctions([&restoreBlend](auto& functions) {
      functions.glDepthMask(GL_TRUE);
      restoreBlend(functions);
      functions.glUseProgram(0U);
    });
    return;
  }

  // Visuals: bucket once into opaque and translucent (layer opacity, override
  // alpha, or a glTF kBlend material) reusing resolved_draws_ so steady frames
  // allocate nothing. Opaque draws first with blending off, then translucent
  // draws blend on top with a read-only depth buffer. Best-effort transparency:
  // draws are not depth-sorted.
  resolved_draws_.clear();
  resolved_draws_.reserve(draws.size());
  bool has_opaque = false;
  bool has_translucent = false;
  for (const DrawCall& draw : draws) {
    MeshResource* resource = resourceForDraw(draw);
    const bool translucent = drawNeedsVisualBlending(draw, resource->has_blended_material, opacity);
    has_opaque = has_opaque || !translucent;
    has_translucent = has_translucent || translucent;
    resolved_draws_.push_back(ResolvedDraw{&draw, resource, translucent});
  }

  if (has_opaque) {
    // Opaque data must overwrite the alpha tonemap marker. Leaving blending on
    // would preserve stale alpha=0 from earlier annotations under the mesh.
    withGlFunctions([](auto& functions) { functions.glDisable(GL_BLEND); });
    for (const ResolvedDraw& item : resolved_draws_) {
      if (!item.translucent) {
        drawOne(view_params, *item.draw, *item.resource, opacity);
      }
    }
  }

  if (has_translucent) {
    withGlFunctions([](auto& functions) {
      functions.glEnable(GL_BLEND);
      functions.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      functions.glDepthMask(GL_FALSE);
    });
    for (const ResolvedDraw& item : resolved_draws_) {
      if (item.translucent) {
        drawOne(view_params, *item.draw, *item.resource, opacity);
      }
    }
    withGlFunctions([](auto& functions) { functions.glDepthMask(GL_TRUE); });
  }

  withGlFunctions([&restoreBlend](auto& functions) {
    restoreBlend(functions);
    functions.glUseProgram(0U);
  });
}

}  // namespace pj::scene3d
