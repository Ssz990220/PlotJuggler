// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Standalone URDF mesh viewer — a fast look-development loop for the 3D mesh
// pipeline (textures, tonemap, SSAO, lighting) without loading an MCAP.
//
//   scene3d_mesh_viewer <robot.urdf> [mesh-search-root]
//
// Renders through the REAL production path: RobotModelLayer (File source) into
// SceneViewWidget (HDR chain -> SSAO -> composite). The only fakery is the TF
// tree: the app parses the URDF's <joint> origins into a static zero-pose
// hierarchy (every joint at its neutral origin) — the product never does this
// (TF from data is truth there), but a bag-free viewer must pose links somehow.
// Mesh refs resolve exactly as in the app (ancestor heuristic from the URDF's
// directory; pass a search root for detached layouts).

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDomDocument>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <cstdio>
#include <functional>
#include <glm/gtc/quaternion.hpp>
#include <memory>

#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/layers/robot_model_layer.h"
#include "pj_scene3d_widgets/passes/mesh_render_pass.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "urdf_package_resolver.h"  // private widgets/src header, like the tests

namespace {

glm::dvec3 parseTriple(const QString& text) {
  const QStringList parts = text.split(' ', Qt::SkipEmptyParts);
  glm::dvec3 v{0.0, 0.0, 0.0};
  for (int i = 0; i < parts.size() && i < 3; ++i) {
    v[i] = parts[i].toDouble();
  }
  return v;
}

// URDF rpy convention: R = Rz(yaw) * Ry(pitch) * Rx(roll).
glm::dquat quatFromRpy(const glm::dvec3& rpy) {
  const glm::dquat qx = glm::angleAxis(rpy.x, glm::dvec3{1.0, 0.0, 0.0});
  const glm::dquat qy = glm::angleAxis(rpy.y, glm::dvec3{0.0, 1.0, 0.0});
  const glm::dquat qz = glm::angleAxis(rpy.z, glm::dvec3{0.0, 0.0, 1.0});
  return qz * qy * qx;
}

// Seed the buffer with the URDF's zero-configuration pose: one static
// parent->child transform per <joint>, taken from its <origin>. All joint
// types are treated as fixed at their neutral position.
int seedZeroPoseTf(const QString& urdf_path, pj::scene3d::TransformBuffer& tf) {
  QFile file(urdf_path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    std::fprintf(stderr, "cannot read %s\n", qPrintable(urdf_path));
    return 0;
  }
  QDomDocument doc;
  if (!doc.setContent(&file)) {
    std::fprintf(stderr, "not parseable XML: %s\n", qPrintable(urdf_path));
    return 0;
  }
  int joints = 0;
  for (QDomElement joint = doc.documentElement().firstChildElement(QStringLiteral("joint")); !joint.isNull();
       joint = joint.nextSiblingElement(QStringLiteral("joint"))) {
    const QString parent = joint.firstChildElement(QStringLiteral("parent")).attribute(QStringLiteral("link"));
    const QString child = joint.firstChildElement(QStringLiteral("child")).attribute(QStringLiteral("link"));
    if (parent.isEmpty() || child.isEmpty()) {
      continue;
    }
    const QDomElement origin = joint.firstChildElement(QStringLiteral("origin"));
    const glm::dvec3 xyz = parseTriple(origin.attribute(QStringLiteral("xyz")));
    const glm::dvec3 rpy = parseTriple(origin.attribute(QStringLiteral("rpy")));
    const auto result = tf.setTransform(
        pj::scene3d::StampedTransform{
            .stamp = PJ::fromRaw(0),
            .parent_frame = parent.toStdString(),
            .child_frame = child.toStdString(),
            .transform = pj::scene3d::Transform(xyz, quatFromRpy(rpy)),
        });
    if (result.has_value()) {
      ++joints;
    }
  }
  return joints;
}

QString rootLinkName(const QString& urdf_path) {
  // First <link> declared == the parser's root inference; good enough here.
  QFile file(urdf_path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return {};
  }
  QDomDocument doc;
  if (!doc.setContent(&file)) {
    return {};
  }
  return doc.documentElement().firstChildElement(QStringLiteral("link")).attribute(QStringLiteral("name"));
}

// Slider mapping value/100 -> float, with a live value label.
QSlider* addSlider(
    QFormLayout* form, const QString& label, int min, int max, int value, const std::function<void(float)>& apply) {
  auto* slider = new QSlider(Qt::Horizontal);
  slider->setRange(min, max);
  slider->setValue(value);
  auto* name = new QLabel(QStringLiteral("%1 (%2)").arg(label).arg(value / 100.0));
  QObject::connect(slider, &QSlider::valueChanged, name, [name, label, apply](int v) {
    name->setText(QStringLiteral("%1 (%2)").arg(label).arg(v / 100.0));
    apply(static_cast<float>(v) / 100.0f);
  });
  form->addRow(name, slider);
  return slider;
}

// Look-development control panel: the knobs most relevant to dialing in the
// scene appearance (composite, SSAO, mesh shading).
QWidget* makeControls(pj::scene3d::SceneViewWidget& view) {
  auto* panel = new QWidget;
  auto* form = new QFormLayout(panel);
  form->setContentsMargins(8, 8, 8, 8);
  const auto repaint = [&view] { view.update(); };

  auto* tonemap = new QComboBox;
  tonemap->addItems({QStringLiteral("None"), QStringLiteral("ACES"), QStringLiteral("AgX")});
  tonemap->setCurrentIndex(view.compositeParams().tonemap_mode);
  QObject::connect(tonemap, &QComboBox::currentIndexChanged, &view, [&view, repaint](int idx) {
    view.compositeParams().tonemap_mode = idx;
    repaint();
  });
  form->addRow(QStringLiteral("Tonemap"), tonemap);

  addSlider(
      form, QStringLiteral("Exposure"), 25, 400, static_cast<int>(view.compositeParams().exposure * 100),
      [&view, repaint](float v) {
        view.compositeParams().exposure = v;
        repaint();
      });
  addSlider(
      form, QStringLiteral("Saturation"), 0, 250, static_cast<int>(view.compositeParams().saturation * 100),
      [&view, repaint](float v) {
        view.compositeParams().saturation = v;
        repaint();
      });

  auto* ssao_box = new QCheckBox(QStringLiteral("SSAO"));
  ssao_box->setChecked(view.compositeParams().ssao_enabled);
  QObject::connect(ssao_box, &QCheckBox::toggled, &view, [&view, repaint](bool on) {
    view.compositeParams().ssao_enabled = on;
    repaint();
  });
  form->addRow(ssao_box);
  addSlider(form, QStringLiteral("AO strength"), 0, 100, 100, [&view, repaint](float v) {
    view.compositeParams().ao_strength = v;
    repaint();
  });
  addSlider(form, QStringLiteral("AO radius m"), 5, 200, 50, [&view, repaint](float v) {
    view.ssaoPass().setRadius(v);
    repaint();
  });
  addSlider(form, QStringLiteral("AO power"), 50, 500, 100, [&view, repaint](float v) {
    view.ssaoPass().setPower(v);
    repaint();
  });

  auto* edl_box = new QCheckBox(QStringLiteral("EDL"));
  edl_box->setChecked(view.compositeParams().edl_enabled);
  QObject::connect(edl_box, &QCheckBox::toggled, &view, [&view, repaint](bool on) {
    view.compositeParams().edl_enabled = on;
    repaint();
  });
  form->addRow(edl_box);
  addSlider(form, QStringLiteral("EDL strength"), 0, 400, 100, [&view, repaint](float v) {
    view.edlPass().setStrength(v);
    repaint();
  });
  addSlider(form, QStringLiteral("EDL radius px"), 10, 500, 60, [&view, repaint](float v) {
    view.edlPass().setRadiusPx(v);
    repaint();
  });

  auto& shading = pj::scene3d::meshShadingParams();
  addSlider(
      form, QStringLiteral("Roughness"), 5, 100, static_cast<int>(shading.roughness * 100),
      [&shading, repaint](float v) {
        shading.roughness = v;
        repaint();
      });
  addSlider(
      form, QStringLiteral("Reflectivity"), 0, 25, static_cast<int>(shading.reflectivity * 100),
      [&shading, repaint](float v) {
        shading.reflectivity = v;
        repaint();
      });
  addSlider(
      form, QStringLiteral("Ambient"), 0, 250, static_cast<int>(shading.ambient_scale * 100),
      [&shading, repaint](float v) {
        shading.ambient_scale = v;
        repaint();
      });
  addSlider(
      form, QStringLiteral("Direct light"), 0, 250, static_cast<int>(shading.direct_scale * 100),
      [&shading, repaint](float v) {
        shading.direct_scale = v;
        repaint();
      });

  auto* axes_box = new QCheckBox(QStringLiteral("TF axes"));
  axes_box->setChecked(false);
  QObject::connect(axes_box, &QCheckBox::toggled, &view, [&view](bool on) { view.setAxesVisible(on); });
  form->addRow(axes_box);

  panel->setFixedWidth(300);
  return panel;
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QString urdf_arg;
  if (argc >= 2) {
    urdf_arg = QString::fromLocal8Bit(argv[1]);
  } else {
    urdf_arg = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("Open URDF"), QString(), QStringLiteral("URDF files (*.urdf *.xml);;All files (*)"));
    if (urdf_arg.isEmpty()) {
      std::fprintf(stderr, "usage: scene3d_mesh_viewer <robot.urdf> [mesh-search-root]\n");
      return 1;
    }
  }
  const QString urdf_path = QFileInfo(urdf_arg).absoluteFilePath();

  PJ::SessionManager session;
  auto tf = std::make_shared<pj::scene3d::TransformBuffer>(pj::scene3d::TransformBuffer::kKeepAll);
  const int joints = seedZeroPoseTf(urdf_path, *tf);
  const QString root = rootLinkName(urdf_path);
  std::printf(
      "[mesh_viewer] %s: %d joints seeded at zero pose, root link '%s'\n", qPrintable(urdf_path), joints,
      qPrintable(root));

  pj::scene3d::UrdfPackageResolver resolver;
  if (argc >= 3) {
    resolver.addSearchRoot(QString::fromLocal8Bit(argv[2]));
  }

  pj::scene3d::RobotModelLayer layer(PJ::ObjectTopicId{.id = 1}, QStringLiteral("mesh_viewer"));
  layer.setPackageResolver(&resolver);
  // Force the File source BEFORE attach so the synthetic topic id never
  // reaches the ObjectStore (the dock's local-layer rule, mirrored here).
  layer.setSourceFile(QString());
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  ctx.tf_buffer = tf;
  layer.attach(ctx);
  layer.setSourceFile(urdf_path);

  QWidget window;
  window.setWindowTitle(QStringLiteral("scene3d_mesh_viewer — %1").arg(QFileInfo(urdf_path).fileName()));
  auto* row = new QHBoxLayout(&window);
  row->setContentsMargins(0, 0, 0, 0);
  auto* view = new pj::scene3d::SceneViewWidget(&window);
  view->setTransformBuffer(tf);
  view->setAxesVisible(false);  // look-dev default: meshes only (panel toggle)
  view->setFixedFrame(root.toStdString());
  view->setLayers({&layer});
  view->setTrackerTime(PJ::fromRaw(0));
  QObject::connect(&layer, &pj::scene3d::Scene3DLayer::repaintRequested, view, qOverload<>(&QWidget::update));
  row->addWidget(view, /*stretch=*/1);
  row->addWidget(makeControls(*view));
  window.resize(1500, 840);
  window.show();
  return QApplication::exec();
}
