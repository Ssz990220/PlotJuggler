#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QObject>
#include <QString>
#include <QStringList>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "pj_scene3d_core/camera/camera.h"  // AABB
#include "pj_scene_common/scene_layer.h"

class QWidget;

namespace PJ {
class SessionManager;
}  // namespace PJ

namespace pj::scene3d {

class TransformBuffer;
struct ViewParams;
struct FrameContext;

// Widget-global services every 3D layer needs at attach time. Scene3DDockWidget
// always passes this concrete context to Scene3DLayer instances; concrete layers
// static_cast PJ::SceneLayerContext to Scene3DLayerContext in attach() and rely
// on that invariant to retrieve the per-dataset TF buffer.
struct Scene3DLayerContext : PJ::SceneLayerContext {
  std::shared_ptr<TransformBuffer> tf_buffer;
};

// Abstract base for everything the 3D scene renders on behalf of a user-added
// topic. Generic identity/lifecycle/config/XML/visibility/repaint behavior is
// inherited from PJ::ISceneLayer; this type adds only 3D-specific frame and GL
// hooks consumed by Scene3DDockWidget and SceneViewWidget.
//
// Concrete subclasses live under
// `pj_scene3D/widgets/include/pj_scene3d_widgets/layers/`.
class Scene3DLayer : public PJ::ISceneLayer {
  Q_OBJECT
 public:
  explicit Scene3DLayer(QObject* parent = nullptr);
  ~Scene3DLayer() override;

  // Reports source frames a layer can render in if the user-picked fixed-frame
  // cannot resolve via TF.
  [[nodiscard]] virtual QStringList fallbackFrames() const = 0;

  // The primary frame this layer's data is expressed in. Used by the dock to
  // decide whether the layer can be transformed into the selected fixed frame;
  // if not, the layer is marked with a warning in the topic list.
  //
  // Concrete layer kinds choose what counts as "primary":
  //   PointCloud / LaserScan / GridMap : message header.frame_id
  //   URDF / RobotDescription          : root link frame (e.g. base_link)
  //   Marker arrays                    : message-level header.frame_id
  //                                      (per-marker frame overrides are a
  //                                      v2 refinement)
  // Return an empty string when no data has been decoded yet — the dock
  // treats that as "not warning, pending data" rather than "broken".
  [[nodiscard]] virtual QString sourceFrame() const = 0;

  // GL. initializeGL() is called by the SceneViewWidget at the top of
  // every paintGL with a current GL context. Subclasses are expected to
  // guard against double-init internally (`if (initialized_) return;`)
  // — mirroring the idiom each IRenderPass already uses. The
  // per-frame call is what lets layers added *after* the widget has
  // been realised initialise their GL state on the first paint they
  // see, without requiring the dock to hold a current context at
  // attach time. render() is called every paintGL frame; the layer
  // internally sequences its one or more IRenderPass instances. frame_ctx
  // (TF buffer + fixed frame + time) is borrowed for the call's duration —
  // layers must not store it.
  virtual void initializeGL() = 0;
  virtual void render(const ViewParams& view_params, const FrameContext& frame_ctx) = 0;

  // Drop the GL resources owned by this layer's render pass(es), returning
  // them to their pre-initializeGL state. Called by SceneViewWidget when the
  // GL context is about to be destroyed (see IRenderPass::releaseGL); the
  // layer re-initializes lazily on the next paintGL.
  virtual void releaseGL() = 0;

  // World-space (source-frame) extent of this layer's geometry, or nullopt when
  // the layer reports no bounds (no data decoded yet, or a bounds-less layer
  // kind). The dock unions these across layers to drive camera framing and
  // adaptive near/far. NON-pure so existing layer kinds need no change; concrete
  // kinds with geometry (point clouds, occupancy grids) override it.
  [[nodiscard]] virtual std::optional<AABB> worldBounds() const {
    return std::nullopt;
  }

 signals:
  // Layer noticed new source-frame candidates — the dock unions these
  // into the fixed-frame combo's fallback list.
  void fallbackFramesChanged(const QStringList& frames);

  // The value sourceFrame() would return has changed — the dock listens
  // and re-runs its orphan check (event-driven so it doesn't have to poll
  // per tracker tick).
  void sourceFrameChanged(const QString& new_frame);
};

}  // namespace pj::scene3d
