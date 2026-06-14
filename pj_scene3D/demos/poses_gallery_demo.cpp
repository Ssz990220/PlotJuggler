// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Standalone visual test for the 3D PosesRenderPass / PosesInFrameLayer triad
// gizmos: synthesizes a PJ.PosesInFrame (a small grid of yawed poses, like a
// pose-array / particle cloud), expands it with buildPoseTriadInstances, and
// renders it in a SceneViewWidget with a trivial identity TF. No ObjectStore, no
// parser, no app — just the render path.
//
// Interactive:  ./build/pj_scene3D/demos/poses_gallery_demo
// Screenshot :  ./build/pj_scene3D/demos/poses_gallery_demo --screenshot out.png [--delay-ms 1500]
//               [--size 0.4] [--opacity 1.0] [--x-only] [--override-color "r,g,b"]

#include <QApplication>
#include <QImage>
#include <QMainWindow>
#include <QSurfaceFormat>
#include <QTimer>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "pj_base/builtin/poses_in_frame.hpp"
#include "pj_scene3d_core/poses_in_frame_render.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/passes/poses_render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_scene3d_widgets/scene_view_widget.h"

namespace {

using namespace pj::scene3d;

// Minimal Scene3DLayer wrapping a PosesRenderPass over a fixed instance set, the
// same way PosesInFrameLayer drives it: resolve the source frame once, then draw.
class PosesDemoLayer : public Scene3DLayer {
 public:
  PosesDemoLayer(std::vector<PoseTriadInstance> instances, std::string frame)
      : instances_(std::move(instances)), frame_(std::move(frame)) {}

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
    return QString::fromStdString(frame_);
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
      pass_.setInstances(instances_);
      gl_ready_ = true;
    }
  }
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override {
    const auto transform = frame_ctx.lookup(frame_);
    if (transform.has_value()) {
      pass_.render(view_params, glm::mat4(transform->matrix()));
    }
  }
  void releaseGL() override {
    pass_.releaseGL();
    gl_ready_ = false;
  }
  QWidget* createConfigWidget(QWidget*) override {
    return nullptr;
  }

 private:
  PosesRenderPass pass_;
  std::vector<PoseTriadInstance> instances_;
  std::string frame_;
  bool gl_ready_ = false;
};

// A 5x5 grid of poses in "map", each yawed about +Z by a position-dependent angle
// so the triads visibly fan out (a stand-in for a PoseArray / AMCL particle set).
PJ::sdk::PosesInFrame makeGrid() {
  PJ::sdk::PosesInFrame msg;
  msg.frame_id = "map";
  for (int ix = -2; ix <= 2; ++ix) {
    for (int iy = -2; iy <= 2; ++iy) {
      PJ::sdk::Pose pose;
      pose.position = {static_cast<double>(ix) * 1.2, static_cast<double>(iy) * 1.2, 0.0};
      const double yaw = (ix + iy) * 0.30;  // radians
      pose.orientation = {0.0, 0.0, std::sin(yaw * 0.5), std::cos(yaw * 0.5)};
      msg.poses.push_back(pose);
    }
  }
  return msg;
}

struct Options {
  QString screenshot_path;
  int delay_ms = 1500;
  float size = 0.45f;
  float opacity = 1.0f;
  bool x_only = false;
  bool override_color = false;
  glm::vec3 color{0.95f, 0.30f, 0.30f};
};

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  for (int i = 1; i < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    const auto next = [&]() { return (i + 1 < argc) ? QString::fromLocal8Bit(argv[++i]) : QString(); };
    if (arg == QStringLiteral("--screenshot")) {
      opts.screenshot_path = next();
    } else if (arg == QStringLiteral("--delay-ms")) {
      opts.delay_ms = next().toInt();
    } else if (arg == QStringLiteral("--size")) {
      opts.size = next().toFloat();
    } else if (arg == QStringLiteral("--opacity")) {
      opts.opacity = next().toFloat();
    } else if (arg == QStringLiteral("--x-only")) {
      opts.x_only = true;
    } else if (arg == QStringLiteral("--override-color")) {
      const QStringList rgb = next().split(QLatin1Char(','));
      if (rgb.size() == 3) {
        opts.override_color = true;
        opts.color = {rgb[0].toFloat(), rgb[1].toFloat(), rgb[2].toFloat()};
      }
    }
  }

  QSurfaceFormat fmt;
  fmt.setVersion(4, 5);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  QSurfaceFormat::setDefaultFormat(fmt);

  QApplication app(argc, argv);

  const PJ::sdk::PosesInFrame grid = makeGrid();
  auto instances = buildPoseTriadInstances(
      grid, {
                .axis_length = opts.size,
                .opacity = opts.opacity,
                .x_arrow_only = opts.x_only,
                .override_color = opts.override_color,
                .color = opts.color,
            });

  // Trivial TF: an identity world<-map edge so the poses (in "map") resolve.
  auto tf = std::make_shared<TransformBuffer>();
  (void)tf->setTransform(StampedTransform{TimePoint{}, "world", "map", Transform::identity()});

  PosesDemoLayer layer(std::move(instances), "map");

  QMainWindow window;
  auto* view = new SceneViewWidget(&window);
  view->setTransformBuffer(tf);
  view->setFixedFrame("world");
  view->setTrackerTime(PJ::Timepoint{});
  view->setLayers({&layer});
  window.setCentralWidget(view);
  window.resize(1100, 760);
  window.setWindowTitle("pj_scene3D — PosesInFrame triad gallery");
  window.show();

  if (!opts.screenshot_path.isEmpty()) {
    QTimer::singleShot(opts.delay_ms, view, [view, &app, path = opts.screenshot_path] {
      const QImage img = view->grabFramebuffer();
      if (!img.isNull() && img.save(path)) {
        std::printf("[poses_gallery_demo] screenshot saved: %s (%dx%d)\n", qPrintable(path), img.width(), img.height());
      } else {
        std::fprintf(stderr, "[poses_gallery_demo] screenshot FAILED\n");
      }
      app.quit();
    });
  }

  return app.exec();
}
