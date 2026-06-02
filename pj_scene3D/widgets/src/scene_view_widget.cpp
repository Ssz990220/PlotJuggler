// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/scene_view_widget.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QPalette>
#include <QSize>
#include <QStyleHints>
#include <QSurfaceFormat>
#include <QWheelEvent>
#include <algorithm>
#include <utility>

#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/gl/debug.h"
#include "pj_scene3d_widgets/render_pass.h"
#include "pj_scene3d_widgets/scene3d_entity.h"

namespace pj::scene3d {

namespace {

QSurfaceFormat make_default_format() {
  QSurfaceFormat fmt;
  fmt.setVersion(4, 5);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  fmt.setSwapInterval(1);  // vsync — caps render at ~60Hz on standard monitors
  return fmt;
}

}  // namespace

SceneViewWidget::SceneViewWidget(QWindow* parent) : QOpenGLWindow(QOpenGLWindow::NoPartialUpdate, parent) {
  setFormat(make_default_format());
  setMinimumSize(QSize(320, 240));
}

SceneViewWidget::~SceneViewWidget() = default;

void SceneViewWidget::setTransformBuffer(std::shared_ptr<TransformBuffer> tf) {
  if (tf_ == tf) {
    return;
  }
  tf_ = std::move(tf);
  refreshAvailableFrames();
  update();
}

void SceneViewWidget::setTrackerTime(std::chrono::nanoseconds t) {
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

void SceneViewWidget::setThemeHint(const QString& theme) {
  const QString lower = theme.toLower();
  if (lower.contains(QStringLiteral("light"))) {
    theme_hint_ = 0;
  } else if (lower.contains(QStringLiteral("dark"))) {
    theme_hint_ = 1;
  } else {
    theme_hint_ = -1;  // fall back to palette luminance
  }
  update();
}

void SceneViewWidget::addEntity(Scene3DEntity* entity) {
  if (entity == nullptr || hasEntity(entity)) {
    return;
  }
  // Entities own their render passes. GL init is deferred to paintGL via
  // the per-entity initializeGL guard, so we don't need a current GL
  // context here — registration is safe from any thread that owns the
  // widget.
  entities_.push_back(entity);
  update();
}

void SceneViewWidget::removeEntity(Scene3DEntity* entity) {
  if (entity == nullptr) {
    return;
  }
  entities_.erase(std::remove(entities_.begin(), entities_.end(), entity), entities_.end());
  update();
}

bool SceneViewWidget::hasEntity(const Scene3DEntity* entity) const {
  return std::find(entities_.begin(), entities_.end(), entity) != entities_.end();
}

void SceneViewWidget::reorderEntities(const std::vector<Scene3DEntity*>& ordered) {
  // Accept only an exact permutation of the current registry: same size, and
  // every current entity present exactly once. This keeps the registry's
  // membership invariant regardless of what the caller passes.
  if (ordered.size() != entities_.size()) {
    return;
  }
  for (Scene3DEntity* entity : entities_) {
    if (std::count(ordered.begin(), ordered.end(), entity) != 1) {
      return;
    }
  }
  entities_ = ordered;
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
  gl::installDebugCallback();
  axes_.initializeGL();
  grid_.initializeGL();
  overlay_.initializeGL();
  // Entity GL is initialised lazily in paintGL — entities may be added
  // dynamically after the widget is already realised, so initializing
  // here would miss late entries.
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
  // QWindow has no palette(); use the application palette for the light/dark
  // luminance fallback (the host normally drives this via setThemeHint).
  const QPalette pal = QGuiApplication::palette();
  const QColor window_bg = pal.color(QPalette::Window);
  const bool dark_theme = theme_hint_ >= 0 ? (theme_hint_ == 1) : (window_bg.valueF() < 0.5f);
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
      camera_.viewMatrix(),
      camera_.projMatrix(aspect),
      height(),
  };

  // Grid never consults the TF buffer; safe to render even when tf_ is null.
  static const TransformBuffer kEmptyBuffer;
  const TransformBuffer& tf_ref = tf_ ? *tf_ : kEmptyBuffer;
  // The TF-resolution triple, bundled for the passes/entities that need it.
  // fixed_frame_ is the long-lived member (no per-frame string copy).
  const FrameContext frame_ctx{tf_ref, fixed_frame_, render_time_};

  grid_.render(view_params, frame_ctx);
  if (tf_ && axes_visible_) {
    axes_.render(view_params, frame_ctx);
  }
  // Iterate entities in insertion order. Each entity is responsible
  // for its own GL state — initializeGL() is intentionally called per
  // frame, and entities (like the render passes they own) guard
  // against double-init via an internal `initialized_` flag. The
  // per-frame call lets entities added *after* the widget realises
  // initialise on their first paint without needing a current GL
  // context at attach time. See Scene3DEntity::initializeGL for the
  // contract.
  if (tf_) {
    for (Scene3DEntity* entity : entities_) {
      if (entity == nullptr) {
        continue;
      }
      entity->initializeGL();
      entity->render(view_params, frame_ctx);
    }
  }

  // Camera-orientation HUD (top-right by default). Drawn last so the solid
  // arrows sit on top of every scene-space pass.
  overlay_.render(view_params, frame_ctx);

  // Restore the alpha write mask so the next frame's glClear repaints alpha=1.0.
  funcs->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void SceneViewWidget::mousePressEvent(QMouseEvent* event) {
  last_mouse_pos_ = event->position().toPoint();
  press_pos_ = last_mouse_pos_;
  dragged_since_press_ = false;
  active_button_ = event->button();
}

void SceneViewWidget::mouseReleaseEvent(QMouseEvent* event) {
  // A right-button release with no intervening drag is a context-menu click;
  // a right-*drag* already zoomed the camera (mouseMoveEvent) and must not also
  // pop a menu. The dock builds the actual QMenu (see contextMenuRequested).
  if (event->button() == Qt::RightButton && !dragged_since_press_) {
    emit contextMenuRequested(event->globalPosition().toPoint());
  }
  // Clear the drag state on release. As a QOpenGLWidget this was masked (moves
  // only arrived while a button was held); as a native QOpenGLWindow the press
  // grabs the mouse and moves keep arriving, so without this the drag never
  // ends and the surface holds the grab — blocking clicks elsewhere.
  if (event->button() == active_button_) {
    active_button_ = Qt::NoButton;
  }
}

void SceneViewWidget::mouseMoveEvent(QMouseEvent* event) {
  if (active_button_ == Qt::NoButton) {
    return;
  }
  const QPoint current = event->position().toPoint();
  // Latch a drag once the cursor leaves the platform drag threshold, so a
  // right release past this point is treated as a zoom, not a menu click.
  if (!dragged_since_press_ &&
      (current - press_pos_).manhattanLength() > QGuiApplication::styleHints()->startDragDistance()) {
    dragged_since_press_ = true;
  }
  const QPoint delta = current - last_mouse_pos_;
  last_mouse_pos_ = current;

  const float dx = static_cast<float>(delta.x());
  const float dy = static_cast<float>(delta.y());
  const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;

  if (active_button_ == Qt::LeftButton && !shift) {
    camera_.rotate(dx, dy);
  } else if (active_button_ == Qt::MiddleButton || (active_button_ == Qt::LeftButton && shift)) {
    camera_.pan(dx, dy);
  } else if (active_button_ == Qt::RightButton) {
    camera_.zoom(dy * 0.01f);
  }

  update();
}

void SceneViewWidget::wheelEvent(QWheelEvent* event) {
  const float ticks = static_cast<float>(event->angleDelta().y()) / 120.0f;
  camera_.zoom(ticks);
  update();
}

}  // namespace pj::scene3d
