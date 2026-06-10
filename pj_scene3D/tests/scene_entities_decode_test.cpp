// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/scene_entities_decode.h"

#include <gtest/gtest.h>

#include <cmath>
#include <glm/glm.hpp>
#include <numbers>

namespace {

using namespace pj::scene3d;
using PJ::sdk::ArrowPrimitive;
using PJ::sdk::AxesPrimitive;
using PJ::sdk::ColorRGBA;
using PJ::sdk::CubePrimitive;
using PJ::sdk::CylinderPrimitive;
using PJ::sdk::LinePrimitive;
using PJ::sdk::LineType;
using PJ::sdk::Point3;
using PJ::sdk::SceneEntities;
using PJ::sdk::SceneEntity;
using PJ::sdk::SpherePrimitive;
using PJ::sdk::TextPrimitive;
using PJ::sdk::TrianglePrimitive;

SceneEntity entityInFrame(std::string frame) {
  SceneEntity e;
  e.frame_id = std::move(frame);
  return e;
}

TEST(SceneEntitiesDecode, InternsFramesAndIndexesPrimitives) {
  SceneEntities batch;

  SceneEntity a = entityInFrame("map");
  a.cubes.push_back(CubePrimitive{});
  SceneEntity b = entityInFrame("odom");
  b.cubes.push_back(CubePrimitive{});
  SceneEntity c = entityInFrame("map");  // reuses "map"
  c.cubes.push_back(CubePrimitive{});
  batch.entities = {a, b, c};

  const auto d = decodeSceneEntities(batch);

  ASSERT_EQ(d.frames.size(), 2U);  // map, odom — deduped
  EXPECT_EQ(d.frames[0], "map");
  EXPECT_EQ(d.frames[1], "odom");
  ASSERT_EQ(d.cubes.size(), 3U);
  EXPECT_EQ(d.cubes[0].frame_index, 0U);  // map
  EXPECT_EQ(d.cubes[1].frame_index, 1U);  // odom
  EXPECT_EQ(d.cubes[2].frame_index, 0U);  // map again
}

TEST(SceneEntitiesDecode, CubeModelCarriesPositionAndSize) {
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  CubePrimitive cube;
  cube.pose.position = {1.0, 2.0, 3.0};  // identity orientation
  cube.size = {2.0, 4.0, 6.0};
  cube.color = {255, 0, 0, 255};
  e.cubes.push_back(cube);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.cubes.size(), 1U);

  // The local origin maps to the marker position.
  const glm::vec4 origin = d.cubes[0].model * glm::vec4(0, 0, 0, 1);
  EXPECT_NEAR(origin.x, 1.0F, 1e-4F);
  EXPECT_NEAR(origin.y, 2.0F, 1e-4F);
  EXPECT_NEAR(origin.z, 3.0F, 1e-4F);

  // A unit cube's +x face (local 0.5) lands at position + size.x/2.
  const glm::vec4 face_x = d.cubes[0].model * glm::vec4(0.5F, 0, 0, 1);
  EXPECT_NEAR(face_x.x, 1.0F + 1.0F, 1e-4F);  // 1 + (2/2)

  EXPECT_NEAR(d.cubes[0].color.r, 1.0F, 1e-3F);
  EXPECT_NEAR(d.cubes[0].color.a, 1.0F, 1e-3F);
}

TEST(SceneEntitiesDecode, CubeOrientationRotatesLocalAxes) {
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  CubePrimitive cube;
  cube.size = {1.0, 1.0, 1.0};
  // 90 deg about +z: (x,y,z,w) = (0, 0, sin45, cos45). Local +x -> world +y.
  const double s = std::sin(std::numbers::pi / 4.0);
  cube.pose.orientation = {0.0, 0.0, s, s};
  e.cubes.push_back(cube);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.cubes.size(), 1U);

  const glm::vec4 local_x = d.cubes[0].model * glm::vec4(1, 0, 0, 1);
  EXPECT_NEAR(local_x.x, 0.0F, 1e-4F);
  EXPECT_NEAR(local_x.y, 1.0F, 1e-4F);
  EXPECT_NEAR(local_x.z, 0.0F, 1e-4F);
}

TEST(SceneEntitiesDecode, SphereNonUniformSizeIsEllipsoid) {
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  SpherePrimitive sphere;
  sphere.size = {2.0, 4.0, 6.0};  // diameters
  e.spheres.push_back(sphere);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.spheres.size(), 1U);

  const glm::vec4 px = d.spheres[0].model * glm::vec4(0.5F, 0, 0, 1);  // unit-sphere +x
  const glm::vec4 py = d.spheres[0].model * glm::vec4(0, 0.5F, 0, 1);
  EXPECT_NEAR(px.x, 1.0F, 1e-4F);  // radius 1 along x
  EXPECT_NEAR(py.y, 2.0F, 1e-4F);  // radius 2 along y
}

TEST(SceneEntitiesDecode, LineStripExpandsToSegmentPairs) {
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  LinePrimitive line;
  line.type = LineType::kLineStrip;
  line.points = {Point3{0, 0, 0}, Point3{1, 0, 0}, Point3{1, 1, 0}};  // 3 points
  e.lines.push_back(line);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.lines.size(), 1U);
  // A 3-point strip = 2 segments = 4 GL_LINES vertices.
  EXPECT_EQ(d.lines[0].vertices.size(), 4U);
}

TEST(SceneEntitiesDecode, LineLoopClosesBackToStart) {
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  LinePrimitive line;
  line.type = LineType::kLineLoop;
  line.points = {Point3{0, 0, 0}, Point3{1, 0, 0}, Point3{1, 1, 0}};
  e.lines.push_back(line);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.lines.size(), 1U);
  // 3-point loop = 3 segments = 6 vertices (strip 2 + closing edge).
  EXPECT_EQ(d.lines[0].vertices.size(), 6U);
}

TEST(SceneEntitiesDecode, LineOutOfRangeIndexIsSkipped) {
  // An index buffer entry past points.size() is untrusted message data: it must
  // not read out of bounds — the offending segment is skipped, not dereferenced.
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  LinePrimitive line;
  line.type = LineType::kLineList;
  line.points = {Point3{0, 0, 0}, Point3{1, 0, 0}};
  line.indices = {0, 1, 0, 99};  // 1st pair (0,1) valid; 2nd pair (0,99) out of range
  e.lines.push_back(line);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.lines.size(), 1U);
  EXPECT_EQ(d.lines[0].vertices.size(), 2U);  // only the valid segment survives
}

TEST(SceneEntitiesDecode, TriangleOutOfRangeIndexIsSkipped) {
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  TrianglePrimitive tri;
  tri.points = {Point3{0, 0, 0}, Point3{1, 0, 0}, Point3{0, 1, 0}};
  tri.indices = {0, 1, 2, 0, 1, 99};  // 1st triangle valid; 2nd references vertex 99
  e.triangles.push_back(tri);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.triangles.size(), 1U);
  EXPECT_EQ(d.triangles[0].vertices.size(), 3U);  // only the valid triangle survives
}

TEST(SceneEntitiesDecode, CylinderCarriesTaperAndScale) {
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  CylinderPrimitive cyl;
  cyl.size = {2.0, 2.0, 4.0};
  cyl.bottom_scale = 1.0;
  cyl.top_scale = 0.0;  // cone
  e.cylinders.push_back(cyl);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.cylinders.size(), 1U);
  EXPECT_NEAR(d.cylinders[0].bottom_scale, 1.0F, 1e-6F);
  EXPECT_NEAR(d.cylinders[0].top_scale, 0.0F, 1e-6F);
  const glm::vec4 px = d.cylinders[0].model * glm::vec4(0.5F, 0, 0, 1);  // unit radius -> size.x/2
  EXPECT_NEAR(px.x, 1.0F, 1e-4F);
}

TEST(SceneEntitiesDecode, ArrowCarriesDimensions) {
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  ArrowPrimitive arrow;
  arrow.pose.position = {5.0, 0.0, 0.0};
  arrow.shaft_length = 1.0;
  arrow.shaft_diameter = 0.1;
  arrow.head_length = 0.3;
  arrow.head_diameter = 0.2;
  e.arrows.push_back(arrow);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.arrows.size(), 1U);
  EXPECT_NEAR(d.arrows[0].shaft_length, 1.0F, 1e-6F);
  EXPECT_NEAR(d.arrows[0].head_diameter, 0.2F, 1e-6F);
  const glm::vec4 origin = d.arrows[0].model * glm::vec4(0, 0, 0, 1);
  EXPECT_NEAR(origin.x, 5.0F, 1e-4F);  // tail at the pose position
}

TEST(SceneEntitiesDecode, AxesCarriesLength) {
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  AxesPrimitive ax;
  ax.length = 0.5;
  ax.thickness = 0.02;
  e.axes.push_back(ax);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.axes.size(), 1U);
  EXPECT_NEAR(d.axes[0].length, 0.5F, 1e-6F);
}

TEST(SceneEntitiesDecode, TriangleFlatNormalAndVertexCount) {
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  TrianglePrimitive tri;
  tri.points = {Point3{0, 0, 0}, Point3{1, 0, 0}, Point3{0, 1, 0}};  // CCW in xy-plane
  e.triangles.push_back(tri);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.triangles.size(), 1U);
  EXPECT_EQ(d.triangles[0].vertices.size(), 3U);
  EXPECT_EQ(d.triangles[0].normals.size(), 3U);
  // CCW xy triangle faces +z.
  EXPECT_NEAR(d.triangles[0].normals[0].x, 0.0F, 1e-4F);
  EXPECT_NEAR(d.triangles[0].normals[0].y, 0.0F, 1e-4F);
  EXPECT_NEAR(d.triangles[0].normals[0].z, 1.0F, 1e-4F);
}

TEST(SceneEntitiesDecode, DegenerateTriangleSkippedNoNaNNormal) {
  // Coincident points (valid producer input, e.g. degenerate padding triangles)
  // give a zero cross product; normalizing it yields NaN, which would corrupt the
  // whole triple in the shader. The decoder must skip the degenerate triangle.
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  TrianglePrimitive tri;
  tri.points = {Point3{1, 1, 1}, Point3{1, 1, 1}, Point3{1, 1, 1}};  // all coincident
  e.triangles.push_back(tri);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  EXPECT_TRUE(d.triangles.empty());  // only degenerate geometry -> nothing emitted
}

TEST(SceneEntitiesDecode, DegenerateTriangleSkippedValidSurvivesWithFiniteNormals) {
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  TrianglePrimitive tri;
  tri.points = {Point3{0, 0, 0}, Point3{0, 0, 0}, Point3{0, 0, 0},   // degenerate, skipped
                Point3{0, 0, 0}, Point3{1, 0, 0}, Point3{0, 1, 0}};  // valid, survives
  e.triangles.push_back(tri);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.triangles.size(), 1U);
  ASSERT_EQ(d.triangles[0].vertices.size(), 3U);  // only the valid triangle
  for (const auto& n : d.triangles[0].normals) {
    EXPECT_TRUE(std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z));
  }
}

TEST(SceneEntitiesDecode, LineStripOutOfRangeIndexIsSkipped) {
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  LinePrimitive line;
  line.type = LineType::kLineStrip;
  line.points = {Point3{0, 0, 0}, Point3{1, 0, 0}, Point3{1, 1, 0}};
  line.indices = {0, 1, 99};  // segment (0,1) valid; segment (1,99) out of range
  e.lines.push_back(line);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.lines.size(), 1U);
  EXPECT_EQ(d.lines[0].vertices.size(), 2U);  // only the valid segment survives
}

TEST(SceneEntitiesDecode, LineLoopOutOfRangeClosingEdgeIsSkipped) {
  // indices {0,1,99}: strip segment (0,1) valid, (1,99) skipped; the closing edge
  // (99,0) is also out of range and skipped. One segment survives.
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  LinePrimitive line;
  line.type = LineType::kLineLoop;
  line.points = {Point3{0, 0, 0}, Point3{1, 0, 0}, Point3{1, 1, 0}};
  line.indices = {0, 1, 99};
  e.lines.push_back(line);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.lines.size(), 1U);
  EXPECT_EQ(d.lines[0].vertices.size(), 2U);
}

TEST(SceneEntitiesDecode, LinePerVertexColorStaysParallelAfterSkip) {
  // When an OOB segment is skipped, the render pass relies on colors being exactly
  // parallel to vertices (per-vertex color = colors.size() == points.size()).
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  LinePrimitive line;
  line.type = LineType::kLineList;
  line.points = {Point3{0, 0, 0}, Point3{1, 0, 0}};
  line.colors = {ColorRGBA{255, 0, 0, 255}, ColorRGBA{0, 255, 0, 255}};
  line.indices = {0, 1, 0, 99};  // 2nd pair out of range -> skipped
  e.lines.push_back(line);
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.lines.size(), 1U);
  EXPECT_EQ(d.lines[0].vertices.size(), 2U);
  EXPECT_EQ(d.lines[0].colors.size(), d.lines[0].vertices.size());
}

TEST(SceneEntitiesDecode, TextCarriedAndEmptySkipped) {
  SceneEntities batch;
  SceneEntity e = entityInFrame("map");
  TextPrimitive a;
  a.text = "hi";
  a.font_size = 14.0;
  TextPrimitive empty;  // empty text -> skipped
  e.texts = {a, empty};
  batch.entities = {e};

  const auto d = decodeSceneEntities(batch);
  ASSERT_EQ(d.texts.size(), 1U);
  EXPECT_EQ(d.texts[0].text, "hi");
}

}  // namespace
