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
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"

class QWidget;

namespace PJ {
class SessionManager;
}  // namespace PJ

namespace pj::scene3d {

class TransformBuffer;
struct ViewParams;
struct FrameContext;

// Identity + UI metadata that Scene3DDockWidget and Scene3DConfigPanel need
// without touching the concrete entity subclass. An entity is one
// orchestrated 3D thing (today: one PointCloud2 topic; later: one URDF
// robot, one gridmap, one marker stream...).
struct Scene3DEntityInfo {
  PJ::ObjectTopicId topic_id;
  PJ::sdk::BuiltinObjectType object_type = PJ::sdk::BuiltinObjectType::kNone;
  QString display_name;  // shown in the topic list
  QString family_name;   // e.g. "PointCloud", "URDF" — for tooltips / filters
  bool visible = true;
};

// Widget-global services every entity needs at attach time. The dock owns
// these (session is the app's SessionManager; tf_buffer is borrowed from
// the same session per-dataset) and hands a snapshot to each entity so
// the entity doesn't reach back into the dock.
struct Scene3DEntityContext {
  PJ::SessionManager* session = nullptr;
  std::shared_ptr<TransformBuffer> tf_buffer;
};

// Abstract base for everything the 3D scene renders on behalf of a user-
// added topic. The dock holds a `std::unique_ptr<Scene3DEntity>` per
// topic, drives lifecycle (attach/detach), pushes widget-global state
// (fixed-frame / tracker time / visibility), and calls render() from
// SceneViewWidget::paintGL. Per-instance parametrization lives entirely
// inside the entity — surfaced to the right-sidepanel by
// createConfigWidget(), which builds a fresh QWidget on demand.
//
// Concrete subclasses live under
// `pj_scene3D/widgets/include/pj_scene3d_widgets/entities/`.
class Scene3DEntity : public QObject {
  Q_OBJECT
 public:
  explicit Scene3DEntity(QObject* parent = nullptr);
  ~Scene3DEntity() override;

  // Identity / introspection. info() is the single source of truth for
  // the topic list row (name, visibility, family). timeRangeNs() lets
  // the dock clamp the tracker against the union of all entities'
  // ranges. fallbackFrames() reports source frames an entity can
  // render in if the user-picked fixed-frame can't resolve via TF.
  [[nodiscard]] virtual Scene3DEntityInfo info() const = 0;
  [[nodiscard]] virtual std::pair<int64_t, int64_t> timeRangeNs() const = 0;
  [[nodiscard]] virtual QStringList fallbackFrames() const = 0;

  // The primary frame this entity's data is expressed in. Used by the dock
  // to decide whether the entity can be transformed into the currently
  // selected fixed frame; if not, the entity is "orphan" and the topic-list
  // row is flagged red with a tooltip explaining why.
  //
  // Concrete entity kinds choose what counts as "primary":
  //   PointCloud / LaserScan / GridMap : message header.frame_id
  //   URDF / RobotDescription          : root link frame (e.g. base_link)
  //   Marker arrays                    : message-level header.frame_id
  //                                      (per-marker frame overrides are a
  //                                      v2 refinement)
  // Return an empty string when no data has been decoded yet — the dock
  // treats that as "not orphan, pending data" rather than "broken".
  [[nodiscard]] virtual QString sourceFrame() const = 0;
  virtual QDomElement xmlSaveState(QDomDocument& doc) const = 0;
  virtual bool xmlLoadState(const QDomElement& element) = 0;

  // Lifecycle. attach() is called once with the widget-global services;
  // it must return false if the entity can't function (no parser for
  // the topic, missing TF, etc.) — the dock will discard it. detach()
  // releases any GL resources owned by the entity and unbinds from the
  // view.
  virtual bool attach(const Scene3DEntityContext& ctx) = 0;
  virtual void detach() = 0;

  // Widget-global state pushed from the dock when it changes. The
  // entity uses these to invalidate caches (range_dirty on fixed-frame
  // change), trigger re-decode (tracker time), or skip rendering
  // (visibility=false).
  virtual void setFixedFrame(const QString& frame) = 0;
  virtual void setTrackerTime(std::chrono::nanoseconds time) = 0;
  virtual void setVisible(bool visible) = 0;

  // GL. initializeGL() is called by the SceneViewWidget at the top of
  // every paintGL with a current GL context. Subclasses are expected to
  // guard against double-init internally (`if (initialized_) return;`)
  // — mirroring the idiom each IRenderPass already uses. The
  // per-frame call is what lets entities added *after* the widget has
  // been realised initialise their GL state on the first paint they
  // see, without requiring the dock to hold a current context at
  // attach time. render() is called every paintGL frame; the entity
  // internally sequences its one or more IRenderPass instances. frame_ctx
  // (TF buffer + fixed frame + time) is borrowed for the call's duration —
  // entities must not store it.
  virtual void initializeGL() = 0;
  virtual void render(const ViewParams& view_params, const FrameContext& frame_ctx) = 0;

  // Per-instance parameter UI. The right-sidepanel calls this when the
  // entity is selected and inserts the returned widget into its params
  // container. Ownership transfers to `parent`; the entity does not
  // retain a pointer (the widget is destroyed when the panel clears
  // its container on the next selection). createConfigWidget() may
  // return nullptr if the entity exposes no per-instance parameters.
  virtual QWidget* createConfigWidget(QWidget* parent) = 0;

 signals:
  // Emitted when info() returns a different display_name / family_name
  // or any field other than visibility (which has its own signal). The
  // dock listens and re-renders the topic-list row.
  void infoChanged();

  // Visibility is broken out so the panel can update its eye toggle
  // without a full row rebuild.
  void visibilityChanged(bool visible);

  // Entity noticed new source-frame candidates — the dock unions these
  // into the fixed-frame combo's fallback list.
  void fallbackFramesChanged(const QStringList& frames);

  // The value sourceFrame() would return has changed — the dock listens
  // and re-runs its orphan check (event-driven so it doesn't have to poll
  // per tracker tick).
  void sourceFrameChanged(const QString& new_frame);

  // Entity wants a paint event. Bound to SceneViewWidget::update via
  // the dock so entities don't have a direct pointer to the view.
  void repaintRequested();
};

}  // namespace pj::scene3d
