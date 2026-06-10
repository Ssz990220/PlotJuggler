#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <QStringList>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/CurveDescriptor.h"

namespace PJ {

class SessionManager;

// Scalar-field payload: numeric series read from the data engine.
struct ScalarFieldPayload {
  QString field_name;
  QString field_path;
  TopicId topic_id = 0;
  std::size_t column_index = 0;
  Timestamp display_offset_ns = 0;
};

// Object-topic payload: time-indexed canonical-object stream from the object
// store (e.g. images, depth, image annotations).
struct ObjectTopicPayload {
  ObjectTopicId object_topic_id;
  sdk::BuiltinObjectType object_type = sdk::BuiltinObjectType::kNone;
  QString metadata_json;
};

// A single entry in the catalog. The variant payload statically separates
// scalar-field state from object-topic state so consumers cannot accidentally
// read object fields off a scalar entry (or vice versa) — the previous flat
// struct had per-variant dead fields that bit-rotted silently.
struct CatalogItem {
  QString key;  // Opaque catalog key, not a display path.
  QString dataset_name;
  QString topic_name;
  DatasetId dataset_id;
  std::variant<ScalarFieldPayload, ObjectTopicPayload> payload;
};

// True for canonical object types the 2D scene module (pj_scene2D) can display
// in a Scene2DDockWidget: raw/compressed images, single video frames, image
// overlays, and depth images. Used by catalog/tree views to mark a topic as
// droppable into a 2D view (icon + drop routing), keeping the
// "what is scene2D-displayable" policy in one place instead of scattered
// per-type == checks. Kept in sync with Scene2DDockWidget::setImageTopic.
[[nodiscard]] inline bool isScene2DDisplayable(sdk::BuiltinObjectType object_type) noexcept {
  switch (object_type) {
    case sdk::BuiltinObjectType::kImage:
    case sdk::BuiltinObjectType::kVideoFrame:
    case sdk::BuiltinObjectType::kImageAnnotations:
    case sdk::BuiltinObjectType::kDepthImage:
      return true;
    default:
      return false;
  }
}

// Convenience accessors. Prefer these over std::get_if at call sites.
[[nodiscard]] inline bool isScalarField(const CatalogItem& item) noexcept {
  return std::holds_alternative<ScalarFieldPayload>(item.payload);
}
[[nodiscard]] inline bool isObjectTopic(const CatalogItem& item) noexcept {
  return std::holds_alternative<ObjectTopicPayload>(item.payload);
}
[[nodiscard]] inline const ScalarFieldPayload* asScalarField(const CatalogItem& item) noexcept {
  return std::get_if<ScalarFieldPayload>(&item.payload);
}
[[nodiscard]] inline const ObjectTopicPayload* asObjectTopic(const CatalogItem& item) noexcept {
  return std::get_if<ObjectTopicPayload>(&item.payload);
}

// Qt-side facade over the catalog of topics/curves known to the current
// session. Populated as data sources load; GUI views (CurveListPanel,
// catalog trees in widget families) subscribe to the add/remove signals.
class CatalogModel : public QObject {
  Q_OBJECT
 public:
  using Ptr = std::shared_ptr<CatalogModel>;

  explicit CatalogModel(SessionManager* session = nullptr, QObject* parent = nullptr);
  ~CatalogModel() override;

  CatalogModel(const CatalogModel&) = delete;
  CatalogModel& operator=(const CatalogModel&) = delete;

  std::vector<CatalogItem> items() const;
  // Fast alternative to items().empty(), which copies and sorts entries.
  [[nodiscard]] bool isEmpty() const noexcept;
  [[nodiscard]] std::optional<CatalogItem> itemDescriptor(const QString& key) const;
  std::vector<CurveDescriptor> curves() const;
  [[nodiscard]] std::optional<CurveDescriptor> curveDescriptor(const QString& key) const;

  // Loaded datasets as (id, display name) pairs, ordered by load (dataset id
  // ascending). Derived from current catalog contents.
  [[nodiscard]] std::vector<std::pair<DatasetId, QString>> datasets() const;

  // Resolves a stable topic+field path to the matching scalar curve within a
  // specific dataset. Lets a layout rebind across similar datasets where the
  // opaque per-load key differs but the topic/field path is identical.
  [[nodiscard]] std::optional<CurveDescriptor> descriptorForPath(
      DatasetId dataset_id, const QString& topic, const QString& field) const;

  void clearAll();
  void removeItems(const std::vector<QString>& keys);
  void removeCurves(const std::vector<QString>& keys);

  // Tombstones one dataset's items (hidden from future rebuildFromDatastore;
  // scalar data stays in the DataEngine). Emits cleared() if it empties the
  // catalog, else one batched itemsRemoved; returns false (no-op) if the dataset
  // has no items. Pure catalog op — media eviction is the caller's job (shell
  // evicts at confirmed-removal sites), so it stays safe for speculative/rollback
  // use. Undo via restoreDataset(); a reload mints a fresh DatasetId.
  bool removeDataset(DatasetId dataset_id);

  // Discards soft-delete tombstones and rebuilds from the datastore so a
  // resurrection path (e.g. layout load) can re-expose previously removed curves.
  void resetRemovalState();

  // Restores one dataset hidden by removeDataset().
  void restoreDataset(DatasetId dataset_id);

  // Overrides the tree-root label of a dataset, replacing the file-derived
  // source_name shown in catalog/tree views (issue #98 — lets a DataSource name
  // its dataset instead of inheriting the opened file's basename). Keyed by the
  // stable DatasetId so the override survives rebuildFromDatastore and
  // clearAll/restoreDataset. The override is normalized ('/'→'_') and
  // participates in duplicate-label ordinal disambiguation, exactly like a
  // source_name-derived label. An empty string clears the override (falls back
  // to source_name). Does not touch the engine's immutable DatasetInfo, so
  // dataset-reuse matching (by source_name) is unaffected.
  void setDatasetDisplayName(DatasetId dataset_id, const QString& display_name);

 public slots:
  void rebuildFromDatastore();

 signals:
  // Batched companion to itemAdded(), emitted once per rebuild so views can
  // update/sort once even when many catalog entries appear at the same time.
  void itemsAdded(const std::vector<CatalogItem>& items);
  void itemAdded(const CatalogItem& item);
  // One emission per removal operation (whole dataset, multi-key trash, or keys
  // that vanished on a rebuild) so consumers react once, not per key.
  void itemsRemoved(const QStringList& keys);
  void cleared();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace PJ
