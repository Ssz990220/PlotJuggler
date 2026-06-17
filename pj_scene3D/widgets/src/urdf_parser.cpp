// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "urdf_parser.h"

#include <QByteArray>
#include <QDomDocument>
#include <QDomElement>
#include <QString>
#include <QStringList>
#include <array>
#include <unordered_map>

namespace pj::scene3d {

namespace {

// Bound on the synchronous GUI-thread DOM parse. Real URDFs are well under 10 MB
// of text; 32 MB is a generous safety margin. QDomDocument::setContent blocks the
// UI for the full parse duration, so we reject anything above this before handing
// it to Qt. (Entity-expansion and external-entity risks are already mitigated by
// Qt 6's QXmlStreamReader layer, so the residual risk is only UI-freeze.)
constexpr std::size_t kMaxUrdfBytes = 32u * 1024u * 1024u;

// Parse a whitespace-separated list of doubles ("x y z") into a fixed array.
// Returns false if fewer than N tokens parse.
template <std::size_t N>
bool parseDoubles(const QString& text, std::array<double, N>& out) {
  const QStringList toks = text.simplified().split(' ', Qt::SkipEmptyParts);
  if (static_cast<std::size_t>(toks.size()) < N) {
    return false;
  }
  for (std::size_t i = 0; i < N; ++i) {
    bool ok = false;
    out[i] = toks[static_cast<int>(i)].toDouble(&ok);
    if (!ok) {
      return false;
    }
  }
  return true;
}

double attrDouble(const QDomElement& el, const QString& name, double fallback) {
  if (!el.hasAttribute(name)) {
    return fallback;
  }
  bool ok = false;
  const double v = el.attribute(name).toDouble(&ok);
  return ok ? v : fallback;
}

// Read <origin xyz="..." rpy="..."/> into any target with origin_xyz/origin_rpy
// (LinkGeom or RobotJoint). Missing attributes leave the target's defaults.
template <typename T>
void parseOriginInto(const QDomElement& parent, T& out) {
  const QDomElement origin = parent.firstChildElement(QStringLiteral("origin"));
  if (origin.isNull()) {
    return;
  }
  std::array<double, 3> xyz{0, 0, 0};
  std::array<double, 3> rpy{0, 0, 0};
  if (parseDoubles(origin.attribute(QStringLiteral("xyz")), xyz)) {
    out.origin_xyz = {xyz[0], xyz[1], xyz[2]};
  }
  if (parseDoubles(origin.attribute(QStringLiteral("rpy")), rpy)) {
    out.origin_rpy = {rpy[0], rpy[1], rpy[2]};
  }
}

// Read <geometry> into a GeomShape. Mesh refs are dispatched to the resolver.
// Returns false if no recognized child geometry is present.
bool parseGeometry(
    const QDomElement& parent, UrdfPackageResolver* resolver, const std::string& urdf_dir, bool source_is_url,
    GeomShape& out) {
  const QDomElement geo = parent.firstChildElement(QStringLiteral("geometry"));
  if (geo.isNull()) {
    return false;
  }

  if (const QDomElement box = geo.firstChildElement(QStringLiteral("box")); !box.isNull()) {
    GeomBox b;
    std::array<double, 3> s{1, 1, 1};
    if (parseDoubles(box.attribute(QStringLiteral("size")), s)) {
      b.size = {s[0], s[1], s[2]};
    }
    out = b;
    return true;
  }
  if (const QDomElement cyl = geo.firstChildElement(QStringLiteral("cylinder")); !cyl.isNull()) {
    GeomCylinder c;
    c.radius = attrDouble(cyl, QStringLiteral("radius"), 1.0);
    c.length = attrDouble(cyl, QStringLiteral("length"), 1.0);
    out = c;
    return true;
  }
  if (const QDomElement sph = geo.firstChildElement(QStringLiteral("sphere")); !sph.isNull()) {
    GeomSphere s;
    s.radius = attrDouble(sph, QStringLiteral("radius"), 1.0);
    out = s;
    return true;
  }
  if (const QDomElement mesh = geo.firstChildElement(QStringLiteral("mesh")); !mesh.isNull()) {
    GeomMesh m;
    m.filename = mesh.attribute(QStringLiteral("filename")).toStdString();
    std::array<double, 3> sc{1, 1, 1};
    if (parseDoubles(mesh.attribute(QStringLiteral("scale")), sc)) {
      m.scale = {sc[0], sc[1], sc[2]};
    }
    if (resolver != nullptr && !m.filename.empty()) {
      const ResolvedMesh r = resolver->resolveUri(m.filename, urdf_dir, source_is_url);
      m.resolved = r.resolved;
      m.resolved_path = r.path;
      m.issue = r.issue;
      m.unresolved_package = r.package;
    }
    out = m;
    return true;
  }
  return false;
}

// Read a <color rgba="r g b a"/> element into a vec4. Returns nullopt when the
// element is null or "rgba" fails to parse; default {0.7,0.7,0.7,1.0} lives
// here so both callers share the same fallback.
std::optional<glm::vec4> readColorRgba(const QDomElement& color_el) {
  if (color_el.isNull()) {
    return std::nullopt;
  }
  std::array<double, 4> rgba{0.7, 0.7, 0.7, 1.0};
  if (!parseDoubles(color_el.attribute(QStringLiteral("rgba")), rgba)) {
    return std::nullopt;
  }
  return glm::vec4{
      static_cast<float>(rgba[0]), static_cast<float>(rgba[1]), static_cast<float>(rgba[2]),
      static_cast<float>(rgba[3])};
}

// Fills geom.color and sets geom.has_color when an inline or named color is
// found; otherwise leaves the LinkGeom defaults.
void parseMaterial(
    const QDomElement& parent, const std::unordered_map<std::string, glm::vec4>& materials, LinkGeom& geom) {
  const QDomElement mat = parent.firstChildElement(QStringLiteral("material"));
  if (mat.isNull()) {
    return;
  }
  if (auto rgba = readColorRgba(mat.firstChildElement(QStringLiteral("color")))) {
    geom.color = *rgba;
    geom.has_color = true;
    return;
  }
  // Named material reference: <material name="Foo"/> with no inline color.
  const std::string name = mat.attribute(QStringLiteral("name")).toStdString();
  if (!name.empty()) {
    auto it = materials.find(name);
    if (it != materials.end()) {
      geom.color = it->second;
      geom.has_color = true;
    }
  }
}

// Parse a single <visual> or <collision> into a LinkGeom. Returns false if it
// has no recognizable geometry.
bool parseGeomElement(
    const QDomElement& el, UrdfPackageResolver* resolver, const std::string& urdf_dir, bool source_is_url,
    const std::unordered_map<std::string, glm::vec4>& materials, LinkGeom& out) {
  if (!parseGeometry(el, resolver, urdf_dir, source_is_url, out.shape)) {
    return false;
  }
  parseOriginInto(el, out);
  parseMaterial(el, materials, out);
  return true;
}

// Map a URDF joint `type` attribute to JointType (unknown/empty ⇒ kOther).
JointType jointTypeFromString(const QString& type) {
  if (type == QStringLiteral("fixed")) {
    return JointType::kFixed;
  }
  if (type == QStringLiteral("revolute")) {
    return JointType::kRevolute;
  }
  if (type == QStringLiteral("continuous")) {
    return JointType::kContinuous;
  }
  if (type == QStringLiteral("prismatic")) {
    return JointType::kPrismatic;
  }
  if (type == QStringLiteral("floating")) {
    return JointType::kFloating;
  }
  if (type == QStringLiteral("planar")) {
    return JointType::kPlanar;
  }
  return JointType::kOther;
}

}  // namespace

bool looksLikeXacro(const std::string& xml, const std::string& filename) {
  if (!filename.empty()) {
    const QString f = QString::fromStdString(filename);
    if (f.endsWith(QLatin1String(".xacro"), Qt::CaseInsensitive)) {
      return true;
    }
  }
  // A `<xacro:` element opener. An `xmlns:xacro` namespace declaration ALONE is
  // not treated as xacro — a plain (already-expanded) URDF may legitimately
  // carry the namespace attribute and must still parse.
  return QString::fromStdString(xml).contains(QLatin1String("<xacro:"));
}

std::pair<std::optional<RobotModel>, std::string> parseUrdf(
    const std::string& xml, UrdfPackageResolver* resolver, const std::string& urdf_dir, bool source_is_url,
    const std::string& filename) {
  if (looksLikeXacro(xml, filename)) {
    return {std::nullopt, "Unsupported format: xacro — run `xacro input.xacro > output.urdf` and load the result."};
  }
  if (xml.size() > kMaxUrdfBytes) {
    const double size_mib = static_cast<double>(xml.size()) / (1024.0 * 1024.0);
    return {
        std::nullopt,
        "robot_description too large (" + std::to_string(static_cast<int>(size_mib + 0.5)) + " MiB, limit 32 MiB)"};
  }

  QDomDocument doc;
  const QByteArray bytes = QByteArray::fromStdString(xml);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  const auto result = doc.setContent(bytes);
  if (!result) {
    return {std::nullopt, "Parse error: " + result.errorMessage.toStdString()};
  }
#else
  QString error_msg;
  int error_line = 0;
  int error_col = 0;
  if (!doc.setContent(bytes, &error_msg, &error_line, &error_col)) {
    return {std::nullopt, "Parse error: " + error_msg.toStdString()};
  }
#endif

  const QDomElement root = doc.documentElement();
  if (root.isNull()) {
    return {std::nullopt, "Parse error: empty document"};
  }
  // Format inference from the root element.
  const QString root_tag = root.tagName();
  if (root_tag != QStringLiteral("robot")) {
    return {std::nullopt, "Format '" + root_tag.toStdString() + "' is not supported — only URDF"};
  }

  RobotModel model;

  // Pass 1 — collect top-level named materials (<robot><material name color>).
  std::unordered_map<std::string, glm::vec4> materials;
  for (QDomElement mat = root.firstChildElement(QStringLiteral("material")); !mat.isNull();
       mat = mat.nextSiblingElement(QStringLiteral("material"))) {
    const std::string name = mat.attribute(QStringLiteral("name")).toStdString();
    if (name.empty()) {
      continue;
    }
    if (auto rgba = readColorRgba(mat.firstChildElement(QStringLiteral("color")))) {
      materials[name] = *rgba;
    }
  }

  // Pass 2 — links. <joint> is intentionally skipped.
  for (QDomElement link_el = root.firstChildElement(QStringLiteral("link")); !link_el.isNull();
       link_el = link_el.nextSiblingElement(QStringLiteral("link"))) {
    RobotLink link;
    link.name = link_el.attribute(QStringLiteral("name")).toStdString();

    for (QDomElement v = link_el.firstChildElement(QStringLiteral("visual")); !v.isNull();
         v = v.nextSiblingElement(QStringLiteral("visual"))) {
      LinkGeom g;
      if (parseGeomElement(v, resolver, urdf_dir, source_is_url, materials, g)) {
        link.visuals.push_back(std::move(g));
      }
    }
    for (QDomElement c = link_el.firstChildElement(QStringLiteral("collision")); !c.isNull();
         c = c.nextSiblingElement(QStringLiteral("collision"))) {
      LinkGeom g;
      if (parseGeomElement(c, resolver, urdf_dir, source_is_url, materials, g)) {
        link.collisions.push_back(std::move(g));
      }
    }

    if (model.root_link.empty()) {
      model.root_link = link.name;  // first declared link
    }
    model.links.push_back(std::move(link));
  }

  // Pass 3 — joints. We keep the kinematic edge (parent/child/type/origin) so a
  // FIXED joint can later be injected as a static TF bridge for a frame the data
  // never publishes; link poses still come from the live TF tree. A joint missing
  // its parent or child link is skipped (it cannot define an edge).
  for (QDomElement joint_el = root.firstChildElement(QStringLiteral("joint")); !joint_el.isNull();
       joint_el = joint_el.nextSiblingElement(QStringLiteral("joint"))) {
    RobotJoint joint;
    joint.name = joint_el.attribute(QStringLiteral("name")).toStdString();
    joint.type = jointTypeFromString(joint_el.attribute(QStringLiteral("type")));
    joint.parent = joint_el.firstChildElement(QStringLiteral("parent")).attribute(QStringLiteral("link")).toStdString();
    joint.child = joint_el.firstChildElement(QStringLiteral("child")).attribute(QStringLiteral("link")).toStdString();
    if (joint.parent.empty() || joint.child.empty()) {
      continue;
    }
    parseOriginInto(joint_el, joint);
    model.joints.push_back(std::move(joint));
  }

  if (model.links.empty()) {
    return {std::nullopt, "Parse error: <robot> has no <link> elements"};
  }
  return {std::move(model), std::string{}};
}

}  // namespace pj::scene3d
