#pragma once

#include <QObject>
#include <QString>
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

  // Discards soft-delete tombstones and rebuilds from the datastore so a
  // resurrection path (e.g. layout load) can re-expose previously removed curves.
  void resetRemovalState();

  // Restores one dataset hidden by removeDataset().
  void restoreDataset(DatasetId dataset_id);

  // Hides one dataset from the catalog and future rebuilds.
  // Emits cleared() if it empties the catalog; otherwise emits itemRemoved().
  bool removeDataset(DatasetId dataset_id);

 public slots:
  void rebuildFromDatastore();

 signals:
  void itemAdded(const CatalogItem& item);
  void itemRemoved(const QString& key);
  void cleared();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace PJ
