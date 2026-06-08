// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// U2 gate: decoupled adaptive near/far + geometric zoom + lifted clamp.

#include <gtest/gtest.h>

#include <cmath>

#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/camera/camera_math.h"

using pj::scene3d::AABB;
using pj::scene3d::adaptiveNearFar;
using pj::scene3d::CameraState;
using pj::scene3d::OrbitCamera;
using pj::scene3d::sceneReach;

namespace {
// A scene a few thousand units across — the original failure case.
constexpr float kSceneHalf = 2500.0f;
const AABB kScene{
    glm::vec3{-kSceneHalf, -kSceneHalf, -kSceneHalf}, glm::vec3{kSceneHalf, kSceneHalf, kSceneHalf}, true};
}  // namespace

TEST(AdaptiveNearFar, InvariantsAcrossRadii) {
  // Focal at the scene centre → reach == diagonal.
  const float reach = sceneReach(kScene, glm::vec3{0.0f});
  ASSERT_GT(reach, 0.0f);

  for (const float working_distance : {0.5f, 2.0f, 50.0f, 5000.0f, 1.0e6f}) {
    float near = 0.0f;
    float far = 0.0f;
    adaptiveNearFar(working_distance, reach, near, far);

    EXPECT_GT(near, 0.0f) << "wd=" << working_distance;
    // The eye-to-focal distance for an orbit camera IS the working distance, so
    // near strictly inside it means close inspection never clips.
    EXPECT_LT(near, working_distance) << "wd=" << working_distance;
    // Far reaches the whole scene.
    EXPECT_GE(far, reach) << "wd=" << working_distance;
    // Depth precision stays bounded.
    EXPECT_LE(far / near, 1.0e5f + 1.0f) << "wd=" << working_distance;
  }
}

TEST(AdaptiveNearFar, UnknownBoundsFallsBackToWorkingDistance) {
  // reach == 0 (no scene bounds): far is driven purely by working distance.
  float near = 0.0f;
  float far = 0.0f;
  adaptiveNearFar(50.0f, 0.0f, near, far);
  EXPECT_FLOAT_EQ(far, 50.0f * 4.0f * 1.5f);  // max(200, 0) * 1.5
  EXPECT_LT(near, 50.0f);
}

TEST(SceneReach, OffsetFocalIncreasesReach) {
  const float centered = sceneReach(kScene, glm::vec3{0.0f});
  const float offset = sceneReach(kScene, glm::vec3{kSceneHalf, kSceneHalf, kSceneHalf});
  EXPECT_GT(offset, centered);                                 // pivot off to one side extends the reach
  EXPECT_FLOAT_EQ(sceneReach(AABB{}, glm::vec3{0.0f}), 0.0f);  // invalid → 0
}

TEST(OrbitZoom, GeometricStep) {
  OrbitCamera cam;  // no scene bounds
  CameraState s;
  s.radius = 100.0f;
  cam.adoptState(s);

  cam.zoom(1.0f);  // one tick in
  EXPECT_NEAR(cam.state().radius, 90.0f, 1e-3f);
  cam.zoom(-1.0f);  // one tick back out
  EXPECT_NEAR(cam.state().radius, 100.0f, 1e-2f);
}

TEST(OrbitZoom, OldThousandUnitClampIsGone) {
  OrbitCamera cam;
  CameraState s;
  s.radius = 5000.0f;  // beyond the retired [0.1, 1000] cap
  cam.adoptState(s);
  cam.zoom(1.0f);
  // Would have been clamped to 1000 before; now it just shrinks geometrically.
  EXPECT_NEAR(cam.state().radius, 4500.0f, 1.0f);
  EXPECT_GT(cam.state().radius, 1000.0f);
}

TEST(OrbitZoom, LargeSceneIsFramable) {
  OrbitCamera cam;
  cam.setSceneBounds(kScene);
  CameraState s;
  s.radius = 2.0f;  // start zoomed in
  cam.adoptState(s);

  const float scene_diag = glm::length(kScene.max - kScene.min);
  for (int i = 0; i < 200; ++i) {
    cam.zoom(-1.0f);  // keep zooming out
  }
  // Can pull back far enough to frame the entire scene (and then some).
  EXPECT_GT(cam.state().radius, scene_diag);
}
