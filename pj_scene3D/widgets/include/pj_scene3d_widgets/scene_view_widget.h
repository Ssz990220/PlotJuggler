#pragma once

#include <QList>
#include <QOpenGLWindow>
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

class QMouseEvent;
class QWheelEvent;

namespace pj::scene3d {

class Scene3DEntity;

// Standalone Qt widget that renders the 3D scene: a grid at the fixed-frame
// origin, an XYZ axis triad per TF frame, and zero or more user entities
// (PointCloud topics, future URDF robots, etc.). Entities are non-owning
// — Scene3DDockWidget owns them and is responsible for calling addEntity
// before they're allowed to render and removeEntity before destruction.
// Implemented as a QOpenGLWindow (native GL surface) rather than a
// QOpenGLWidget: the latter renders to an FBO that Qt composites into the
// window backingstore, which re-uploads the entire raster UI (~21.5 MB) to the
// GPU every frame the view repaints. A native surface is presented directly by
// the windowing-system compositor, so the UI is not re-textured on 3D updates.
// Embedded in the widget tree via QWidget::createWindowContainer (see
// Scene3DDockWidget).
class SceneViewWidget : public QOpenGLWindow {
  Q_OBJECT

 public:
  explicit SceneViewWidget(QWindow* parent = nullptr);
  ~SceneViewWidget() override;

  void setTransformBuffer(std::shared_ptr<TransformBuffer> tf);
  void setTrackerTime(std::chrono::nanoseconds t);
  void setFixedFrame(const std::string& frame);

  // Explicit theme hint, used when the host (pj_app) applies its theme
  // via QSS rather than QPalette — QSS doesn't update palette() returns,
  // so palette-based detection picks up the unrelated OS default. The
  // host passes "light" or "dark" (anything else falls back to palette
  // luminance detection).
  void setThemeHint(const QString& theme);

  // ---- Entity registry. Entities render in insertion order (depth test
  // still wins; insertion order matters only when fragments coincide).
  // Adding the same entity twice is a no-op.
  void addEntity(Scene3DEntity* entity);
  void removeEntity(Scene3DEntity* entity);
  [[nodiscard]] bool hasEntity(const Scene3DEntity* entity) const;

  // Replace the render order with `ordered`, which must be a permutation of the
  // currently registered entities (same elements, no dupes). Index 0 renders
  // first (behind); the last renders on top — for coplanar overlays (costmaps)
  // this is what decides the overlap winner. A non-permutation is ignored.
  void reorderEntities(const std::vector<Scene3DEntity*>& ordered);

  // Entities in current render order (insertion order, or whatever
  // reorderEntities last set). Used to persist the order.
  [[nodiscard]] const std::vector<Scene3DEntity*>& entities() const {
    return entities_;
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

 private:
  // Owned passes that don't depend on the entity count.
  AxisRenderPass axes_;
  GridRenderPass grid_;
  AxisOverlayPass overlay_;

  // Non-owning entity registry, kept in insertion order so render
  // sequencing is deterministic.
  std::vector<Scene3DEntity*> entities_;

  OrbitCamera camera_;

  std::shared_ptr<TransformBuffer> tf_;
  // The time the scene renders at. Distinct from the global playhead: the dock
  // pushes a clamped time via setTrackerTime, and paint is async from ticks, so
  // this is render state, not the clock. Fed to the per-frame FrameContext.
  std::chrono::nanoseconds render_time_{0};
  std::string fixed_frame_;

  QList<FrameRow> last_frame_list_;

  QPoint last_mouse_pos_;
  Qt::MouseButton active_button_{Qt::NoButton};

  // -1 = unset (use palette luminance), 0 = light, 1 = dark.
  int theme_hint_ = -1;
};

}  // namespace pj::scene3d
