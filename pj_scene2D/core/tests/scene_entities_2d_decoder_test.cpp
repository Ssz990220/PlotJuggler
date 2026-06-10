// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <any>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "pj_base/builtin/image_annotations.hpp"
#include "pj_base/builtin/scene_entities.hpp"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_scene2d_core/scene_decoder.h"

namespace PJ {
namespace {

sdk::Pose poseAt(double x, double y, double z = 0.0) {
  sdk::Pose pose;
  pose.position = {.x = x, .y = y, .z = z};
  return pose;
}

TEST(SceneEntities2DDecoderTest, FactoryCreatesDecoderForSchema) {
  auto decoder = makeSceneDecoder(kSchemaSceneEntities);
  EXPECT_NE(decoder, nullptr);
}

TEST(SceneEntities2DDecoderTest, ProjectsRenderablePrimitivesToImageAnnotations) {
  sdk::SceneEntities entities;
  sdk::SceneEntity entity;
  entity.timestamp = 42;
  entity.frame_id = "camera";
  entity.id = "overlay";

  sdk::LinePrimitive line;
  line.type = sdk::LineType::kLineStrip;
  line.pose = poseAt(10.0, 20.0);
  line.thickness = 3.0;
  line.points = {{0.0, 0.0, 0.0}, {5.0, 0.0, 0.0}, {5.0, 5.0, 0.0}};
  line.color = {255, 0, 0, 255};
  entity.lines.push_back(std::move(line));

  sdk::TextPrimitive text;
  text.pose = poseAt(30.0, 40.0);
  text.font_size = 12.0;
  text.color = {0, 255, 0, 255};
  text.text = "label";
  entity.texts.push_back(std::move(text));

  sdk::SpherePrimitive sphere;
  sphere.pose = poseAt(50.0, 60.0);
  sphere.size = {.x = 8.0, .y = 6.0, .z = 2.0};
  sphere.color = {0, 0, 255, 255};
  entity.spheres.push_back(std::move(sphere));

  entities.entities.push_back(std::move(entity));
  const auto bytes = serializeSceneEntities(entities);
  ASSERT_FALSE(bytes.empty());

  SceneEntities2DDecoder decoder;
  auto frame = decoder.decode(bytes.data(), bytes.size());
  ASSERT_TRUE(frame.has_value()) << frame.error();

  EXPECT_EQ(frame->timestamp, 42);
  ASSERT_EQ(frame->annotations.size(), 1u);
  const auto& annotation = frame->annotations.front();
  EXPECT_EQ(annotation.timestamp, 42);

  ASSERT_EQ(annotation.points.size(), 1u);
  const auto& projected_line = annotation.points.front();
  EXPECT_EQ(projected_line.topology, AnnotationTopology::kLineStrip);
  EXPECT_EQ(projected_line.thickness, 3.0);
  ASSERT_EQ(projected_line.points.size(), 3u);
  EXPECT_DOUBLE_EQ(projected_line.points[0].x, 10.0);
  EXPECT_DOUBLE_EQ(projected_line.points[0].y, 20.0);
  EXPECT_DOUBLE_EQ(projected_line.points[2].x, 15.0);
  EXPECT_DOUBLE_EQ(projected_line.points[2].y, 25.0);

  ASSERT_EQ(annotation.texts.size(), 1u);
  EXPECT_EQ(annotation.texts.front().text, "label");
  EXPECT_DOUBLE_EQ(annotation.texts.front().position.x, 30.0);
  EXPECT_DOUBLE_EQ(annotation.texts.front().position.y, 40.0);

  ASSERT_EQ(annotation.circles.size(), 1u);
  EXPECT_DOUBLE_EQ(annotation.circles.front().center.x, 50.0);
  EXPECT_DOUBLE_EQ(annotation.circles.front().center.y, 60.0);
  EXPECT_DOUBLE_EQ(annotation.circles.front().radius, 4.0);
}

TEST(SceneEntities2DDecoderTest, ObjectRouteMatchesByteRoute) {
  // A-2: parser-backed topics decode the canonical object directly instead of
  // re-serializing it to bytes and re-parsing. Both routes must agree exactly.
  sdk::SceneEntities entities;
  sdk::SceneEntity entity;
  entity.timestamp = 7;
  sdk::LinePrimitive line;
  line.type = sdk::LineType::kLineStrip;
  line.pose = poseAt(1.0, 2.0);
  line.thickness = 2.0;
  line.points = {{0.0, 0.0, 0.0}, {4.0, 0.0, 0.0}};
  line.color = {10, 20, 30, 255};
  entity.lines.push_back(std::move(line));
  entities.entities.push_back(std::move(entity));

  SceneEntities2DDecoder decoder;
  const auto bytes = serializeSceneEntities(entities);
  auto from_bytes = decoder.decode(bytes.data(), bytes.size());
  ASSERT_TRUE(from_bytes.has_value()) << from_bytes.error();

  const sdk::BuiltinObject object = entities;  // BuiltinObject == std::any
  auto from_object = decoder.decode(object);
  ASSERT_TRUE(from_object.has_value()) << from_object.error();

  EXPECT_EQ(from_object->timestamp, from_bytes->timestamp);
  ASSERT_EQ(from_object->annotations.size(), 1u);
  ASSERT_EQ(from_bytes->annotations.size(), 1u);
  const auto& obj_pts = from_object->annotations.front().points;
  const auto& byte_pts = from_bytes->annotations.front().points;
  ASSERT_EQ(obj_pts.size(), byte_pts.size());
  ASSERT_EQ(obj_pts.size(), 1u);
  EXPECT_EQ(obj_pts.front().topology, byte_pts.front().topology);
  ASSERT_EQ(obj_pts.front().points.size(), byte_pts.front().points.size());
  EXPECT_DOUBLE_EQ(obj_pts.front().points[0].x, byte_pts.front().points[0].x);
  EXPECT_DOUBLE_EQ(obj_pts.front().points[1].x, byte_pts.front().points[1].x);
}

TEST(SceneEntities2DDecoderTest, ObjectRouteRejectsWrongType) {
  // The object route any_casts: a BuiltinObject of the wrong kind must error,
  // not silently drop (this is what surfaces a parser contract violation).
  SceneEntities2DDecoder decoder;
  const sdk::BuiltinObject not_entities = sdk::ImageAnnotations{};
  EXPECT_FALSE(decoder.decode(not_entities).has_value());
}

}  // namespace
}  // namespace PJ
