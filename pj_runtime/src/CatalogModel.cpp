// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/CatalogModel.h"

#include <fmt/format.h>
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>

#include <QHash>
#include <QLoggingCategory>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_datastore/column_buffer.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace PJ {
namespace {

Q_LOGGING_CATEGORY(lcCatalog, "pj.runtime.catalog")

struct QStringHash {
  [[nodiscard]] std::size_t operator()(const QString& value) const noexcept {
    return qHash(value);
  }
};

[[nodiscard]] bool isCatalogNumeric(PrimitiveType type) noexcept {
  switch (type) {
    case PrimitiveType::kFloat32:
    case PrimitiveType::kFloat64:
    case PrimitiveType::kInt32:
    case PrimitiveType::kInt64:
    case PrimitiveType::kUint64:
    case PrimitiveType::kBool:
      return true;
    case PrimitiveType::kInt8:
    case PrimitiveType::kInt16:
    case PrimitiveType::kUint8:
    case PrimitiveType::kUint16:
    case PrimitiveType::kUint32:
    case PrimitiveType::kString:
    case PrimitiveType::kUnspecified:
      return false;
  }
  return false;
}

[[nodiscard]] QString baseDatasetLabel(const DatasetInfo* dataset) {
  QString label = dataset != nullptr ? QString::fromStdString(dataset->source_name) : QString{};
  if (label.isEmpty()) {
    label = QStringLiteral("Dataset");
  }
  label.replace('/', '_');
  return label;
}

[[nodiscard]] QString makeCurveKey(DatasetId dataset_id, TopicId topic_id, std::size_t column_index) {
  return QStringLiteral("dataset:%1/topic:%2/column:%3").arg(dataset_id).arg(topic_id).arg(column_index);
}

[[nodiscard]] QString makeObjectTopicKey(DatasetId dataset_id, ObjectTopicId object_topic_id) {
  return QStringLiteral("dataset:%1/object_topic:%2").arg(dataset_id).arg(object_topic_id.id);
}

[[nodiscard]] CurveDescriptor curveFromItem(const CatalogItem& item) {
  const auto* scalar = asScalarField(item);
  Q_ASSERT(scalar != nullptr);  // Precondition: caller verified isScalarField(item).
  return CurveDescriptor{
      .name = item.key,
      .dataset_name = item.dataset_name,
      .topic_name = item.topic_name,
      .field_name = scalar->field_name,
      .topic_id = scalar->topic_id,
      .dataset_id = item.dataset_id,
      .column_index = scalar->column_index,
      .field_path = scalar->field_path,
  };
}

[[nodiscard]] bool catalogItemLess(const CatalogItem& lhs, const CatalogItem& rhs) {
  if (lhs.dataset_id != rhs.dataset_id) {
    return lhs.dataset_id < rhs.dataset_id;
  }
  const int topic_compare = QString::localeAwareCompare(lhs.topic_name, rhs.topic_name);
  if (topic_compare != 0) {
    return topic_compare < 0;
  }
  // ObjectTopic sorts after ScalarField within the same topic (the variant
  // index gives a deterministic order: ScalarFieldPayload=0, ObjectTopic=1).
  if (lhs.payload.index() != rhs.payload.index()) {
    return lhs.payload.index() < rhs.payload.index();
  }
  if (const auto* l = asScalarField(lhs)) {
    const auto* r = asScalarField(rhs);  // index match implies r is non-null
    if (l->column_index != r->column_index) {
      return l->column_index < r->column_index;
    }
  }
  return lhs.key < rhs.key;
}

void collectTypeTreeLeaves(
    const TypeTreeNode& node, const std::string& prefix, std::size_t& next_column,
    std::vector<ColumnDescriptor>& columns) {
  const std::string current_path = prefix.empty() ? node.name : prefix + "." + node.name;

  if (node.kind == TypeKind::kStruct) {
    for (const auto& child : node.children) {
      collectTypeTreeLeaves(*child, current_path, next_column, columns);
    }
    return;
  }

  if (node.kind == TypeKind::kArray) {
    if (node.element_type && node.fixed_array_size.has_value()) {
      for (uint32_t i = 0; i < *node.fixed_array_size; ++i) {
        const std::string element_path = fmt::format("{}[{}]", current_path, i);
        if (node.element_type->kind == TypeKind::kStruct) {
          for (const auto& child : node.element_type->children) {
            collectTypeTreeLeaves(*child, element_path, next_column, columns);
          }
        } else {
          columns.push_back(
              ColumnDescriptor{
                  .field_id = static_cast<FieldId>(next_column),
                  .logical_type = node.element_type->primitive_type.value_or(PrimitiveType::kFloat64),
                  .field_path = element_path,
              });
          ++next_column;
        }
      }
    }
    return;
  }

  columns.push_back(
      ColumnDescriptor{
          .field_id = static_cast<FieldId>(next_column),
          .logical_type = node.primitive_type.value_or(PrimitiveType::kFloat64),
          .field_path = current_path,
      });
  ++next_column;
}

[[nodiscard]] std::vector<ColumnDescriptor> columnsFromTypeTree(const TypeTreeNode& root) {
  std::vector<ColumnDescriptor> columns;
  std::size_t next_column = 0;
  if (root.kind == TypeKind::kStruct) {
    for (const auto& child : root.children) {
      collectTypeTreeLeaves(*child, "", next_column, columns);
    }
  } else {
    collectTypeTreeLeaves(root, "", next_column, columns);
  }
  return columns;
}

[[nodiscard]] std::vector<ColumnDescriptor> topicColumns(const TopicStorage& storage, const TypeTreeNode* type_tree) {
  if (!storage.columnDescriptors().empty()) {
    return storage.columnDescriptors();
  }

  const auto& chunks = storage.sealedChunks();
  if (!chunks.empty()) {
    std::vector<ColumnDescriptor> columns;
    columns.reserve(chunks.front().columns.size());
    for (const auto& column : chunks.front().columns) {
      if (column.descriptor) {
        columns.push_back(*column.descriptor);
      }
    }
    return columns;
  }

  if (type_tree != nullptr) {
    return columnsFromTypeTree(*type_tree);
  }

  return {};
}

}  // namespace

sdk::BuiltinObjectType objectTypeFromMetadata(std::string_view metadata_json) {
  if (metadata_json.empty()) {
    return sdk::BuiltinObjectType::kNone;
  }
  try {
    const auto metadata = nlohmann::json::parse(metadata_json);
    const auto it = metadata.find("builtin_object_type");
    if (it == metadata.end()) {
      qCWarning(lcCatalog) << "objectTypeFromMetadata: missing 'builtin_object_type' key — metadata="
                           << QString::fromUtf8(metadata_json.data(), static_cast<int>(metadata_json.size()));
      return sdk::BuiltinObjectType::kNone;
    }
    if (!it->is_string()) {
      qCWarning(lcCatalog) << "objectTypeFromMetadata: 'builtin_object_type' is not a string — metadata="
                           << QString::fromUtf8(metadata_json.data(), static_cast<int>(metadata_json.size()));
      return sdk::BuiltinObjectType::kNone;
    }
    const auto raw = it->get<std::string>();
    const auto parsed = sdk::parseBuiltinObjectType(raw);
    if (!parsed.has_value()) {
      qCWarning(lcCatalog) << "objectTypeFromMetadata: unknown builtin_object_type='" << QString::fromStdString(raw)
                           << "' — topic will be hidden from object-aware views";
      return sdk::BuiltinObjectType::kNone;
    }
    return *parsed;
  } catch (const nlohmann::json::exception& e) {
    qCWarning(lcCatalog) << "objectTypeFromMetadata: JSON parse failed:" << e.what() << "metadata="
                         << QString::fromUtf8(metadata_json.data(), static_cast<int>(metadata_json.size()));
    return sdk::BuiltinObjectType::kNone;
  }
}

struct CatalogModel::Impl {
  explicit Impl(SessionManager* session_in) : session(session_in) {}

  using ItemMap = tsl::robin_map<QString, CatalogItem, QStringHash>;
  using NameSet = tsl::robin_set<QString, QStringHash>;

  SessionManager* session = nullptr;
  ItemMap items;
  // Datasets hidden by clearAll: rebuildFromDatastore skips them entirely.
  // A reload of the same file creates a fresh DatasetId not in this set, so
  // the curves come back.
  tsl::robin_set<DatasetId> removed_datasets;
  // Per-dataset name blacklist for selective removal via removeCurves. Keyed
  // by the DatasetId the curve belonged to, so a reload (new DatasetId)
  // re-introduces the name.
  tsl::robin_map<DatasetId, NameSet> removed_names_per_dataset;
  // Plugin-provided tree-root labels (issue #98), keyed by stable DatasetId so
  // they survive rebuilds and clearAll/restoreDataset. Never holds empty values.
  tsl::robin_map<DatasetId, QString> dataset_display_overrides;

  // Content fingerprint as of the last completed rebuildFromDatastore(), used by
  // the samplesIngested gate (rebuildIfChanged) to skip redundant full rebuilds.
  // Unset until the first rebuild, so the first ingest always rebuilds.
  std::optional<std::uint64_t> last_fingerprint;
};

CatalogModel::CatalogModel(SessionManager* session, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(session)) {
  if (impl_->session != nullptr) {
    connect(impl_->session, &SessionManager::samplesIngested, this, &CatalogModel::rebuildIfChanged);
  }
}

CatalogModel::~CatalogModel() = default;

std::vector<CatalogItem> CatalogModel::items() const {
  std::vector<CatalogItem> items;
  items.reserve(impl_->items.size());
  for (const auto& [key, item] : impl_->items) {
    (void)key;
    items.push_back(item);
  }
  std::sort(items.begin(), items.end(), catalogItemLess);
  return items;
}

bool CatalogModel::isEmpty() const noexcept {
  return impl_->items.empty();
}

std::optional<CatalogItem> CatalogModel::itemDescriptor(const QString& key) const {
  const auto it = impl_->items.find(key);
  if (it == impl_->items.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<double> CatalogModel::scalarValueAt(const QString& key, double display_seconds) const {
  if (impl_->session == nullptr) {
    return std::nullopt;
  }
  const auto it = impl_->items.find(key);
  if (it == impl_->items.end()) {
    return std::nullopt;
  }
  const ScalarFieldPayload* scalar = asScalarField(it->second);
  if (scalar == nullptr || scalar->is_string) {
    return std::nullopt;  // object topics carry no scalar value; strings → stringValueAt
  }
  // Display-axis seconds -> raw ns through this dataset's display offset, then a
  // null-aware latest-at-or-before point query (zero-order hold) read under the
  // engine lock. latestNumericAt distinguishes a null cell from a real 0, so a
  // sparsely-populated field shows "-" rather than a fabricated "0.000".
  const DisplayOffset offset = impl_->session->displayOffset(it->second.dataset_id);
  const Timestamp raw_ns = displaySecondsToRaw(DisplaySeconds{display_seconds}, offset);
  const DataReader reader(impl_->session->dataEngine());
  const auto value_or =
      reader.latestNumericAt(QueryPoint{.topic_id = scalar->topic_id, .t = raw_ns}, scalar->column_index);
  if (!value_or.has_value() || !value_or->has_value()) {
    return std::nullopt;  // read error, no row at/before the query time, or null cell
  }
  return **value_or;
}

bool CatalogModel::isScalarKey(const QString& key) const {
  const auto it = impl_->items.find(key);
  return it != impl_->items.end() && isScalarField(it->second);
}

bool CatalogModel::isStringKey(const QString& key) const {
  const auto it = impl_->items.find(key);
  if (it == impl_->items.end()) {
    return false;
  }
  const ScalarFieldPayload* scalar = asScalarField(it->second);
  return scalar != nullptr && scalar->is_string;
}

std::optional<QString> CatalogModel::stringValueAt(const QString& key, double display_seconds) const {
  if (impl_->session == nullptr) {
    return std::nullopt;
  }
  const auto it = impl_->items.find(key);
  if (it == impl_->items.end()) {
    return std::nullopt;
  }
  const ScalarFieldPayload* scalar = asScalarField(it->second);
  if (scalar == nullptr || !scalar->is_string) {
    return std::nullopt;  // only string fields carry a string value
  }
  const DisplayOffset offset = impl_->session->displayOffset(it->second.dataset_id);
  const Timestamp raw_ns = displaySecondsToRaw(DisplaySeconds{display_seconds}, offset);
  const DataReader reader(impl_->session->dataEngine());
  const auto value_or =
      reader.latestStringAt(QueryPoint{.topic_id = scalar->topic_id, .t = raw_ns}, scalar->column_index);
  if (!value_or.has_value() || !value_or->has_value()) {
    return std::nullopt;  // read error, or no row at/before the query time
  }
  return QString::fromStdString(**value_or);
}

std::vector<CurveDescriptor> CatalogModel::curves() const {
  std::vector<CurveDescriptor> curves;
  curves.reserve(impl_->items.size());
  for (const auto& [key, item] : impl_->items) {
    (void)key;
    // String fields are catalog items but not plottable curves.
    if (const ScalarFieldPayload* scalar = asScalarField(item); scalar != nullptr && !scalar->is_string) {
      curves.push_back(curveFromItem(item));
    }
  }
  std::sort(curves.begin(), curves.end(), [](const CurveDescriptor& lhs, const CurveDescriptor& rhs) {
    if (lhs.dataset_id != rhs.dataset_id) {
      return lhs.dataset_id < rhs.dataset_id;
    }
    if (lhs.topic_id != rhs.topic_id) {
      return lhs.topic_id < rhs.topic_id;
    }
    return lhs.column_index < rhs.column_index;
  });
  return curves;
}

std::optional<CurveDescriptor> CatalogModel::curveDescriptor(const QString& key) const {
  const auto it = impl_->items.find(key);
  if (it == impl_->items.end()) {
    return std::nullopt;
  }
  // String fields are not plottable curves, so they have no CurveDescriptor.
  const ScalarFieldPayload* scalar = asScalarField(it->second);
  if (scalar == nullptr || scalar->is_string) {
    return std::nullopt;
  }
  return curveFromItem(it->second);
}

std::vector<std::pair<DatasetId, QString>> CatalogModel::datasets() const {
  std::vector<std::pair<DatasetId, QString>> result;
  tsl::robin_set<DatasetId> seen;
  for (const auto& [key, item] : impl_->items) {
    (void)key;
    if (seen.insert(item.dataset_id).second) {
      result.emplace_back(item.dataset_id, item.dataset_name);
    }
  }
  std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  return result;
}

std::optional<CurveDescriptor> CatalogModel::descriptorForPath(
    DatasetId dataset_id, const QString& topic, const QString& field) const {
  for (const auto& [key, item] : impl_->items) {
    (void)key;
    if (item.dataset_id != dataset_id || item.topic_name != topic || !isScalarField(item)) {
      continue;
    }
    if (asScalarField(item)->field_path == field) {
      return curveFromItem(item);
    }
  }
  return std::nullopt;
}

void CatalogModel::rebuildFromDatastore() {
  rebuildNow();
  // Keep the samplesIngested gate's cache in sync after EVERY rebuild — whether
  // triggered by the gate or by an explicit caller (load completion, dataset
  // removal, display-name change) — so the next ingest can correctly skip.
  impl_->last_fingerprint = catalogFingerprint();
}

void CatalogModel::rebuildIfChanged() {
  // Gate for the high-frequency samplesIngested path: a full rebuild re-scans and
  // re-allocates every curve key/label, but most ingest batches only append rows.
  // Skip when nothing that affects catalog contents changed since the last
  // rebuild. Stays synchronous (no timer), so callers that read the catalog right
  // after a structural commit still see it updated.
  const std::uint64_t fp = catalogFingerprint();
  if (impl_->last_fingerprint == fp) {
    return;
  }
  rebuildFromDatastore();
}

std::uint64_t CatalogModel::catalogFingerprint() const {
  // Order-independent accumulation (sum of well-mixed per-element hashes) so it
  // does not depend on list*() enumeration order. Allocation-light: only the
  // small vectors returned by list*(); no per-column QString building.
  const auto mix = [](std::uint64_t tag, std::uint64_t value) -> std::uint64_t {
    std::uint64_t x = (tag * 1099511628211ULL) ^ (value + 0x9E3779B97F4A7C15ULL);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    return x;
  };
  std::uint64_t fp = 0;

  if (impl_->session != nullptr) {
    const DataReader reader = impl_->session->createReader();
    DataEngine& engine = impl_->session->dataEngine();
    ObjectStore& object_store = impl_->session->objectStore();
    // Ephemeral (preview) transform outputs are excluded from the catalog, so they
    // must not move the fingerprint either — otherwise each preview keystroke (which
    // mints a fresh output topic id) would trip a full catalog rebuild.
    const std::unordered_set<TopicId> ephemeral_outputs =
        impl_->session->dataProcessorService().ephemeralOutputTopics();
    for (const DatasetId dataset_id : reader.listDatasets()) {
      fp += mix(1, dataset_id);
      {
        // getTopicStorage() is raw + non-locking; hold the engine lock for the
        // scan. The object store has an INDEPENDENT lock that must never nest
        // with the engine lock, so it is queried after this scope closes.
        const auto lock = engine.lockEngine();
        for (const TopicId topic_id : reader.listTopics(dataset_id)) {
          if (ephemeral_outputs.count(topic_id) > 0) {
            continue;
          }
          std::uint64_t columns = 0;
          if (const TopicStorage* storage = engine.getTopicStorage(topic_id); storage != nullptr) {
            columns = storage->columnDescriptors().size();
          }
          fp += mix(2, (static_cast<std::uint64_t>(topic_id) << 20) ^ columns);
        }
      }
      for (const ObjectTopicId object_topic_id : object_store.listTopics(dataset_id)) {
        fp += mix(3, object_topic_id.id);
      }
    }
  }

  // Tombstones + display overrides also change catalog output, and some mutators
  // (clearAll, removeDataset) change them WITHOUT calling rebuildFromDatastore —
  // folding them in keeps the gate self-correcting regardless of the trigger.
  for (const DatasetId dataset_id : impl_->removed_datasets) {
    fp += mix(4, dataset_id);
  }
  for (const auto& [dataset_id, names] : impl_->removed_names_per_dataset) {
    fp += mix(5, dataset_id);
    for (const QString& name : names) {
      fp += mix(6, (static_cast<std::uint64_t>(dataset_id) << 32) ^ qHash(name));
    }
  }
  for (const auto& [dataset_id, label] : impl_->dataset_display_overrides) {
    fp += mix(7, (static_cast<std::uint64_t>(dataset_id) << 32) ^ qHash(label));
  }
  return fp;
}

void CatalogModel::rebuildNow() {
  if (impl_->session == nullptr) {
    if (!impl_->items.empty()) {
      impl_->items.clear();
      emit cleared();
    }
    return;
  }

  Impl::ItemMap next_items;
  const DataReader reader = impl_->session->createReader();
  DataEngine& engine = impl_->session->dataEngine();
  ObjectStore& object_store = impl_->session->objectStore();
  const std::vector<DatasetId> dataset_ids = reader.listDatasets();
  // Ephemeral (preview) transform outputs are intentionally NOT catalogued — the
  // Transform Editor's live preview materializes a real output topic, and it must
  // never surface in the Sources tree regardless of when a rebuild fires.
  const std::unordered_set<TopicId> ephemeral_outputs = impl_->session->dataProcessorService().ephemeralOutputTopics();

  // issue #98: a plugin-provided display name (set via setDatasetDisplayName)
  // overrides the file-derived source_name label, with the same '/'→'_'
  // normalization as baseDatasetLabel so it participates in the duplicate-label
  // ordinal disambiguation below. Absent/empty ⇒ the source_name label.
  const auto effective_base_label = [this, &engine](DatasetId id) -> QString {
    if (const auto it = impl_->dataset_display_overrides.find(id);
        it != impl_->dataset_display_overrides.end() && !it->second.isEmpty()) {
      QString label = it->second;
      label.replace('/', '_');
      return label;
    }
    return baseDatasetLabel(engine.getDataset(id));
  };

  tsl::robin_map<DatasetId, QString> dataset_labels;
  tsl::robin_map<QString, int, QStringHash> label_counts;
  for (const DatasetId dataset_id : dataset_ids) {
    if (impl_->removed_datasets.count(dataset_id) > 0) {
      continue;
    }
    ++label_counts[effective_base_label(dataset_id)];
  }

  tsl::robin_map<QString, int, QStringHash> label_ordinals;
  for (const DatasetId dataset_id : dataset_ids) {
    if (impl_->removed_datasets.count(dataset_id) > 0) {
      continue;
    }
    const QString base_label = effective_base_label(dataset_id);
    QString label = base_label;
    if (label_counts[base_label] > 1) {
      const int ordinal = ++label_ordinals[base_label];
      if (ordinal > 1) {
        label = QStringLiteral("%1 (%2)").arg(base_label).arg(ordinal);
      }
    }
    dataset_labels.insert_or_assign(dataset_id, std::move(label));
  }

  for (const DatasetId dataset_id : dataset_ids) {
    if (impl_->removed_datasets.count(dataset_id) > 0) {
      continue;
    }
    const Impl::NameSet* removed_names_for_dataset = nullptr;
    if (const auto it = impl_->removed_names_per_dataset.find(dataset_id);
        it != impl_->removed_names_per_dataset.end()) {
      removed_names_for_dataset = &it->second;
    }

    const auto dataset_label_it = dataset_labels.find(dataset_id);
    const QString dataset_label =
        dataset_label_it != dataset_labels.end() ? dataset_label_it->second : effective_base_label(dataset_id);

    for (const TopicId topic_id : reader.listTopics(dataset_id)) {
      if (ephemeral_outputs.count(topic_id) > 0) {
        continue;  // preview node output — never shown in the catalog
      }
      const auto metadata = reader.getMetadata(topic_id);
      if (!metadata.has_value()) {
        continue;
      }

      // Fetch the type tree (itself a locking read) BEFORE the short lock below, so
      // the lock spans only the raw getTopicStorage + column copy. (The recursive
      // mutex would tolerate nesting; this just keeps the hold short.)
      const TypeTreeNode* type_tree = reader.getTypeTree(topic_id);

      // getTopicStorage() is raw + non-locking: its result is valid only while the
      // engine lock is held, so copy the columns under a short lock — a concurrent
      // worker commit / createTopicField cannot then race the descriptor read.
      std::vector<ColumnDescriptor> columns;
      {
        const auto lock = engine.lockEngine();
        const TopicStorage* storage = engine.getTopicStorage(topic_id);
        if (storage == nullptr) {
          continue;
        }
        columns = topicColumns(*storage, type_tree);
      }
      for (std::size_t column_index = 0; column_index < columns.size(); ++column_index) {
        const ColumnDescriptor& column = columns[column_index];
        // Surface numeric columns (plottable curves) and string columns (shown in
        // the value column as read-only text). Other non-numeric types stay out.
        const bool is_string = column.logical_type == PrimitiveType::kString;
        if (!isCatalogNumeric(column.logical_type) && !is_string) {
          continue;
        }

        const QString key = makeCurveKey(dataset_id, topic_id, column_index);
        if (removed_names_for_dataset != nullptr && removed_names_for_dataset->count(key) > 0) {
          continue;
        }
        next_items.insert_or_assign(
            key, CatalogItem{
                     .key = key,
                     .dataset_name = dataset_label,
                     .topic_name = QString::fromStdString(metadata->name),
                     .dataset_id = dataset_id,
                     .payload =
                         ScalarFieldPayload{
                             .field_name = QString::fromStdString(column.field_path),
                             .field_path = QString::fromStdString(column.field_path),
                             .topic_id = topic_id,
                             .column_index = column_index,
                             .is_string = is_string,
                         },
                 });
      }
    }

    for (const ObjectTopicId object_topic_id : object_store.listTopics(dataset_id)) {
      const ObjectTopicDescriptor& object_topic = object_store.descriptor(object_topic_id);
      const QString key = makeObjectTopicKey(dataset_id, object_topic_id);
      if (removed_names_for_dataset != nullptr && removed_names_for_dataset->count(key) > 0) {
        // Honor the per-dataset removal blacklist for object topics too.
        // Without this, dropping an image topic into the trash would silently
        // come back on the next rebuildFromDatastore.
        continue;
      }
      next_items.insert_or_assign(
          key, CatalogItem{
                   .key = key,
                   .dataset_name = dataset_label,
                   .topic_name = QString::fromStdString(object_topic.topic_name),
                   .dataset_id = dataset_id,
                   .payload =
                       ObjectTopicPayload{
                           .object_topic_id = object_topic_id,
                           .object_type = objectTypeFromMetadata(object_topic.metadata_json),
                           .metadata_json = QString::fromStdString(object_topic.metadata_json),
                       },
               });
    }
  }

  const Impl::ItemMap previous_items = std::move(impl_->items);
  impl_->items = std::move(next_items);

  if (!previous_items.empty() && impl_->items.empty()) {
    emit cleared();
    return;
  }

  std::vector<CatalogItem> added_items;
  QStringList removed_keys;
  for (const auto& [key, descriptor] : previous_items) {
    (void)descriptor;
    if (impl_->items.find(key) == impl_->items.end()) {
      removed_keys.push_back(key);
    }
  }
  for (const auto& [key, descriptor] : impl_->items) {
    if (previous_items.find(key) == previous_items.end()) {
      added_items.push_back(descriptor);
    }
  }
  if (!added_items.empty()) {
    std::sort(added_items.begin(), added_items.end(), catalogItemLess);
    emit itemsAdded(added_items);
    for (const CatalogItem& descriptor : added_items) {
      emit itemAdded(descriptor);
    }
  }
  if (!removed_keys.isEmpty()) {
    emit itemsRemoved(removed_keys);
  }
}

void CatalogModel::clearAll(bool tombstone) {
  if (impl_->items.empty()) {
    return;
  }
  // Tombstone only when the underlying engine data is NOT being erased: the tombstone
  // hides data that lingers in the engine. A real "delete all" erases the engine too,
  // so there is nothing to hide (tombstone=false) and no soft-delete state accumulates.
  if (tombstone) {
    for (const auto& kv : impl_->items) {
      impl_->removed_datasets.insert(kv.second.dataset_id);
    }
  }
  impl_->items.clear();
  impl_->removed_names_per_dataset.clear();
  emit cleared();
}

void CatalogModel::resetRemovalState() {
  if (impl_->removed_datasets.empty() && impl_->removed_names_per_dataset.empty()) {
    return;
  }
  impl_->removed_datasets.clear();
  impl_->removed_names_per_dataset.clear();
  rebuildFromDatastore();
}

void CatalogModel::restoreDataset(DatasetId dataset_id) {
  const bool was_removed = impl_->removed_datasets.erase(dataset_id) > 0;
  const bool had_per_item = impl_->removed_names_per_dataset.erase(dataset_id) > 0;
  if (was_removed || had_per_item) {
    rebuildFromDatastore();
  }
}

void CatalogModel::setDatasetDisplayName(DatasetId dataset_id, const QString& display_name) {
  if (display_name.isEmpty()) {
    if (impl_->dataset_display_overrides.erase(dataset_id) > 0) {
      rebuildFromDatastore();
    }
    return;
  }
  const auto it = impl_->dataset_display_overrides.find(dataset_id);
  if (it != impl_->dataset_display_overrides.end() && it->second == display_name) {
    return;
  }
  impl_->dataset_display_overrides.insert_or_assign(dataset_id, display_name);
  rebuildFromDatastore();
}

void CatalogModel::removeItems(const std::vector<QString>& keys) {
  // Accepts any catalog item kind. Object-topic keys are honored too, so the
  // trash/remove flow stays symmetric across scalar fields and object topics
  // — rebuildFromDatastore consults the same blacklist for both, so an entry
  // removed here will not silently come back on the next commit.
  QStringList removed;
  for (const QString& key : keys) {
    const auto it = impl_->items.find(key);
    if (it == impl_->items.end()) {
      continue;
    }
    const DatasetId dataset_id = it->second.dataset_id;
    impl_->removed_names_per_dataset[dataset_id].insert(key);
    impl_->items.erase(it);
    removed.push_back(key);
  }
  if (!removed.isEmpty()) {
    emit itemsRemoved(removed);
  }
}

void CatalogModel::removeCurves(const std::vector<QString>& keys) {
  removeItems(keys);
}

bool CatalogModel::removeDataset(DatasetId dataset_id, bool tombstone) {
  std::vector<QString> keys;
  for (const auto& [key, item] : impl_->items) {
    if (item.dataset_id == dataset_id) {
      keys.push_back(key);
    }
  }
  if (keys.empty()) {
    return false;
  }
  // Whole-dataset tombstone (vs removeItems' per-name blacklist): rebuildFromDatastore
  // hides this id until a reload mints a new DatasetId or restoreDataset un-hides it.
  // Skipped for a real "Remove Dataset": the caller erases the engine/object data too, so
  // there is nothing left to hide and no soft-delete state should accumulate.
  if (tombstone) {
    impl_->removed_datasets.insert(dataset_id);
  }
  impl_->removed_names_per_dataset.erase(dataset_id);
  for (const QString& key : keys) {
    impl_->items.erase(key);
  }
  // Empty catalog: one cleared() (views reset in O(1)); otherwise one batched
  // itemsRemoved so consumers react once, not per key.
  if (impl_->items.empty()) {
    emit cleared();
  } else {
    emit itemsRemoved(QStringList(keys.begin(), keys.end()));
  }
  return true;
}

}  // namespace PJ
