// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Standalone visual test for the 3D MarkerRenderPass: synthesizes a
// sdk::SceneEntities batch, decodes it, and renders it in a SceneViewWidget with
// a trivial identity TF. No ObjectStore, no parser, no app — just the render
// path. Run: ./build/pj_scene3D/demos/marker_gallery_demo

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QDockWidget>
#include <QFormLayout>
#include <QMainWindow>
#include <QPushButton>
#include <QSlider>
#include <QSurfaceFormat>
#include <QWidget>
#include <chrono>
#include <cmath>
#include <memory>
#include <numbers>
#include <utility>

#include "pj_base/builtin/scene_entities.hpp"
#include "pj_scene3d_core/scene_entities_decode.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/passes/marker_render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_scene3d_widgets/scene_view_widget.h"

namespace {

using namespace pj::scene3d;

// Minimal Scene3DLayer that wraps a MarkerRenderPass over a fixed pre-decoded
// batch. SceneViewWidget only calls initializeGL/render/releaseGL; the rest are
// trivial stubs for the demo.
class MarkerDemoEntity : public Scene3DLayer {
 public:
  explicit MarkerDemoEntity(std::shared_ptr<const DecodedSceneEntities> markers) : markers_(std::move(markers)) {}

  PJ::SceneLayerInfo info() const override {
    return {};
  }
  PJ::Range<PJ::Timepoint> timeRange() const override {
    return {PJ::Timepoint{}, PJ::Timepoint{}};
  }
  QStringList fallbackFrames() const override {
    return {};
  }
  QString sourceFrame() const override {
    return {};
  }
  QDomElement xmlSaveState(QDomDocument&) const override {
    return {};
  }
  bool xmlLoadState(const QDomElement&) override {
    return true;
  }
  bool attach(const PJ::SceneLayerContext&) override {
    return true;
  }
  void detach() override {}
  void setFixedFrame(const QString&) override {}
  void setTrackerTime(PJ::Timepoint) override {}
  void setVisible(bool) override {}
  void initializeGL() override {
    if (!gl_ready_) {
      pass_.initializeGL();
      pass_.setActive(markers_);
      gl_ready_ = true;
    }
  }
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override {
    pass_.render(view_params, frame_ctx);
  }
  void releaseGL() override {
    pass_.releaseGL();
    gl_ready_ = false;
  }
  QWidget* createConfigWidget(QWidget*) override {
    return nullptr;
  }

  // Demo-only hook: forward the editor's display overrides to the pass.
  void setDisplayOverrides(const MarkerRenderPass::DisplayOverrides& overrides) {
    pass_.setOverrides(overrides);
  }

 private:
  MarkerRenderPass pass_;
  std::shared_ptr<const DecodedSceneEntities> markers_;
  bool gl_ready_ = false;
};

PJ::sdk::CubePrimitive cube(
    double x, double y, double z, PJ::sdk::Vector3 size, PJ::sdk::ColorRGBA color,
    PJ::sdk::Quaternion q = {0.0, 0.0, 0.0, 1.0}) {
  PJ::sdk::CubePrimitive c;
  c.pose.position = {x, y, z};
  c.pose.orientation = q;
  c.size = size;
  c.color = color;
  return c;
}

PJ::sdk::SpherePrimitive sphere(double x, double y, double z, PJ::sdk::Vector3 size, PJ::sdk::ColorRGBA color) {
  PJ::sdk::SpherePrimitive s;
  s.pose.position = {x, y, z};
  s.size = size;
  s.color = color;
  return s;
}

PJ::sdk::CylinderPrimitive cylinder(
    double x, double y, double z, PJ::sdk::Vector3 size, double bottom, double top, PJ::sdk::ColorRGBA color) {
  PJ::sdk::CylinderPrimitive c;
  c.pose.position = {x, y, z};
  c.size = size;
  c.bottom_scale = bottom;
  c.top_scale = top;
  c.color = color;
  return c;
}

PJ::sdk::ArrowPrimitive arrow(
    double x, double y, double z, double len, double diam, PJ::sdk::ColorRGBA color,
    PJ::sdk::Quaternion q = {0.0, 0.0, 0.0, 1.0}) {
  PJ::sdk::ArrowPrimitive a;
  a.pose.position = {x, y, z};
  a.pose.orientation = q;
  a.shaft_length = len * 0.7;
  a.shaft_diameter = diam * 0.5;
  a.head_length = len * 0.3;
  a.head_diameter = diam;
  a.color = color;
  return a;
}

PJ::sdk::AxesPrimitive axes(double x, double y, double z, double len, double thk) {
  PJ::sdk::AxesPrimitive a;
  a.pose.position = {x, y, z};
  a.length = len;
  a.thickness = thk;
  return a;
}

PJ::sdk::LinePrimitive lineStrip(std::vector<PJ::sdk::Point3> pts, double thickness, PJ::sdk::ColorRGBA color) {
  PJ::sdk::LinePrimitive l;
  l.type = PJ::sdk::LineType::kLineStrip;
  l.points = std::move(pts);
  l.thickness = thickness;
  l.color = color;
  return l;
}

PJ::sdk::TrianglePrimitive triangleFan(std::vector<PJ::sdk::Point3> pts, PJ::sdk::ColorRGBA color) {
  PJ::sdk::TrianglePrimitive t;
  t.points = std::move(pts);
  t.color = color;
  return t;
}

// A small gallery exercising position, non-uniform size, and rotation.
std::shared_ptr<const DecodedSceneEntities> makeGallery() {
  PJ::sdk::SceneEntities batch;
  PJ::sdk::SceneEntity e;
  e.frame_id = "map";

  const double s = std::sin(std::numbers::pi / 8.0);  // 45 deg about z
  const double c = std::cos(std::numbers::pi / 8.0);
  e.cubes = {
      cube(0.0, 0.0, 0.0, {1.0, 1.0, 1.0}, {220, 60, 60, 255}),                      // red unit cube
      cube(2.5, 0.0, 0.0, {0.6, 0.6, 2.0}, {60, 200, 90, 255}),                      // green tall box
      cube(-2.5, 0.0, 0.0, {1.2, 1.2, 1.2}, {70, 120, 230, 255}, {0.0, 0.0, s, c}),  // blue, rotated 45 deg
      cube(0.0, 2.5, 0.0, {2.0, 0.5, 0.5}, {230, 200, 60, 255}),                     // yellow wide bar
  };
  e.spheres = {
      sphere(0.0, -2.5, 0.0, {1.0, 1.0, 1.0}, {200, 80, 200, 255}),  // magenta unit sphere
      sphere(2.5, -2.5, 0.0, {1.6, 0.8, 0.8}, {80, 200, 200, 255}),  // cyan ellipsoid (stretched x)
  };
  e.cylinders = {
      cylinder(-2.5, -2.5, 0.0, {1.0, 1.0, 2.0}, 1.0, 1.0, {230, 140, 40, 255}),  // orange cylinder
      cylinder(-5.0, 0.0, 0.0, {1.2, 1.2, 2.0}, 1.0, 0.0, {150, 90, 220, 255}),   // purple cone (top->0)
  };
  e.arrows = {
      arrow(0.0, 0.0, 3.0, 2.5, 0.5, {240, 120, 40, 255}),  // orange arrow pointing +X
  };
  e.axes = {
      axes(3.0, 2.5, 0.0, 1.5, 0.12),  // RGB axes glyph
  };
  e.lines = {
      lineStrip(
          {{-2.0, 4.5, 0.0}, {-1.0, 5.5, 0.0}, {0.0, 4.5, 0.0}, {1.0, 5.5, 0.0}}, 2.0,
          {255, 255, 255, 255}),  // white zigzag
  };
  e.triangles = {
      triangleFan({{-6.5, -2.0, 0.0}, {-5.0, -2.0, 0.0}, {-5.75, -0.5, 1.2}}, {255, 210, 60, 255}),  // gold tri
  };
  batch.entities = {e};
  return std::make_shared<const DecodedSceneEntities>(decodeSceneEntities(batch));
}

}  // namespace

int main(int argc, char** argv) {
  QSurfaceFormat fmt;
  fmt.setVersion(4, 5);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  QSurfaceFormat::setDefaultFormat(fmt);

  QApplication app(argc, argv);

  auto markers = makeGallery();

  // Trivial TF: an identity world<-map edge so the markers (in "map") resolve.
  auto tf = std::make_shared<TransformBuffer>();
  (void)tf->setTransform(StampedTransform{TimePoint{}, "world", "map", Transform::identity()});

  MarkerDemoEntity entity(markers);

  QMainWindow window;
  auto* view = new SceneViewWidget(&window);
  view->setTransformBuffer(tf);
  view->setFixedFrame("world");
  view->setTrackerTime(PJ::Timepoint{});
  view->setLayers({&entity});
  window.setCentralWidget(view);

  // --- Display editor (the same overrides the app's right panel would drive) ---
  MarkerRenderPass::DisplayOverrides overrides;
  QColor override_color(255, 255, 255);
  const auto apply = [&]() {
    entity.setDisplayOverrides(overrides);
    view->update();
  };

  auto* dock = new QDockWidget(QObject::tr("Display"), &window);
  auto* panel = new QWidget(dock);
  auto* form = new QFormLayout(panel);

  auto* opacity = new QSlider(Qt::Horizontal, panel);
  opacity->setRange(0, 100);
  opacity->setValue(100);
  QObject::connect(opacity, &QSlider::valueChanged, panel, [&](int v) {
    overrides.opacity = static_cast<float>(v) / 100.0F;
    apply();
  });
  form->addRow(QObject::tr("Opacity:"), opacity);

  auto* override_chk = new QCheckBox(QObject::tr("Override color"), panel);
  QObject::connect(override_chk, &QCheckBox::toggled, panel, [&](bool on) {
    overrides.color_override = on;
    apply();
  });
  form->addRow(override_chk);

  auto* color_btn = new QPushButton(QObject::tr("Pick color…"), panel);
  QObject::connect(color_btn, &QPushButton::clicked, panel, [&]() {
    const QColor picked = QColorDialog::getColor(override_color, panel, QObject::tr("Marker color"));
    if (picked.isValid()) {
      override_color = picked;
      overrides.override_color = {
          static_cast<float>(picked.redF()), static_cast<float>(picked.greenF()), static_cast<float>(picked.blueF()),
          1.0F};
      apply();
    }
  });
  form->addRow(color_btn);

  auto* wire_chk = new QCheckBox(QObject::tr("Wireframe"), panel);
  QObject::connect(wire_chk, &QCheckBox::toggled, panel, [&](bool on) {
    overrides.wireframe = on;
    apply();
  });
  form->addRow(wire_chk);

  dock->setWidget(panel);
  window.addDockWidget(Qt::RightDockWidgetArea, dock);

  window.resize(1180, 760);
  window.setWindowTitle("pj_scene3D — marker gallery (all primitives + display editor)");
  window.show();

  return app.exec();
}
