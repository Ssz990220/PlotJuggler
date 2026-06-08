// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/scene_view_widget.h"

#include <QEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QPalette>
#include <QSurfaceFormat>
#include <QWheelEvent>
#include <algorithm>
#include <utility>

#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/gl/debug.h"
#include "pj_scene3d_widgets/render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

namespace pj::scene3d {

namespace {

QSurfaceFormat make_default_format() {
  QSurfaceFormat fmt;
  fmt.setVersion(4, 5);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  // 4x MSAA. Requested explicitly because a QOpenGLWidget renders into an FBO
  // sized by the format's concrete sample count — the default (-1, "don't
  // care") yields a single-sampled FBO. The previous native QOpenGLWindow got
  // multisampling incidentally from the platform's default visual; the FBO path
  // does not, so without this the grid/TF/pointcloud edges alias.
  fmt.setSamples(4);
  fmt.setSwapInterval(1);  // vsync — caps render at ~60Hz on standard monitors
  return fmt;
}

}  // namespace

SceneViewWidget::SceneViewWidget(QWidget* parent) : QOpenGLWidget(parent) {
  setFormat(make_default_format());
  setMinimumSize(320, 240);
  setMouseTracking(false);
}

SceneViewWidget::~SceneViewWidget() {
  // Disconnect the teardown hook first so aboutToBeDestroyed can't fire on this
  // half-destroyed object when the base QOpenGLWidget destroys the context, then
  // free GL resources while our members and the context are still alive.
  QObject::disconnect(context_cleanup_connection_);
  releaseGlResources();
}

void SceneViewWidget::setTransformBuffer(std::shared_ptr<TransformBuffer> tf) {
  if (tf_ == tf) {
    return;
  }
  tf_ = std::move(tf);
  refreshAvailableFrames();
  update();
}

void SceneViewWidget::setTrackerTime(PJ::Timepoint t) {
  if (render_time_ == t) {
    return;
  }
  render_time_ = t;
  refreshAvailableFrames();
  update();
}

void SceneViewWidget::setFixedFrame(const std::string& frame) {
  if (fixed_frame_ == frame) {
    return;
  }
  fixed_frame_ = frame;
  update();
}

void SceneViewWidget::setAxesVisible(bool visible) {
  if (axes_visible_ == visible) {
    return;
  }
  axes_visible_ = visible;
  update();
}

void SceneViewWidget::setLayers(const std::vector<Scene3DLayer*>& ordered) {
  if (layers_ == ordered) {
    return;
  }
  if (context() != nullptr) {
    makeCurrent();
    for (Scene3DLayer* layer : layers_) {
      if (layer != nullptr && std::find(ordered.begin(), ordered.end(), layer) == ordered.end()) {
        layer->releaseGL();
      }
    }
    doneCurrent();
  }
  layers_ = ordered;
  update();
}

void SceneViewWidget::refreshAvailableFrames() {
  QList<FrameRow> list;
  if (tf_) {
    auto rows = tf_->getFrameHierarchy();
    list.reserve(static_cast<qsizetype>(rows.size()));
    for (auto&& r : rows) {
      list.append(FrameRow{std::move(r.name), r.depth});
    }
  }
  if (list != last_frame_list_) {
    last_frame_list_ = list;
    emit framesChanged(list);
  }
}

void SceneViewWidget::initializeGL() {
  // Qt calls this once per GL context — on first realize and again after every
  // context recreation (QOpenGLWidget rebuilds its context when reparented by
  // ADS dock/float/split). Rewire the teardown hook to THIS context so the
  // dying context releases its own resources; releaseGlResources() already ran
  // for the previous context (via its aboutToBeDestroyed), clearing the passes'
  // initialized_ latches, so the rebuild below starts from a clean slate.
  QObject::disconnect(context_cleanup_connection_);
  context_cleanup_connection_ = connect(
      context(), &QOpenGLContext::aboutToBeDestroyed, this, &SceneViewWidget::releaseGlResources, Qt::DirectConnection);

  gl::installDebugCallback();
  axes_.initializeGL();
  grid_.initializeGL();
  overlay_.initializeGL();
  // Layer GL is initialised lazily in paintGL — layers may be added
  // dynamically after the widget is already realised, so initializing
  // here would miss late entries. releaseGlResources() reset their lazy-init
  // guards, so they rebuild on the first paint after a context recreation.
}

void SceneViewWidget::releaseGlResources() {
  // Drop every pass's and layer's GL objects with a context current so their
  // glDelete* actually run (the wrappers self-skip without a current context).
  // Called from the context's aboutToBeDestroyed (reparent or teardown) and the
  // destructor. context() is null before the first show / after full teardown.
  if (context() == nullptr) {
    return;
  }
  makeCurrent();
  axes_.releaseGL();
  grid_.releaseGL();
  overlay_.releaseGL();
  for (Scene3DLayer* layer : layers_) {
    if (layer != nullptr) {
      layer->releaseGL();
    }
  }
  doneCurrent();
}

void SceneViewWidget::resizeGL(int /*w*/, int /*h*/) {
  // Viewport is set by Qt; nothing extra to do.
}

void SceneViewWidget::paintGL() {
  auto* ctx = QOpenGLContext::currentContext();
  auto* funcs = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(ctx);
  if (funcs == nullptr) {
    return;
  }

  // Theme-aware background + grid — see Phase 1 commit (theme-aware
  // background + high-contrast grid color) for the full rationale.
  // Read the APPLICATION palette, not this widget's: QStyleSheetStyle's polish
  // rewrites widget palettes from QSS rules (the app stylesheet's transparent
  // QWidget background lands as #000000 in QPalette::Window), while the
  // application palette is kept in lockstep with the theme by pj_app's Theme.
  const QPalette pal = QGuiApplication::palette();
  const QColor window_bg = pal.color(QPalette::Window);
  const bool dark_theme = window_bg.valueF() < 0.5F;
  const QColor bg = dark_theme ? QColor(45, 48, 56) : QColor(232, 234, 238);
  const QColor fg = dark_theme ? QColor(220, 220, 220) : QColor(40, 40, 40);
  funcs->glClearColor(
      static_cast<float>(bg.redF()), static_cast<float>(bg.greenF()), static_cast<float>(bg.blueF()), 1.0f);
  constexpr float kGridBlend = 0.35f;
  grid_.setColor(
      glm::vec3{
          static_cast<float>(bg.redF() * (1.0 - kGridBlend) + fg.redF() * kGridBlend),
          static_cast<float>(bg.greenF() * (1.0 - kGridBlend) + fg.greenF() * kGridBlend),
          static_cast<float>(bg.blueF() * (1.0 - kGridBlend) + fg.blueF() * kGridBlend),
      });
  funcs->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  funcs->glEnable(GL_DEPTH_TEST);
  funcs->glEnable(GL_BLEND);
  funcs->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // The 3D viewport is opaque. The clear above set the framebuffer alpha to 1.0;
  // masking alpha writes keeps it there through every pass. RGB still blends
  // normally (src.a is the blend *factor*, not an alpha write), so transparent
  // content like the occupancy grid (opacity < 1) looks correct — but no pass,
  // present or future, can lower the framebuffer's alpha. Without this, a
  // sub-1.0 alpha left in the QOpenGLWidget's FBO makes the Wayland compositor
  // treat those regions as translucent and bleed the previous frame through them
  // (the semi-transparent "phantom" seen while zooming, absent from screenshots
  // because grabFramebuffer reads opaque RGB). Restored at the end of paintGL so
  // the next frame's glClear can repaint alpha (glClear honours the color mask).
  funcs->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);

  const float aspect = static_cast<float>(width()) / static_cast<float>(std::max(height(), 1));
  const ViewParams view_params{
      camera_->viewMatrix(),
      camera_->projMatrix(aspect),
      height(),
  };

  // Grid never consults the TF buffer; safe to render even when tf_ is null.
  static const TransformBuffer kEmptyBuffer;
  const TransformBuffer& tf_ref = tf_ ? *tf_ : kEmptyBuffer;
  // The TF-resolution triple, bundled for the passes/layers that need it.
  // fixed_frame_ is the long-lived member (no per-frame string copy).
  const FrameContext frame_ctx{tf_ref, fixed_frame_, render_time_};

  grid_.render(view_params, frame_ctx);
  if (tf_ && axes_visible_) {
    axes_.render(view_params, frame_ctx);
  }
  // Iterate layers in the order supplied by SceneDockWidget. Each layer is responsible
  // for its own GL state — initializeGL() is intentionally called per
  // frame, and layers (like the render passes they own) guard
  // against double-init via an internal `initialized_` flag. The
  // per-frame call lets layers added *after* the widget realises
  // initialise on their first paint without needing a current GL
  // context at attach time. See Scene3DLayer::initializeGL for the
  // contract.
  if (tf_) {
    for (Scene3DLayer* layer : layers_) {
      if (layer == nullptr) {
        continue;
      }
      layer->initializeGL();
      layer->render(view_params, frame_ctx);
    }
  }

  // Camera-orientation HUD (top-right by default). Drawn last so the solid
  // arrows sit on top of every scene-space pass.
  overlay_.render(view_params, frame_ctx);

  // Restore the alpha write mask so the next frame's glClear repaints alpha=1.0.
  funcs->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void SceneViewWidget::setCameraModel(CameraModel model) {
  const CameraState carried = camera_->state();
  std::unique_ptr<ICamera> next;
  switch (model) {
    case CameraModel::Orbit:
      next = std::make_unique<OrbitCamera>();
      break;
    case CameraModel::TopDownOrtho:
      next = std::make_unique<TopDownOrthoCamera>();
      break;
    case CameraModel::Fly:
      next = std::make_unique<FlyCamera>();
      break;
    case CameraModel::XYOrbit:
      next = std::make_unique<XYOrbitCamera>();
      break;
  }
  next->adoptState(carried);            // carry the pose across so the view doesn't jump
  next->setSceneBounds(scene_bounds_);  // bounds aren't part of CameraState
  camera_ = std::move(next);
  update();
}

void SceneViewWidget::setSceneBounds(const AABB& bounds) {
  scene_bounds_ = bounds;
  camera_->setSceneBounds(bounds);
}

void SceneViewWidget::mousePressEvent(QMouseEvent* event) {
  last_mouse_pos_ = event->position().toPoint();
  active_button_ = event->button();
}

void SceneViewWidget::mouseReleaseEvent(QMouseEvent* event) {
  // Clear the active-gesture latch when its button is released so a chorded
  // drag (e.g. press LMB then MMB, release MMB, keep dragging LMB) doesn't keep
  // applying the released button's gesture.
  if (event->button() == active_button_) {
    active_button_ = Qt::NoButton;
  }
}

void SceneViewWidget::mouseMoveEvent(QMouseEvent* event) {
  if (active_button_ == Qt::NoButton) {
    return;
  }
  const QPoint current = event->position().toPoint();
  const QPoint delta = current - last_mouse_pos_;
  last_mouse_pos_ = current;

  const float dx = static_cast<float>(delta.x());
  const float dy = static_cast<float>(delta.y());
  const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;

  if (active_button_ == Qt::LeftButton && !shift) {
    camera_->rotate(dx, dy);
  } else if (active_button_ == Qt::MiddleButton || (active_button_ == Qt::LeftButton && shift)) {
    camera_->pan(dx, dy);
  } else if (active_button_ == Qt::RightButton) {
    // Right-drag stays center-of-view zoom — cursor-anchoring per drag delta
    // walks the focal (focal creep); only the wheel is cursor-anchored.
    camera_->zoom(dy * 0.01f);
  }

  update();
}

void SceneViewWidget::wheelEvent(QWheelEvent* event) {
  const float ticks = static_cast<float>(event->angleDelta().y()) / 120.0f;
  const QPointF pos = event->position();
  camera_->zoomToCursor(ticks, glm::vec2{static_cast<float>(pos.x()), static_cast<float>(pos.y())}, width(), height());
  update();
}

void SceneViewWidget::changeEvent(QEvent* event) {
  if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
    update();
  }
  QOpenGLWidget::changeEvent(event);
}

}  // namespace pj::scene3d
