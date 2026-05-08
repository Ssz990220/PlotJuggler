#include "pj_runtime/CatalogModel.h"

#include <tsl/robin_map.h>

#include <QHash>
#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_datastore/column_buffer.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_runtime/SessionManager.h"

namespace PJ {
namespace {

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

[[nodiscard]] QString makeCurveName(const std::string& topic_name, const std::string& field_path) {
  QString name = QString::fromStdString(topic_name);
  if (!field_path.empty()) {
    if (!name.endsWith('/')) {
      name += '/';
    }
    name += QString::fromStdString(field_path).replace('.', '/');
  }
  return name;
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
        const std::string element_path = current_path + "[" + std::to_string(i) + "]";
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

struct CatalogModel::Impl {
  explicit Impl(SessionManager* session_in) : session(session_in) {}

  using CurveMap = tsl::robin_map<QString, CurveDescriptor, QStringHash>;

  SessionManager* session = nullptr;
  CurveMap curves;
};

CatalogModel::CatalogModel(SessionManager* session, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(session)) {
  if (impl_->session != nullptr) {
    connect(impl_->session, &SessionManager::topicsCommitted, this, &CatalogModel::rebuildFromDatastore);
  }
}

CatalogModel::~CatalogModel() = default;

std::vector<QString> CatalogModel::curveNames() const {
  std::vector<QString> names;
  names.reserve(impl_->curves.size());
  for (const auto& [name, descriptor] : impl_->curves) {
    (void)descriptor;
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::optional<CurveDescriptor> CatalogModel::curveDescriptor(const QString& name) const {
  const auto it = impl_->curves.find(name);
  if (it == impl_->curves.end()) {
    return std::nullopt;
  }
  return it->second;
}

void CatalogModel::rebuildFromDatastore() {
  if (impl_->session == nullptr) {
    if (!impl_->curves.empty()) {
      impl_->curves.clear();
      emit cleared();
    }
    return;
  }

  Impl::CurveMap next_curves;
  const DataReader reader = impl_->session->createReader();
  DataEngine& engine = impl_->session->dataEngine();

  for (const DatasetId dataset_id : reader.listDatasets()) {
    const DatasetInfo* dataset = engine.getDataset(dataset_id);
    const TimeDomain* time_domain = nullptr;
    if (dataset != nullptr && dataset->time_domain.id != 0) {
      time_domain = engine.getTimeDomain(dataset->time_domain.id);
    }
    const Timestamp display_offset = time_domain != nullptr ? time_domain->display_offset : 0;

    for (const TopicId topic_id : reader.listTopics(dataset_id)) {
      const auto metadata = reader.getMetadata(topic_id);
      if (!metadata.has_value()) {
        continue;
      }

      const TopicStorage* storage = engine.getTopicStorage(topic_id);
      if (storage == nullptr) {
        continue;
      }

      const std::vector<ColumnDescriptor> columns = topicColumns(*storage, reader.getTypeTree(topic_id));
      for (std::size_t column_index = 0; column_index < columns.size(); ++column_index) {
        const ColumnDescriptor& column = columns[column_index];
        if (!isCatalogNumeric(column.logical_type)) {
          continue;
        }

        const QString field_path = QString::fromStdString(column.field_path);
        const QString name = makeCurveName(metadata->name, column.field_path);
        next_curves.insert_or_assign(
            name, CurveDescriptor{
                      .name = name,
                      .topic_id = topic_id,
                      .dataset_id = dataset_id,
                      .column_index = column_index,
                      .field_path = field_path,
                      .display_offset_ns = display_offset,
                  });
      }
    }
  }

  const Impl::CurveMap previous_curves = std::move(impl_->curves);
  impl_->curves = std::move(next_curves);

  if (!previous_curves.empty() && impl_->curves.empty()) {
    emit cleared();
    return;
  }

  for (const auto& [name, descriptor] : previous_curves) {
    (void)descriptor;
    if (impl_->curves.find(name) == impl_->curves.end()) {
      emit curveRemoved(name);
    }
  }

  for (const auto& [name, descriptor] : impl_->curves) {
    (void)descriptor;
    if (previous_curves.find(name) == previous_curves.end()) {
      emit curveAdded(name);
    }
  }
}

}  // namespace PJ
