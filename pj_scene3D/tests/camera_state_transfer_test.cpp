// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// U4 gate: per-model reset defaults + state round-trips across model switches.

#include <gtest/gtest.h>

#include "pj_scene3d_core/camera/camera.h"

using pj::scene3d::CameraState;
using pj::scene3d::cameraStateFromJson;
using pj::scene3d::cameraStateToJson;
using pj::scene3d::FlyCamera;
using pj::scene3d::OrbitCamera;
using pj::scene3d::TopDownOrthoCamera;

TEST(CameraReset, OrbitDefaults) {
  OrbitCamera cam;
  CameraState s;
  s.focal = glm::vec3{9.0f, -4.0f, 2.0f};
  s.radius = 123.0f;
  s.azimuth = 1.2f;
  cam.adoptState(s);
  cam.reset();

  const CameraState d = cam.state();
  EXPECT_EQ(d.focal, glm::vec3{0.0f});
  EXPECT_FLOAT_EQ(d.radius, 5.0f);
  EXPECT_FLOAT_EQ(d.azimuth, 0.0f);
  EXPECT_FLOAT_EQ(d.elevation, glm::radians(30.0f));
  EXPECT_TRUE(d.perspective);
}

TEST(CameraReset, TopDownOrthoDefaults) {
  TopDownOrthoCamera cam;
  cam.rotate(300.0f, 0.0f);  // change heading
  cam.pan(50.0f, 50.0f);     // move focal
  cam.zoom(3.0f);            // change scale
  cam.reset();

  const CameraState d = cam.state();
  EXPECT_EQ(d.focal, glm::vec3{0.0f});
  EXPECT_FLOAT_EQ(d.azimuth, 0.0f);
  EXPECT_FALSE(d.perspective);
  EXPECT_GT(d.ortho_scale, 0.0f);
}

TEST(CameraReset, FlyDefaultsLookAtOrigin) {
  FlyCamera cam;
  cam.rotate(200.0f, 100.0f);
  cam.pan(40.0f, 40.0f);
  cam.reset();

  EXPECT_FLOAT_EQ(cam.position().x, 5.0f);
  EXPECT_FLOAT_EQ(cam.position().y, 5.0f);
  EXPECT_FLOAT_EQ(cam.position().z, 10.0f);
  // The synthesized pivot is in front of the eye, toward the origin it looks at.
  const glm::vec3 to_focal = glm::normalize(cam.state().focal - cam.position());
  const glm::vec3 to_origin = glm::normalize(glm::vec3{0.0f} - cam.position());
  EXPECT_NEAR(glm::dot(to_focal, to_origin), 1.0f, 1e-4f);
}

TEST(CameraStateTransfer, OrbitToTopDownToOrbitRoundTrip) {
  CameraState s;
  s.focal = glm::vec3{3.0f, -2.0f, 1.0f};
  s.radius = 42.0f;
  s.azimuth = 0.7f;
  s.elevation = 0.4f;
  s.perspective = true;

  OrbitCamera orbit;
  orbit.adoptState(s);

  TopDownOrthoCamera top_down;
  top_down.adoptState(orbit.state());
  EXPECT_FALSE(top_down.state().perspective);
  EXPECT_FLOAT_EQ(top_down.state().ortho_scale, 42.0f);  // seeded from radius

  OrbitCamera orbit2;
  orbit2.adoptState(top_down.state());
  const CameraState r = orbit2.state();
  EXPECT_TRUE(r.perspective);
  EXPECT_NEAR(r.focal.x, s.focal.x, 1e-4f);
  EXPECT_NEAR(r.focal.y, s.focal.y, 1e-4f);
  EXPECT_NEAR(r.focal.z, s.focal.z, 1e-4f);
  EXPECT_NEAR(r.azimuth, s.azimuth, 1e-4f);
  EXPECT_NEAR(r.radius, s.radius, 1e-4f);  // radius → ortho_scale → radius
}

TEST(CameraStateSerialize, JsonRoundTrip) {
  CameraState s;
  s.focal = glm::vec3{3.5f, -2.25f, 1.0f};
  s.radius = 42.0f;
  s.azimuth = 0.7f;
  s.elevation = 0.4f;
  s.fov_y = glm::radians(50.0f);
  s.ortho_scale = 12.5f;
  s.perspective = false;

  const CameraState r = cameraStateFromJson(cameraStateToJson(s));
  EXPECT_NEAR(r.focal.x, s.focal.x, 1e-5f);
  EXPECT_NEAR(r.focal.y, s.focal.y, 1e-5f);
  EXPECT_NEAR(r.focal.z, s.focal.z, 1e-5f);
  EXPECT_NEAR(r.radius, s.radius, 1e-5f);
  EXPECT_NEAR(r.azimuth, s.azimuth, 1e-5f);
  EXPECT_NEAR(r.elevation, s.elevation, 1e-5f);
  EXPECT_NEAR(r.fov_y, s.fov_y, 1e-5f);
  EXPECT_NEAR(r.ortho_scale, s.ortho_scale, 1e-5f);
  EXPECT_EQ(r.perspective, s.perspective);
}

TEST(CameraStateSerialize, MissingFieldsTakeFallback) {
  CameraState fallback;
  fallback.radius = 99.0f;
  fallback.azimuth = 0.0f;
  const CameraState r = cameraStateFromJson(R"({"azimuth": 1.5})", fallback);
  EXPECT_FLOAT_EQ(r.azimuth, 1.5f);  // present → taken
  EXPECT_FLOAT_EQ(r.radius, 99.0f);  // missing → fallback
}

TEST(CameraStateSerialize, MalformedReturnsFallback) {
  CameraState fallback;
  fallback.radius = 7.0f;
  EXPECT_FLOAT_EQ(cameraStateFromJson("not json at all", fallback).radius, 7.0f);
  EXPECT_FLOAT_EQ(cameraStateFromJson("", fallback).radius, 7.0f);
}

TEST(CameraStateSerialize, WrongTypedFieldReturnsFallback) {
  CameraState fallback;
  fallback.radius = 7.0f;
  // A present-but-wrong-typed field makes nlohmann's value() throw json::type_error;
  // the loader must catch it and fall back rather than crash layout restore.
  CameraState recovered = fallback;
  EXPECT_NO_THROW(recovered = cameraStateFromJson(R"({"radius": "fast"})", fallback));
  EXPECT_FLOAT_EQ(recovered.radius, 7.0f);
}

TEST(CameraStateSerialize, OutOfRangeValuesAreSanitized) {
  // A corrupt/hand-edited layout with non-positive or non-finite fields must never
  // reach glm::perspective/lookAt as-is; the loader clamps them to a usable range.
  const CameraState recovered = cameraStateFromJson(R"({"radius": 0, "ortho_scale": -5, "fov_y": 0})");
  EXPECT_GT(recovered.radius, 0.0f);
  EXPECT_GT(recovered.ortho_scale, 0.0f);
  EXPECT_GT(recovered.fov_y, 0.0f);
}
