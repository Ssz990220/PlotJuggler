// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// U3 ACCEPTANCE GATE: cursor-anchored zoom must keep the world point under the
// cursor pixel-locked. Pick a pixel, unproject it to the ground point P under the
// cursor, zoomToCursor at that pixel, then re-project P and assert its NDC is
// unchanged (within 1e-3). Also assert no NaN near the nadir singularity.

#include <gtest/gtest.h>

#include <cmath>

#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/camera/camera_math.h"

using pj::scene3d::CameraState;
using pj::scene3d::OrbitCamera;
using pj::scene3d::Ray;
using pj::scene3d::rayPlane;
using pj::scene3d::TopDownOrthoCamera;
using pj::scene3d::unprojectRay;

namespace {
constexpr int kW = 800;
constexpr int kH = 600;
constexpr float kAspect = static_cast<float>(kW) / static_cast<float>(kH);

glm::vec2 pixelToNdc(glm::vec2 px) {
  return glm::vec2{2.0f * px.x / static_cast<float>(kW) - 1.0f, 1.0f - 2.0f * px.y / static_cast<float>(kH)};
}

// NDC.xy of a world point under the given view/proj (perspective divide).
glm::vec2 projectToNdc(const glm::vec3& p, const glm::mat4& view, const glm::mat4& proj) {
  const glm::vec4 clip = proj * view * glm::vec4(p, 1.0f);
  return glm::vec2{clip.x / clip.w, clip.y / clip.w};
}

// World point on the ground (z=0) under `pixel`, for the camera's current pose.
bool groundPointUnderPixel(const OrbitCamera& cam, glm::vec2 pixel, glm::vec3& out) {
  const glm::mat4 view = cam.viewMatrix();
  const glm::mat4 proj = cam.projMatrix(kAspect);
  const Ray ray = unprojectRay(pixel, kW, kH, view, proj);
  float t = 0.0f;
  if (!rayPlane(ray.origin, ray.dir, glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, t) || t <= 0.0f) {
    return false;
  }
  out = ray.origin + t * ray.dir;
  return true;
}

OrbitCamera makeOffAxisCamera() {
  OrbitCamera cam;
  CameraState s;
  s.focal = glm::vec3{0.0f, 0.0f, 0.0f};
  s.radius = 10.0f;
  s.azimuth = 0.6f;
  s.elevation = 0.5f;  // eye above ground, looking down toward origin
  cam.adoptState(s);
  return cam;
}
}  // namespace

TEST(ZoomToCursor, CursorPointStaysPixelLockedZoomingIn) {
  for (const glm::vec2 pixel : {glm::vec2{520.0f, 300.0f}, glm::vec2{300.0f, 420.0f}, glm::vec2{640.0f, 180.0f}}) {
    OrbitCamera cam = makeOffAxisCamera();
    glm::vec3 p{};
    ASSERT_TRUE(groundPointUnderPixel(cam, pixel, p)) << "pixel sees the ground";
    const glm::vec2 ndc_before = pixelToNdc(pixel);

    cam.zoomToCursor(2.0f, pixel, kW, kH);  // zoom in two ticks

    const glm::vec2 ndc_after = projectToNdc(p, cam.viewMatrix(), cam.projMatrix(kAspect));
    EXPECT_NEAR(ndc_after.x, ndc_before.x, 1e-3f) << "pixel=(" << pixel.x << "," << pixel.y << ")";
    EXPECT_NEAR(ndc_after.y, ndc_before.y, 1e-3f) << "pixel=(" << pixel.x << "," << pixel.y << ")";
  }
}

TEST(ZoomToCursor, CursorPointStaysPixelLockedZoomingOut) {
  const glm::vec2 pixel{560.0f, 260.0f};
  OrbitCamera cam = makeOffAxisCamera();
  glm::vec3 p{};
  ASSERT_TRUE(groundPointUnderPixel(cam, pixel, p));
  const glm::vec2 ndc_before = pixelToNdc(pixel);

  cam.zoomToCursor(-3.0f, pixel, kW, kH);  // zoom out three ticks

  const glm::vec2 ndc_after = projectToNdc(p, cam.viewMatrix(), cam.projMatrix(kAspect));
  EXPECT_NEAR(ndc_after.x, ndc_before.x, 1e-3f);
  EXPECT_NEAR(ndc_after.y, ndc_before.y, 1e-3f);
}

TEST(ZoomToCursor, ZoomingInActuallyShrinksRadius) {
  const glm::vec2 pixel{520.0f, 300.0f};
  OrbitCamera cam = makeOffAxisCamera();
  const float before = cam.state().radius;
  cam.zoomToCursor(1.0f, pixel, kW, kH);
  EXPECT_LT(cam.state().radius, before);  // it's a real zoom, not just a pan
}

TEST(ZoomToCursor, NoNaNNearNadir) {
  OrbitCamera cam;
  CameraState s;
  s.focal = glm::vec3{0.0f};
  s.radius = 10.0f;
  s.azimuth = 0.0f;
  s.elevation = glm::radians(89.0f);  // looking almost straight down
  cam.adoptState(s);

  cam.zoomToCursor(1.0f, glm::vec2{400.0f, 300.0f}, kW, kH);  // center-ish, near nadir

  const CameraState r = cam.state();
  EXPECT_TRUE(std::isfinite(r.focal.x) && std::isfinite(r.focal.y) && std::isfinite(r.focal.z));
  EXPECT_TRUE(std::isfinite(r.radius));
  EXPECT_TRUE(std::isfinite(r.azimuth));
  EXPECT_TRUE(std::isfinite(r.elevation));
  EXPECT_GT(r.radius, 0.0f);
}

TEST(ZoomToCursorOrtho, CursorPointStaysPixelLocked) {
  // The ortho top-down branch keeps the ground point under the cursor fixed by
  // shifting the focal by the unproject delta. Exact and singularity-free.
  TopDownOrthoCamera cam;
  CameraState s = cam.state();
  s.focal = glm::vec3{2.0f, -1.0f, 0.0f};
  s.ortho_scale = 10.0f;
  s.azimuth = 0.3f;
  s.perspective = false;
  cam.adoptState(s);

  const glm::vec2 pixel{560.0f, 260.0f};
  const Ray ray = unprojectRay(pixel, kW, kH, cam.viewMatrix(), cam.projMatrix(kAspect));
  float t = 0.0f;
  ASSERT_TRUE(rayPlane(ray.origin, ray.dir, glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, t));
  const glm::vec3 p = ray.origin + t * ray.dir;
  const glm::vec2 ndc_before = pixelToNdc(pixel);

  cam.zoomToCursor(2.0f, pixel, kW, kH);

  const glm::vec2 ndc_after = projectToNdc(p, cam.viewMatrix(), cam.projMatrix(kAspect));
  EXPECT_NEAR(ndc_after.x, ndc_before.x, 1e-3f);
  EXPECT_NEAR(ndc_after.y, ndc_before.y, 1e-3f);
}

TEST(ZoomToCursor, UpwardSkyPixelStaysFinite) {
  // A pixel toward the sky may miss the ground plane; the focal-plane fallback
  // (or, failing that, center zoom) keeps the result finite and shrinking — never
  // a NaN or runaway.
  OrbitCamera cam;
  CameraState s;
  s.focal = glm::vec3{0.0f};
  s.radius = 10.0f;
  s.azimuth = 0.0f;
  s.elevation = -0.5f;  // eye below the focal, looking up
  cam.adoptState(s);
  const float before = cam.state().radius;

  cam.zoomToCursor(1.0f, glm::vec2{400.0f, 50.0f}, kW, kH);  // a pixel toward the sky

  EXPECT_TRUE(std::isfinite(cam.state().radius));
  EXPECT_LE(cam.state().radius, before);  // a zoom-in shrinks (or holds) the radius, no blow-up
}
