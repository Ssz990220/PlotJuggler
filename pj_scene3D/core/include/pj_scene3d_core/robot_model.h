#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Pure URDF data model — no Qt, no assimp, no GL. The URDF parser
// (pj_scene3d_widgets/urdf_parser) fills these structs; render passes consume
// them. Per the design "Crucial framing": the URDF is a frame→visual decoration
// map (links only); TF owns kinematics, so <joint> is never represented here.

#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <variant>
#include <vector>

namespace pj::scene3d {

// Geometry kind discriminator for GeomShape. Mesh is the resolved/unresolved
// reference path; primitives carry their dimensions inline.
enum class GeomKind : std::uint8_t {
  kBox,
  kCylinder,
  kSphere,
  kMesh,
};

// <box size="x y z"/>
struct GeomBox {
  glm::dvec3 size{1.0, 1.0, 1.0};
};

// <cylinder radius="r" length="l"/> (URDF cylinders are Z-aligned)
struct GeomCylinder {
  double radius{1.0};
  double length{1.0};
};

// <sphere radius="r"/>
struct GeomSphere {
  double radius{1.0};
};

// <mesh filename="..." scale="x y z"/>. `filename` is the verbatim reference as
// written in the URDF (package://, file://, http(s)://, or bare relative). The
// resolver turns this into a loadable path; `resolved_path` is filled in once a
// resolution attempt has run ("" + resolved=false ⇒ unresolved ⇒ placeholder).
struct GeomMesh {
  std::string filename;  // verbatim reference string from the URDF
  glm::dvec3 scale{1.0, 1.0, 1.0};
  std::string resolved_path;  // empty until resolved
  bool resolved{false};
};

using GeomShape = std::variant<GeomBox, GeomCylinder, GeomSphere, GeomMesh>;

inline GeomKind kindOf(const GeomShape& s) {
  switch (s.index()) {
    case 0:
      return GeomKind::kBox;
    case 1:
      return GeomKind::kCylinder;
    case 2:
      return GeomKind::kSphere;
    default:
      return GeomKind::kMesh;
  }
}

// One <visual> or <collision> element: a shape, its origin offset within the
// link frame (translation + RPY), and an optional material color.
struct LinkGeom {
  GeomShape shape;
  glm::dvec3 origin_xyz{0.0, 0.0, 0.0};
  glm::dvec3 origin_rpy{0.0, 0.0, 0.0};  // roll, pitch, yaw (radians)
  glm::vec4 color{0.7f, 0.7f, 0.7f, 1.0f};
  bool has_color{false};  // true when an explicit <material><color> applied
};

// One <link>: its name (== TF frame name) plus all visual & collision geoms.
// Joints are intentionally absent — TF positions every link.
struct RobotLink {
  std::string name;
  std::vector<LinkGeom> visuals;
  std::vector<LinkGeom> collisions;
};

// The parsed robot: every link keyed by name, plus the inferred root link name
// (first link declared; URDF has no explicit root without the joint graph,
// which we deliberately ignore).
struct RobotModel {
  std::string root_link;
  std::vector<RobotLink> links;
};

// Compose a link/visual origin into a 4x4 matrix: translate(xyz) * Rz * Ry * Rx
// (URDF RPY convention: intrinsic X-Y-Z == extrinsic Z-Y-X applied as Rz·Ry·Rx).
inline glm::dmat4 originToMat4(const glm::dvec3& xyz, const glm::dvec3& rpy) {
  const double cr = std::cos(rpy.x), sr = std::sin(rpy.x);  // roll  (X)
  const double cp = std::cos(rpy.y), sp = std::sin(rpy.y);  // pitch (Y)
  const double cy = std::cos(rpy.z), sy = std::sin(rpy.z);  // yaw   (Z)

  // R = Rz(yaw) * Ry(pitch) * Rx(roll), expanded.
  glm::dmat4 m(1.0);
  m[0][0] = cy * cp;
  m[0][1] = sy * cp;
  m[0][2] = -sp;
  m[1][0] = cy * sp * sr - sy * cr;
  m[1][1] = sy * sp * sr + cy * cr;
  m[1][2] = cp * sr;
  m[2][0] = cy * sp * cr + sy * sr;
  m[2][1] = sy * sp * cr - cy * sr;
  m[2][2] = cp * cr;
  m[3][0] = xyz.x;
  m[3][1] = xyz.y;
  m[3][2] = xyz.z;
  return m;
}

}  // namespace pj::scene3d
