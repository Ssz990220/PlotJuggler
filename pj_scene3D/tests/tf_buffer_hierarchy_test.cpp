// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>

#include "pj_scene3d_core/tf/tf_buffer.h"

namespace PJ {
namespace {

using pj::scene3d::FrameRow;
using pj::scene3d::StampedTransform;
using pj::scene3d::TimePoint;
using pj::scene3d::Transform;
using pj::scene3d::TransformBuffer;
using namespace std::chrono_literals;

Transform identity() {
  return Transform{{0.0, 0.0, 0.0}, glm::dquat{1.0, 0.0, 0.0, 0.0}};
}

StampedTransform makeStatic(const std::string& parent, const std::string& child) {
  return StampedTransform{TimePoint{}, parent, child, identity()};
}

// ----------------------- getFrameHierarchy (flat) -----------------------

TEST(TfBufferHierarchy, EmptyBufferReturnsEmptyVector) {
  TransformBuffer buf;
  EXPECT_TRUE(buf.getFrameHierarchy().empty());
}

TEST(TfBufferHierarchy, SingleEdgeProducesRootAndChild) {
  TransformBuffer buf;
  buf.setTransform(makeStatic("odom", "base_link"));

  const auto rows = buf.getFrameHierarchy();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0].name, "odom");
  EXPECT_EQ(rows[0].depth, 0);
  EXPECT_EQ(rows[1].name, "base_link");
  EXPECT_EQ(rows[1].depth, 1);
}

TEST(TfBufferHierarchy, DisconnectedForestsKeepBothRootsAtDepthZero) {
  TransformBuffer buf;
  buf.setTransform(makeStatic("map", "robot"));
  buf.setTransform(makeStatic("world", "drone"));

  const auto rows = buf.getFrameHierarchy();
  ASSERT_EQ(rows.size(), 4U);
  EXPECT_EQ(rows[0].name, "map");
  EXPECT_EQ(rows[0].depth, 0);
  EXPECT_EQ(rows[1].name, "robot");
  EXPECT_EQ(rows[1].depth, 1);
  EXPECT_EQ(rows[2].name, "world");
  EXPECT_EQ(rows[2].depth, 0);
  EXPECT_EQ(rows[3].name, "drone");
  EXPECT_EQ(rows[3].depth, 1);
}

TEST(TfBufferHierarchy, ChildrenAtSameDepthAreAlphabetical) {
  TransformBuffer buf;
  // Insert in non-alphabetical order; expect alphabetical output.
  buf.setTransform(makeStatic("base_link", "z_link"));
  buf.setTransform(makeStatic("base_link", "a_link"));
  buf.setTransform(makeStatic("base_link", "m_link"));
  buf.setTransform(makeStatic("odom", "base_link"));

  const auto rows = buf.getFrameHierarchy();
  ASSERT_EQ(rows.size(), 5U);
  EXPECT_EQ(rows[0].name, "odom");
  EXPECT_EQ(rows[1].name, "base_link");
  EXPECT_EQ(rows[2].name, "a_link");
  EXPECT_EQ(rows[3].name, "m_link");
  EXPECT_EQ(rows[4].name, "z_link");
}

TEST(TfBufferHierarchy, MixedCaseSiblingsSortCaseInsensitively) {
  TransformBuffer buf;
  // Foxglove-style: capital-O frames must interleave with lowercase
  // siblings, not cluster ahead of them because uppercase < lowercase ASCII.
  buf.setTransform(makeStatic("base_link", "Omniwheel_1"));
  buf.setTransform(makeStatic("base_link", "imu_link"));
  buf.setTransform(makeStatic("base_link", "Omniwheel_2"));
  buf.setTransform(makeStatic("base_link", "rail_left"));
  buf.setTransform(makeStatic("odom", "base_link"));

  const auto rows = buf.getFrameHierarchy();
  ASSERT_EQ(rows.size(), 6U);
  EXPECT_EQ(rows[0].name, "odom");
  EXPECT_EQ(rows[1].name, "base_link");
  // Children sorted case-insensitively: imu < Omniwheel_1 < Omniwheel_2 < rail
  EXPECT_EQ(rows[2].name, "imu_link");
  EXPECT_EQ(rows[3].name, "Omniwheel_1");
  EXPECT_EQ(rows[4].name, "Omniwheel_2");
  EXPECT_EQ(rows[5].name, "rail_left");
}

TEST(TfBufferHierarchy, FlatWalkIsCycleSafe) {
  TransformBuffer buf;
  buf.setTransform(makeStatic("a", "b"));
  // Attempting to set b->a would throw (parents_ enforces single-parent).
  // To exercise the visited guard, manually craft via two non-conflicting
  // edges that form a degenerate self-cycle is not directly possible with
  // the public API. Instead, verify a deep linear chain terminates.
  buf.setTransform(makeStatic("b", "c"));
  buf.setTransform(makeStatic("c", "d"));

  const auto rows = buf.getFrameHierarchy();
  ASSERT_EQ(rows.size(), 4U);
  EXPECT_EQ(rows[0].name, "a");
  EXPECT_EQ(rows[3].name, "d");
  EXPECT_EQ(rows[3].depth, 3);
}

// ----------------------- getFrameHierarchyJson (nested) -----------------------

TEST(TfBufferHierarchyJson, EmptyBufferReturnsEmptyArray) {
  TransformBuffer buf;
  const auto json = buf.getFrameHierarchyJson();
  EXPECT_TRUE(json.is_array());
  EXPECT_EQ(json.size(), 0U);
}

TEST(TfBufferHierarchyJson, SingleEdgeNestsChild) {
  TransformBuffer buf;
  buf.setTransform(makeStatic("odom", "base_link"));

  const auto json = buf.getFrameHierarchyJson();
  const auto expected = nlohmann::json::parse(R"([
    {"name":"odom","children":[
      {"name":"base_link","children":[]}
    ]}
  ])");
  EXPECT_EQ(json, expected);
}

TEST(TfBufferHierarchyJson, DisconnectedForestsAppearAsTwoTopLevelEntries) {
  TransformBuffer buf;
  buf.setTransform(makeStatic("map", "robot"));
  buf.setTransform(makeStatic("world", "drone"));

  const auto json = buf.getFrameHierarchyJson();
  ASSERT_TRUE(json.is_array());
  ASSERT_EQ(json.size(), 2U);
  EXPECT_EQ(json[0]["name"], "map");
  EXPECT_EQ(json[0]["children"][0]["name"], "robot");
  EXPECT_EQ(json[1]["name"], "world");
  EXPECT_EQ(json[1]["children"][0]["name"], "drone");
}

TEST(TfBufferHierarchyJson, ChildrenAlphabeticalAtEachDepth) {
  TransformBuffer buf;
  buf.setTransform(makeStatic("base_link", "z_link"));
  buf.setTransform(makeStatic("base_link", "a_link"));
  buf.setTransform(makeStatic("base_link", "m_link"));
  buf.setTransform(makeStatic("odom", "base_link"));

  const auto json = buf.getFrameHierarchyJson();
  ASSERT_EQ(json.size(), 1U);
  const auto& base_link_children = json[0]["children"][0]["children"];
  ASSERT_EQ(base_link_children.size(), 3U);
  EXPECT_EQ(base_link_children[0]["name"], "a_link");
  EXPECT_EQ(base_link_children[1]["name"], "m_link");
  EXPECT_EQ(base_link_children[2]["name"], "z_link");
}

TEST(TfBufferHierarchyJson, DeepChainTerminates) {
  TransformBuffer buf;
  buf.setTransform(makeStatic("a", "b"));
  buf.setTransform(makeStatic("b", "c"));
  buf.setTransform(makeStatic("c", "d"));

  const auto json = buf.getFrameHierarchyJson();
  ASSERT_EQ(json.size(), 1U);
  EXPECT_EQ(json[0]["name"], "a");
  EXPECT_EQ(json[0]["children"][0]["name"], "b");
  EXPECT_EQ(json[0]["children"][0]["children"][0]["name"], "c");
  EXPECT_EQ(json[0]["children"][0]["children"][0]["children"][0]["name"], "d");
  EXPECT_TRUE(json[0]["children"][0]["children"][0]["children"][0]["children"].empty());
}

}  // namespace
}  // namespace PJ
