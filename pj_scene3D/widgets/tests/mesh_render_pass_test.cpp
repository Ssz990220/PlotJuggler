// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// GL-free unit tests for MeshRenderPass policy seams: the material-slot ->
// color-space mapping, the (key, color space) texture-cache lookup, and the
// opaque/translucent draw-bucketing rule. Anything that needs a live GL context
// (upload, draw) is out of scope here.

#include "pj_scene3d_widgets/passes/mesh_render_pass.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace pj::scene3d {
namespace {

// Regression guard for the renderer-owned upload policy: base color and
// emissive are color data (sRGB, hardware-linearized on sample); the
// metallic-roughness/normal/occlusion data maps must stay linear so their
// channels are read verbatim.
TEST(MeshRenderPassTest, SlotColorSpaceMapping) {
  EXPECT_EQ(textureColorSpaceForSlot(MaterialTextureSlot::kBaseColor), TextureColorSpace::kSrgb);
  EXPECT_EQ(textureColorSpaceForSlot(MaterialTextureSlot::kEmissive), TextureColorSpace::kSrgb);
  EXPECT_EQ(textureColorSpaceForSlot(MaterialTextureSlot::kMetallicRoughness), TextureColorSpace::kLinear);
  EXPECT_EQ(textureColorSpaceForSlot(MaterialTextureSlot::kNormal), TextureColorSpace::kLinear);
  EXPECT_EQ(textureColorSpaceForSlot(MaterialTextureSlot::kOcclusion), TextureColorSpace::kLinear);
}

// One image legally serving a color slot in one material and a data slot in
// another must resolve to two distinct cache entries, not collide on the key.
TEST(MeshRenderPassTest, TextureCacheIsKeyedByKeyAndColorSpace) {
  std::vector<MeshRenderPass::CachedTexture> cache;
  cache.push_back(MeshRenderPass::CachedTexture{"emb:deadbeef", TextureColorSpace::kSrgb, gl::Texture2D{}});
  cache.push_back(MeshRenderPass::CachedTexture{"emb:deadbeef", TextureColorSpace::kLinear, gl::Texture2D{}});

  const auto* srgb_entry = MeshRenderPass::findCachedTexture(cache, "emb:deadbeef", TextureColorSpace::kSrgb);
  const auto* linear_entry = MeshRenderPass::findCachedTexture(cache, "emb:deadbeef", TextureColorSpace::kLinear);
  ASSERT_NE(srgb_entry, nullptr);
  ASSERT_NE(linear_entry, nullptr);
  EXPECT_NE(srgb_entry, linear_entry) << "same key in two color spaces must not dedup to one entry";
  EXPECT_EQ(srgb_entry, cache.data());
  EXPECT_EQ(linear_entry, cache.data() + 1);
}

TEST(MeshRenderPassTest, TextureCacheLookupMisses) {
  std::vector<MeshRenderPass::CachedTexture> cache;
  cache.push_back(MeshRenderPass::CachedTexture{"emb:deadbeef", TextureColorSpace::kSrgb, gl::Texture2D{}});

  EXPECT_EQ(MeshRenderPass::findCachedTexture(cache, "emb:deadbeef", TextureColorSpace::kLinear), nullptr)
      << "a cached sRGB upload must not satisfy a linear request";
  EXPECT_EQ(MeshRenderPass::findCachedTexture(cache, "emb:other", TextureColorSpace::kSrgb), nullptr);
  EXPECT_EQ(MeshRenderPass::findCachedTexture({}, "emb:deadbeef", TextureColorSpace::kSrgb), nullptr);
}

// --- Opaque/translucent draw bucketing ---------------------------------------

// A kBlend material anywhere in the mesh marks it blended; kOpaque/kMask do not,
// and a null material (renderer fallback guard) counts as opaque.
TEST(MeshRenderPassTest, MeshHasBlendedMaterialScansSubmeshes) {
  MeshData mesh;
  const auto opaque = std::make_shared<Material>();
  const auto mask = std::make_shared<Material>();
  mask->alpha_mode = AlphaMode::kMask;
  mesh.submeshes.push_back(SubMesh{0, 3, opaque});
  mesh.submeshes.push_back(SubMesh{3, 3, mask});
  mesh.submeshes.push_back(SubMesh{6, 3, nullptr});
  EXPECT_FALSE(MeshRenderPass::meshHasBlendedMaterial(mesh));

  const auto blend = std::make_shared<Material>();
  blend->alpha_mode = AlphaMode::kBlend;
  mesh.submeshes.push_back(SubMesh{9, 3, blend});
  EXPECT_TRUE(MeshRenderPass::meshHasBlendedMaterial(mesh));
}

// The rule that splits drawBatch into opaque-first/blended-second: layer opacity
// < 1, an override tint with alpha < 1, or a kBlend material each routes a draw
// to the translucent bucket. A regression to one undifferentiated pass (the old
// glDisable(GL_BLEND)-for-kBlend path) trips these expectations.
TEST(MeshRenderPassTest, DrawNeedsVisualBlendingRouting) {
  MeshRenderPass::DrawCall draw;  // defaults: use_vertex_color = true, opaque color

  EXPECT_FALSE(MeshRenderPass::drawNeedsVisualBlending(draw, false, 1.0f));
  // Layer opacity below 1 forces blending regardless of material.
  EXPECT_TRUE(MeshRenderPass::drawNeedsVisualBlending(draw, false, 0.5f));
  // A glTF kBlend material forces blending even at full opacity.
  EXPECT_TRUE(MeshRenderPass::drawNeedsVisualBlending(draw, true, 1.0f));

  // The override tint's alpha only matters when the draw uses the override
  // (use_vertex_color == false); otherwise the shader ignores the tint.
  draw.color.a = 0.5f;
  EXPECT_FALSE(MeshRenderPass::drawNeedsVisualBlending(draw, false, 1.0f));
  draw.use_vertex_color = false;
  EXPECT_TRUE(MeshRenderPass::drawNeedsVisualBlending(draw, false, 1.0f));
  draw.color.a = 1.0f;
  EXPECT_FALSE(MeshRenderPass::drawNeedsVisualBlending(draw, false, 1.0f));
}

// --- Per-view shading plumbing (WP4: retired the process-global singleton) ----

// MeshShadingParams now travels per-frame inside ViewParams::shading (drawOne and
// RobotModelLayer::render read it from there), so two views carry independent look
// state. A regression to a shared process-global would make these aliases.
TEST(MeshRenderPassTest, ViewParamsCarryIndependentShadingPerView) {
  ViewParams view_a{};
  ViewParams view_b{};

  // Default-initialized to the same look-dev defaults, but distinct storage.
  EXPECT_EQ(view_a.shading.mesh_opacity, MeshShadingParams{}.mesh_opacity);
  EXPECT_NE(&view_a.shading, &view_b.shading) << "shading must be per-ViewParams, not a shared global";

  // Mutating one view's copy (the Scene3DConfigPanel path) must not touch the other.
  view_a.shading.mesh_opacity = 0.25f;
  view_a.shading.collisions_visible = false;
  EXPECT_EQ(view_b.shading.mesh_opacity, MeshShadingParams{}.mesh_opacity);
  EXPECT_TRUE(view_b.shading.collisions_visible);
}

}  // namespace
}  // namespace pj::scene3d
