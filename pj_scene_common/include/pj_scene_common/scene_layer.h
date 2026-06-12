#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QObject>
#include <QString>
#include <QWidget>
#include <chrono>
#include <cstdint>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/Time.h"  // PJ::Timepoint, PJ::Range, fromRaw/toRaw

namespace PJ {

class SessionManager;

/// Stable snapshot of one layer's identity and presentation state.
struct SceneLayerInfo {
  ObjectTopicId topic_id;
  sdk::BuiltinObjectType object_type = sdk::BuiltinObjectType::kNone;
  QString display_name;
  QString family_name;
  bool visible = true;
};

/// Non-owning services passed to a layer when it is attached to a scene dock.
///
/// Scene families can derive from this context to expose renderer-specific
/// dependencies while keeping ISceneLayer independent from 2D/3D backends.
struct SceneLayerContext {
  SessionManager* session = nullptr;
  virtual ~SceneLayerContext() = default;
};

/// Live time range of a single ObjectStore topic, in the inverted-empty form
/// `ISceneLayer::timeRange()` expects (`{Timepoint::max(), Timepoint::min()}`
/// when `store` is null or the topic has no entries). Single-topic layers should
/// return this verbatim from `timeRange()`: it reflects the store as it stands
/// now, never a cached last-render range — a stale range pins the dock's scrub
/// clamp to an evicted time during streaming and freezes the scene.
[[nodiscard]] PJ::Range<PJ::Timepoint> liveTopicTimeRange(const ObjectStore* store, ObjectTopicId topic_id);

/// Backend-neutral contract for one object topic in a layered scene.
///
/// Implementations own the per-topic adapter state and expose changes through
/// Qt signals. A layer is attached before use, updated by the dock as tracker
/// time and visibility change, and detached before removal or destruction.
class ISceneLayer : public QObject {
  Q_OBJECT
 public:
  explicit ISceneLayer(QObject* parent = nullptr);
  ~ISceneLayer() override;

  /// Returns the current layer metadata used by dock and list UI.
  [[nodiscard]] virtual SceneLayerInfo info() const = 0;

  /// Returns the valid data range as an absolute Timepoint interval, or an
  /// inverted/empty range `{Timepoint::max(), Timepoint::min()}` if the layer
  /// carries no data (e.g. a static layer positioned purely by TF). The dock
  /// detects emptiness with `range.max < range.min`.
  ///
  /// MUST reflect the layer's data as it stands NOW — for a store-backed layer,
  /// the live ObjectStore range, never a value cached at the last render. The
  /// dock clamps the tracker to this range, so a stale range pins scrubbing to
  /// an evicted time and freezes the scene during streaming. Single-topic layers
  /// should delegate to `liveTopicTimeRange()`.
  [[nodiscard]] virtual PJ::Range<PJ::Timepoint> timeRange() const = 0;

  /// Binds the layer to shared scene services. Called once before updates.
  virtual bool attach(const SceneLayerContext& ctx) = 0;

  /// Releases resources acquired by attach(). Called before the layer is dropped.
  virtual void detach() = 0;

  /// Moves the layer to the current tracker Timepoint after dock-level clamping.
  virtual void setTrackerTime(PJ::Timepoint time) = 0;

  /// Updates visibility; implementations should emit visibilityChanged on change.
  virtual void setVisible(bool visible) = 0;

  /// Updates the fixed frame used by layers that depend on transforms.
  virtual void setFixedFrame(const QString& frame);

  /// Creates optional layer settings UI owned through the supplied Qt parent.
  virtual QWidget* createConfigWidget(QWidget* parent) = 0;

  /// Saves optional layer-specific configuration inside the dock's layer node.
  virtual QDomElement xmlSaveState(QDomDocument& doc) const;

  /// Restores optional layer-specific configuration from xmlSaveState().
  virtual bool xmlLoadState(const QDomElement& element);

 signals:
  void infoChanged();
  void visibilityChanged(bool visible);
  void repaintRequested();
  void warningChanged(bool warn, QString reason);
};

}  // namespace PJ
