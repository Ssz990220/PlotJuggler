// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "urdf_parser.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <fstream>
#include <sstream>
#include <string>

#include "pj_scene3d_core/robot_model.h"
#include "urdf_package_resolver.h"

namespace pj::scene3d {
namespace {

#ifndef PJ_SCENE3D_FIXTURES_DIR
#error "PJ_SCENE3D_FIXTURES_DIR must be defined by the build"
#endif

std::string readFixture(const std::string& name) {
  std::ifstream in(std::string(PJ_SCENE3D_FIXTURES_DIR) + "/" + name, std::ios::binary);
  EXPECT_TRUE(in.good()) << "missing fixture: " << name;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Find a link by name (helper; links are an unordered vector).
const RobotLink* findLink(const RobotModel& m, const std::string& name) {
  for (const auto& l : m.links) {
    if (l.name == name) {
      return &l;
    }
  }
  return nullptr;
}

TEST(UrdfParser, ParsesTwoLinkModel) {
  const std::string xml = readFixture("two_link.urdf");
  UrdfPackageResolver resolver;  // no roots seeded ⇒ package:// will be unresolved
  auto [model, err] = parseUrdf(xml, &resolver, std::string(PJ_SCENE3D_FIXTURES_DIR), /*source_is_url=*/false);
  ASSERT_TRUE(model.has_value()) << err;
  EXPECT_TRUE(err.empty());

  EXPECT_EQ(model->root_link, "base_link");
  ASSERT_EQ(model->links.size(), 2u);
  EXPECT_NE(findLink(*model, "base_link"), nullptr);
  EXPECT_NE(findLink(*model, "arm_link"), nullptr);
}

TEST(UrdfParser, MultipleVisualsAndCollisionsPerLink) {
  const std::string xml = readFixture("two_link.urdf");
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, std::string(PJ_SCENE3D_FIXTURES_DIR), false);
  ASSERT_TRUE(model.has_value()) << err;

  const RobotLink* base = findLink(*model, "base_link");
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(base->visuals.size(), 2u);     // mesh + box
  EXPECT_EQ(base->collisions.size(), 1u);  // bare-relative mesh

  const RobotLink* arm = findLink(*model, "arm_link");
  ASSERT_NE(arm, nullptr);
  EXPECT_EQ(arm->visuals.size(), 1u);     // cylinder
  EXPECT_EQ(arm->collisions.size(), 1u);  // bare-relative-that-looks-like-package
}

TEST(UrdfParser, PrimitiveGeometriesParsed) {
  const std::string xml = readFixture("two_link.urdf");
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, std::string(PJ_SCENE3D_FIXTURES_DIR), false);
  ASSERT_TRUE(model.has_value()) << err;

  const RobotLink* base = findLink(*model, "base_link");
  ASSERT_NE(base, nullptr);
  // visuals[1] is the box primitive.
  ASSERT_EQ(kindOf(base->visuals[1].shape), GeomKind::kBox);
  const auto& box = std::get<GeomBox>(base->visuals[1].shape);
  EXPECT_DOUBLE_EQ(box.size.x, 0.3);
  EXPECT_DOUBLE_EQ(box.size.y, 0.2);
  EXPECT_DOUBLE_EQ(box.size.z, 0.1);

  const RobotLink* arm = findLink(*model, "arm_link");
  ASSERT_NE(arm, nullptr);
  ASSERT_EQ(kindOf(arm->visuals[0].shape), GeomKind::kCylinder);
  const auto& cyl = std::get<GeomCylinder>(arm->visuals[0].shape);
  EXPECT_DOUBLE_EQ(cyl.radius, 0.05);
  EXPECT_DOUBLE_EQ(cyl.length, 1.0);
}

TEST(UrdfParser, NamedMaterialResolvedAndInlineColor) {
  const std::string xml = readFixture("two_link.urdf");
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, std::string(PJ_SCENE3D_FIXTURES_DIR), false);
  ASSERT_TRUE(model.has_value()) << err;

  const RobotLink* base = findLink(*model, "base_link");
  ASSERT_NE(base, nullptr);
  // visuals[0]: <material name="Blue"/> resolves to the robot-level color.
  EXPECT_TRUE(base->visuals[0].has_color);
  EXPECT_FLOAT_EQ(base->visuals[0].color.b, 1.0f);
  EXPECT_FLOAT_EQ(base->visuals[0].color.r, 0.0f);
  // visuals[1]: inline <color rgba="1 0 0 1"/>.
  EXPECT_TRUE(base->visuals[1].has_color);
  EXPECT_FLOAT_EQ(base->visuals[1].color.r, 1.0f);
}

TEST(UrdfParser, OriginComposedTranslateThenRotate) {
  const std::string xml = readFixture("two_link.urdf");
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, std::string(PJ_SCENE3D_FIXTURES_DIR), false);
  ASSERT_TRUE(model.has_value()) << err;

  const RobotLink* base = findLink(*model, "base_link");
  ASSERT_NE(base, nullptr);
  EXPECT_DOUBLE_EQ(base->visuals[0].origin_xyz.z, 0.1);
  EXPECT_NEAR(base->visuals[0].origin_rpy.z, 1.5708, 1e-4);

  // originToMat4 = translate(xyz)*Rz: a +90deg yaw maps local +X to world +Y.
  const glm::dmat4 m = originToMat4(base->visuals[0].origin_xyz, base->visuals[0].origin_rpy);
  const glm::dvec4 x_axis = m * glm::dvec4(1, 0, 0, 0);
  EXPECT_NEAR(x_axis.x, 0.0, 1e-3);
  EXPECT_NEAR(x_axis.y, 1.0, 1e-3);
  // Translation column is the xyz.
  EXPECT_DOUBLE_EQ(m[3][2], 0.1);
}

// The bare-path-that-looks-like-a-package case: "robotiq_arg85_description/.."
// has no package:// prefix → the guard resolves it against urdf_dir, never the
// package search.
TEST(UrdfParser, BarePathLooksLikePackageResolvesAgainstUrdfDir) {
  const std::string xml = readFixture("two_link.urdf");
  UrdfPackageResolver resolver;
  const std::string urdf_dir = std::string(PJ_SCENE3D_FIXTURES_DIR);
  auto [model, err] = parseUrdf(xml, &resolver, urdf_dir, false);
  ASSERT_TRUE(model.has_value()) << err;

  const RobotLink* arm = findLink(*model, "arm_link");
  ASSERT_NE(arm, nullptr);
  const auto& mesh = std::get<GeomMesh>(arm->collisions[0].shape);
  EXPECT_EQ(mesh.filename, "robotiq_arg85_description/meshes/gripper.stl");
  EXPECT_TRUE(mesh.resolved);  // resolved as a bare relative path …
  // … against urdf_dir, NOT via the package search (which never ran).
  EXPECT_EQ(mesh.resolved_path, urdf_dir + "/robotiq_arg85_description/meshes/gripper.stl");
  // The guard kept this bare path out of the package chain: it is NEVER recorded
  // as an unresolved package. (demo_description, a real package:// ref, may be.)
  EXPECT_FALSE(resolver.unresolvedPackages().contains("robotiq_arg85_description"));
}

TEST(UrdfParser, PackageRefRecordedUnresolvedWhenNoRoots) {
  const std::string xml = readFixture("two_link.urdf");
  UrdfPackageResolver resolver;  // no roots, no attachments
  auto [model, err] = parseUrdf(xml, &resolver, std::string(PJ_SCENE3D_FIXTURES_DIR), false);
  ASSERT_TRUE(model.has_value()) << err;

  const RobotLink* base = findLink(*model, "base_link");
  ASSERT_NE(base, nullptr);
  const auto& mesh = std::get<GeomMesh>(base->visuals[0].shape);
  EXPECT_FALSE(mesh.resolved);
  EXPECT_TRUE(resolver.unresolvedPackages().contains("demo_description"));
}

TEST(UrdfParser, XacroRejectedWithExplicitError) {
  const std::string xml = readFixture("robot.urdf.xacro");
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, std::string(PJ_SCENE3D_FIXTURES_DIR), false);
  EXPECT_FALSE(model.has_value());
  EXPECT_NE(err.find("xacro"), std::string::npos) << err;
}

TEST(UrdfParser, NonRobotRootRejected) {
  const std::string xml = R"(<?xml version="1.0"?><sdf version="1.6"><model/></sdf>)";
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, "", false);
  EXPECT_FALSE(model.has_value());
  EXPECT_NE(err.find("sdf"), std::string::npos) << err;
}

TEST(UrdfParser, MalformedXmlRejected) {
  const std::string xml = R"(<robot name="x"><link name="a")";  // truncated
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, "", false);
  EXPECT_FALSE(model.has_value());
  EXPECT_FALSE(err.empty());
}

TEST(UrdfParser, JointsIgnored) {
  // A <joint> between the links must be silently ignored (TF owns kinematics).
  const std::string xml = R"(<?xml version="1.0"?>
    <robot name="jr">
      <link name="a"/>
      <joint name="a_to_b" type="fixed">
        <parent link="a"/><child link="b"/>
        <origin xyz="1 2 3"/>
      </joint>
      <link name="b"/>
    </robot>)";
  UrdfPackageResolver resolver;
  auto [model, err] = parseUrdf(xml, &resolver, "", false);
  ASSERT_TRUE(model.has_value()) << err;
  EXPECT_EQ(model->links.size(), 2u);  // a + b; the joint contributed no link
}

}  // namespace
}  // namespace pj::scene3d
