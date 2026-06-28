// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_scene3d_core/camera/camera.h"  // AABB (shadow caster bounds)
#include "pj_scene3d_widgets/gl/buffer.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/texture.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"
#include "pj_scene3d_widgets/mesh_data.h"
#include "pj_scene3d_widgets/mesh_shading_params.h"
#include "pj_scene3d_widgets/render_pass.h"

class QImage;

namespace pj::scene3d {

// GL upload color space for a material texture map: sRGB maps (GL_SRGB8_ALPHA8)
// are hardware-linearized on sample; linear maps (GL_RGBA8) are read verbatim.
enum class TextureColorSpace { kLinear, kSrgb };

// The five texture slots of the glTF 2.0 metallic-roughness material model.
enum class MaterialTextureSlot { kBaseColor, kMetallicRoughness, kNormal, kOcclusion, kEmissive };

// Color-space policy per material slot: base-color/emissive carry color data
// (sRGB); metallic-roughness/normal/occlusion carry linear data. The renderer —
// not the loader — owns this mapping, so the same image can legally serve a
// color slot in one material and a data slot in another.
[[nodiscard]] constexpr TextureColorSpace textureColorSpaceForSlot(MaterialTextureSlot slot) {
  switch (slot) {
    case MaterialTextureSlot::kBaseColor:
    case MaterialTextureSlot::kEmissive:
      return TextureColorSpace::kSrgb;
    case MaterialTextureSlot::kMetallicRoughness:
    case MaterialTextureSlot::kNormal:
    case MaterialTextureSlot::kOcclusion:
      return TextureColorSpace::kLinear;
  }
  return TextureColorSpace::kLinear;
}

// GL pass for URDF/scene meshes. RobotModelLayer resolves TF and visual origins
// into per-object DrawCalls; this pass owns shader compilation, CPU primitive
// meshes, mesh-data upload, and the visual-vs-collision GL state.
class MeshRenderPass : public IRenderPass {
 public:
  enum class GeometryKind {
    kMesh,
    kBox,
    kCylinder,
    kSphere,
    kPlaceholderCube,
  };

  struct DrawCall {
    GeometryKind kind{GeometryKind::kPlaceholderCube};
    std::string mesh_key;
    glm::mat4 model{1.0f};
    glm::vec4 color{0.7f, 0.7f, 0.7f, 1.0f};
    bool use_vertex_color{true};
  };

  // One entry of the per-pass texture cache. The same source key may legally
  // appear twice — once sRGB, once linear — when one image serves both a color
  // slot and a data slot (textureColorSpaceForSlot decides per slot).
  struct CachedTexture {
    std::string key;
    TextureColorSpace color_space{TextureColorSpace::kLinear};
    gl::Texture2D texture;
  };

  // Cache lookup keyed by (source key, color space); nullptr on miss. Static and
  // GL-free so the dedup rule stays unit-testable without a live context.
  [[nodiscard]] static const CachedTexture* findCachedTexture(
      const std::vector<CachedTexture>& cache, std::string_view key, TextureColorSpace color_space);

  // Any submesh material with AlphaMode::kBlend (null materials count as opaque).
  // Cached per mesh at setMeshData() so drawBatch never re-walks submeshes.
  [[nodiscard]] static bool meshHasBlendedMaterial(const MeshData& data);

  // Model-space AABB enclosing every vertex of `data` (GL-free, static). Empty or
  // failed mesh data yields an invalid box. The shadow pre-pass lifts it into world
  // space — transformedAABB(draw.model, localBounds(data)) — to fit the light
  // frustum to the mesh casters (which scene_bounds_ deliberately excludes).
  [[nodiscard]] static AABB localBounds(const MeshData& data);

  // True when a visual draw belongs in the translucent bucket: layer opacity < 1,
  // an override tint with alpha < 1, or a mesh with any glTF kBlend material
  // (pass the cached meshHasBlendedMaterial result). Static and GL-free so the
  // opaque/translucent split stays unit-testable without a live context.
  [[nodiscard]] static bool drawNeedsVisualBlending(
      const DrawCall& draw, bool mesh_has_blended_material, float opacity);

  MeshRenderPass();
  ~MeshRenderPass() override;

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;

  // Drop all keyed meshes and cached textures. Safe to call with NO GL context
  // current (e.g. from a layer's detach()/source-swap on the GUI thread): the GL
  // wrappers are moved onto retirement lists, not destroyed here. Their GL
  // teardown is deferred to the next drawBatch()/releaseGL(), which run under the
  // owning context — so handles free in the right context instead of leaking or
  // deleting a sibling view's names.
  void clearMeshes();
  // Store/replace a keyed mesh's CPU data; the GL upload is deferred to the next
  // drawBatch() under a current context. Safe to call off the GL thread.
  void setMeshData(const std::string& key, MeshData data);

  // Draw the opaque + translucent visual buckets. REQUIRES a current GL context
  // (call only from a layer's render()). Establishes its own blend state: opaque
  // draws with GL_BLEND off, then translucent draws with a coverage-union alpha
  // blend and a read-only depth buffer (not depth-sorted); the caller's
  // GL_BLEND enable is saved and restored. `opacity` multiplies material/vertex
  // alpha and selects the translucent bucket when < 1; callers pass the per-view
  // MeshShadingParams::mesh_opacity.
  void renderVisuals(const ViewParams& view_params, const std::vector<DrawCall>& draws, float opacity);
  // Draw the collision overlay. REQUIRES a current GL context. Establishes a
  // coverage-union alpha blend with the depth mask OFF (overlay-on-top); the
  // caller's GL_BLEND enable is saved and restored. `opacity` multiplies
  // material/vertex alpha; callers pass MeshShadingParams::collision_opacity.
  void renderCollisions(const ViewParams& view_params, const std::vector<DrawCall>& draws, float opacity);

  // World-space AABB enclosing `draws` as shadow casters: the union of each draw's
  // model-transformed local bounds (transformedAABB(draw.model, resource bounds)).
  // GL-free at runtime (resources cache their local AABB at build/setMeshData), so
  // the shadow pre-pass can fit the light frustum to the meshes the camera AABB
  // omits. Unknown mesh keys fall back to the placeholder cube's bounds, matching
  // what actually draws. Returns an invalid box when `draws` is empty.
  [[nodiscard]] AABB worldBoundsOfDraws(const std::vector<DrawCall>& draws);

  // Depth-only render of `draws` from the light's point of view into the currently-
  // bound shadow FBO (ShadowMapPass::begin() must have run). Uses the pass's own
  // depth-only program: u_light_vp once, u_model per draw, reusing each mesh's
  // existing VAO (position is attribute 0; no materials, no lighting). REQUIRES a
  // current GL context. The collision bucket is intentionally NOT a caster (a hull
  // coincident with the visual mesh would double-darken its own silhouette).
  void renderDepthOnly(const glm::mat4& light_view_proj, const std::vector<DrawCall>& draws);

  // GL names of a keyed mesh's vertex-array object and element buffer after upload,
  // or nullopt if the key is unknown / not yet uploaded. Test seam only: the real-GL
  // regression test binds the VAO and asserts its recorded GL_ELEMENT_ARRAY_BUFFER is
  // this same EBO. uploadIfNeeded() once uploaded the EBO before binding its own VAO,
  // so a prior caster's VAO (left bound by renderDepthOnly) had its index buffer
  // hijacked — the root cause of mesh "shadow acne" speckle under streaming.
  struct GlNamesForTest {
    unsigned int vao;
    unsigned int ebo;
  };
  [[nodiscard]] std::optional<GlNamesForTest> resourceGlNamesForTest(const std::string& key) const;

 private:
  struct MeshResource {
    MeshData data;
    gl::VertexArray vao;
    gl::Buffer vbo;
    gl::Buffer ebo;
    std::size_t index_count{0};
    bool dirty{false};
    bool uploaded{false};
    // Any submesh material with AlphaMode::kBlend, cached at setMeshData() so
    // drawBatch can bucket draws without re-walking submeshes every frame.
    bool has_blended_material{false};
    // Model-space AABB of `data`, cached alongside it (at the ctor for primitives,
    // at setMeshData for keyed meshes) so worldBoundsOfDraws never rescans vertices
    // and works without a GL context (the CPU data is set before upload).
    AABB local_bounds;
  };

  // One bucketed visual draw: the resolved mesh resource plus whether it goes
  // into the translucent pass. Pointers borrow from `draws`/`meshes_` for the
  // duration of a single drawBatch call only.
  struct ResolvedDraw {
    const DrawCall* draw{nullptr};
    MeshResource* resource{nullptr};
    bool translucent{false};
  };

  MeshResource& resourceFor(GeometryKind kind);
  const MeshResource* meshResource(const std::string& key) const;
  MeshResource* meshResource(const std::string& key);
  // Resolve a draw to its mesh resource, falling back to the magenta placeholder
  // cube for unknown keys or failed imports.
  MeshResource* resourceForDraw(const DrawCall& draw);
  void uploadIfNeeded(MeshResource& resource);
  // Upload a decoded image as a GL texture. `color_space` selects GL_SRGB8_ALPHA8
  // (color/emissive maps, hardware-linearized on sample) vs GL_RGBA8 (linear data
  // maps: metallic-roughness/normal/occlusion).
  gl::Texture2D uploadTexture(const QImage& image, TextureColorSpace color_space);
  // Resolve a material texture map to a cached GL texture name (0 for
  // empty/failed sources). Returned BY VALUE: textures_ reallocates as new maps
  // are cached, so handing out element pointers would dangle. The cache is keyed
  // by (TextureSource::key, color_space) — see findCachedTexture. External maps
  // load from disk; embedded maps decode their inline bytes via QImage::fromData.
  [[nodiscard]] GLuint textureIdFor(const TextureSource& source, TextureColorSpace color_space);
  void drawOne(const ViewParams& view_params, const DrawCall& draw, MeshResource& resource, float opacity);
  void drawBatch(const ViewParams& view_params, const std::vector<DrawCall>& draws, float opacity, bool collision);
  // Destroy meshes/textures retired by clearMeshes(). MUST run under the owning
  // GL context: it clears retired_meshes_/retired_textures_, whose gl wrapper
  // destructors glDelete* against the current context. Called from drawBatch()
  // (inside paintGL) and releaseGL() (the view runs it under makeCurrent).
  void drainRetired();

  // Per-submesh material uniform locations, queried once when the program links
  // (initializeGL) instead of re-querying ~11 string-keyed locations per draw per
  // frame. Re-cached after GL-context recreation (releaseGL resets initialized_).
  struct MaterialUniforms {
    GLint has_base{-1}, has_mr{-1}, has_normal{-1}, has_ao{-1}, has_emissive{-1};
    GLint base_factor{-1}, metallic{-1}, roughness{-1}, emissive_factor{-1};
    GLint alpha_mode{-1}, alpha_cutoff{-1};
  };

  bool initialized_{false};
  std::unique_ptr<gl::Program> program_;
  // Depth-only caster program for the shadow pre-pass (position -> light clip). Built
  // beside program_ in initializeGL, reset in releaseGL — per-context like everything.
  std::unique_ptr<gl::Program> depth_program_;
  MaterialUniforms uniforms_;
  std::vector<std::pair<std::string, MeshResource>> meshes_;
  // Scratch buffer reused by drawBatch's opaque/translucent bucketing so steady
  // frames allocate nothing; contents are only valid within one drawBatch call.
  std::vector<ResolvedDraw> resolved_draws_;
  // Per-pass texture cache keyed by (source key, color space) — the key alone is
  // the absolute path for external maps / content hash for embedded ones. Each
  // pass owns its own textures_ with no cross-layer dedup. First textured frame
  // and post-ADS-reparent rebuild synchronously decode and upload on the GL
  // thread; accepted for now, with off-thread decode as a follow-up. Failed loads
  // cache id 0 so the matching u_has_*_tex stays 0 (the slot falls back to its
  // factor). Keys that stop being referenced are not evicted until
  // clearMeshes()/releaseGL(), which is acceptable for static URDFs but a slow
  // leak for streaming mesh swaps.
  std::vector<CachedTexture> textures_;
  // Meshes/textures retired by clearMeshes() but not yet destroyed: clearMeshes()
  // may run with no GL context current, so the GL wrappers wait here until
  // drainRetired() (drawBatch/releaseGL) frees them under the owning context.
  std::vector<CachedTexture> retired_textures_;
  std::vector<std::pair<std::string, MeshResource>> retired_meshes_;
  MeshResource cube_;
  MeshResource cylinder_;
  MeshResource sphere_;
};

}  // namespace pj::scene3d
