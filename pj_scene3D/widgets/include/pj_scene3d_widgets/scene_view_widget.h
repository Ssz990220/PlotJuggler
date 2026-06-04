#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QList>
#include <QOpenGLWidget>
#include <QPoint>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/camera.h"
#include "pj_scene3d_widgets/passes/axis_overlay_pass.h"
#include "pj_scene3d_widgets/passes/axis_render_pass.h"
#include "pj_scene3d_widgets/passes/grid_render_pass.h"

class QEvent;
class QMouseEvent;
class QWheelEvent;

namespace pj::scene3d {

class Scene3DLayer;

// Standalone Qt widget that renders the 3D scene: a grid at the fixed-frame
// origin, an XYZ axis triad per TF frame, and zero or more user layers
// (PointCloud topics, future URDF robots, etc.). Layers are non-owning
// — SceneDockWidget owns them and Scene3DDockWidget pushes the base-owned
// render order into this view whenever it changes.
class SceneViewWidget : public QOpenGLWidget {
  Q_OBJECT

 public:
  explicit SceneViewWidget(QWidget* parent = nullptr);
  ~SceneViewWidget() override;

  void setTransformBuffer(std::shared_ptr<TransformBuffer> tf);
  void setTrackerTime(std::chrono::nanoseconds t);
  void setFixedFrame(const std::string& frame);

  // Replace the render order: index 0 renders first (behind), the last on top.
  // For coplanar overlays (costmaps) this is what decides the overlap winner.
  void setLayers(const std::vector<Scene3DLayer*>& ordered);

  [[nodiscard]] const std::vector<Scene3DLayer*>& layers() const {
    return layers_;
  }

  AxisRenderPass& axisPass() {
    return axes_;
  }
  GridRenderPass& gridPass() {
    return grid_;
  }
  OrbitCamera& camera() {
    return camera_;
  }

  // Re-poll the TransformBuffer for the current frame set; emits
  // framesChanged if the set differs from the previous poll.
  void refreshAvailableFrames();

  const std::string& fixedFrame() const {
    return fixed_frame_;
  }

  // Show/hide the per-frame TF axis triads. The TF buffer is still used to
  // transform layers regardless — this only gates drawing the axes. Default
  // visible (TF is a first-class always-on display; see docs/REQUIREMENTS.md §4).
  void setAxesVisible(bool visible);
  [[nodiscard]] bool axesVisible() const {
    return axes_visible_;
  }

 signals:
  void framesChanged(const QList<FrameRow>& frames);

 protected:
  void initializeGL() override;
  void resizeGL(int w, int h) override;
  void paintGL() override;

  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  // Release every pass's and layer's GL resources, returning them to their
  // pre-initializeGL state. Connected to the current QOpenGLContext's
  // aboutToBeDestroyed (rewired per context in initializeGL) and also called
  // from the destructor. QOpenGLWidget recreates its context on every reparent
  // (ADS dock/float/split) and destroys it on teardown; VAOs/FBOs aren't shared
  // across contexts, so stale handles must be dropped and rebuilt.
  void releaseGlResources();

  // Owned passes that don't depend on the layer count.
  AxisRenderPass axes_;
  GridRenderPass grid_;
  AxisOverlayPass overlay_;

  // Non-owning layer registry, in the order supplied by SceneDockWidget.
  std::vector<Scene3DLayer*> layers_;

  OrbitCamera camera_;

  std::shared_ptr<TransformBuffer> tf_;
  // The time the scene renders at. Distinct from the global playhead: the dock
  // pushes a clamped time via setTrackerTime, and paint is async from ticks, so
  // this is render state, not the clock. Fed to the per-frame FrameContext.
  std::chrono::nanoseconds render_time_{0};
  std::string fixed_frame_;

  QList<FrameRow> last_frame_list_;

  // Whether the TF axis triads are drawn (see setAxesVisible). Does not affect
  // layer frame resolution, only the axes pass.
  bool axes_visible_ = true;

  QPoint last_mouse_pos_;
  Qt::MouseButton active_button_{Qt::NoButton};

  // Connection to the current GL context's aboutToBeDestroyed signal. Rewired to
  // each new context in initializeGL and disconnected in the destructor so the
  // teardown hook never fires on a half-destroyed widget.
  QMetaObject::Connection context_cleanup_connection_;
};

}  // namespace pj::scene3d
