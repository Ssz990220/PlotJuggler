// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/derived_engine.hpp"

#include <fmt/format.h>
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>

#include <algorithm>
#include <cassert>
#include <limits>
#include <queue>
#include <string>
#include <vector>

#include "pj_base/type_tree.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Walk a TypeTreeNode DFS to find the first primitive leaf's PrimitiveType.
static std::optional<PJ::PrimitiveType> findFirstLeaf(const PJ::TypeTreeNode& node) {
  switch (node.kind) {
    case PJ::TypeKind::kPrimitive:
      return node.primitive_type;
    case PJ::TypeKind::kEnum:
      return node.primitive_type;  // set by make_enum via primitive_type field
    case PJ::TypeKind::kStruct:
      for (const auto& child : node.children) {
        if (auto r = findFirstLeaf(*child)) {
          return r;
        }
      }
      return std::nullopt;
    case PJ::TypeKind::kArray:
      if (node.element_type) {
        return findFirstLeaf(*node.element_type);
      }
      return std::nullopt;
  }
  return std::nullopt;
}

// Return the PrimitiveType of the flat leaf column at index `target` (0-based).
// Arrays are expanded element-wise exactly like count_leaf_fields_impl / the
// chunk column layout, so `target` matches the engine's column index. `seen`
// accumulates the number of leaves visited so far (start at 0).
static std::optional<PJ::PrimitiveType> nthLeafPrimitive(
    const PJ::TypeTreeNode& node, std::size_t target, std::size_t& seen) {
  switch (node.kind) {
    case PJ::TypeKind::kPrimitive:
    case PJ::TypeKind::kEnum:
      if (seen == target) {
        return node.primitive_type;
      }
      ++seen;
      return std::nullopt;
    case PJ::TypeKind::kStruct:
      for (const auto& child : node.children) {
        if (auto r = nthLeafPrimitive(*child, target, seen)) {
          return r;
        }
      }
      return std::nullopt;
    case PJ::TypeKind::kArray:
      if (node.element_type && node.fixed_array_size.has_value()) {
        for (uint32_t i = 0; i < *node.fixed_array_size; ++i) {
          if (auto r = nthLeafPrimitive(*node.element_type, target, seen)) {
            return r;
          }
        }
      }
      return std::nullopt;  // dynamic array contributes no columns
  }
  return std::nullopt;
}

static PJ::PrimitiveType storageKindToPrimitive(StorageKind k) {
  switch (k) {
    case StorageKind::kFloat32:
      return PJ::PrimitiveType::kFloat32;
    case StorageKind::kFloat64:
      return PJ::PrimitiveType::kFloat64;
    case StorageKind::kInt32:
      return PJ::PrimitiveType::kInt32;
    case StorageKind::kInt64:
      return PJ::PrimitiveType::kInt64;
    case StorageKind::kUint64:
      return PJ::PrimitiveType::kUint64;
    case StorageKind::kBool:
      return PJ::PrimitiveType::kBool;
    case StorageKind::kString:
      return PJ::PrimitiveType::kString;
  }
  return PJ::PrimitiveType::kFloat64;
}

// Decode one row of a chunk column into a VarValue, based on the column's StorageKind.
static VarValue decodeAsVarvalue(const TopicChunk& chunk, std::size_t col, std::size_t row, StorageKind kind) {
  switch (kind) {
    case StorageKind::kFloat32:
    case StorageKind::kFloat64:
      return chunk.readNumericAsDouble(col, row);
    case StorageKind::kInt32:
    case StorageKind::kInt64:
      return chunk.readNumericAsInt64(col, row);
    case StorageKind::kUint64:
      return chunk.readNumericAsUint64(col, row);
    case StorageKind::kBool:
      return static_cast<int64_t>(chunk.readBool(col, row) ? 1 : 0);
    case StorageKind::kString:
      return std::string(chunk.readString(col, row));
  }
  return 0.0;
}

// Write a VarValue to a DataWriter row at (topic, col), coercing to out_kind.
static void writeVarvalue(
    DataWriter& writer, PJ::TopicId tid, std::size_t col, const VarValue& val, StorageKind out_kind) {
  if (out_kind == StorageKind::kString) {
    if (const auto* s = std::get_if<std::string>(&val)) {
      writer.set(tid, col, std::string_view(*s));
    }
    return;
  }

  // Integer→integer fast paths: avoid the lossy double round-trip.
  if (out_kind == StorageKind::kUint64) {
    if (const auto* u = std::get_if<uint64_t>(&val)) {
      writer.set(tid, col, *u);
      return;
    }
    if (const auto* i = std::get_if<int64_t>(&val)) {
      writer.set(tid, col, static_cast<uint64_t>(*i));
      return;
    }
  }
  if (out_kind == StorageKind::kInt64) {
    if (const auto* i = std::get_if<int64_t>(&val)) {
      writer.set(tid, col, *i);
      return;
    }
    if (const auto* u = std::get_if<uint64_t>(&val)) {
      writer.set(tid, col, static_cast<int64_t>(*u));
      return;
    }
  }
  if (out_kind == StorageKind::kInt32) {
    if (const auto* i = std::get_if<int64_t>(&val)) {
      writer.set(tid, col, static_cast<int32_t>(*i));
      return;
    }
    if (const auto* u = std::get_if<uint64_t>(&val)) {
      writer.set(tid, col, static_cast<int32_t>(*u));
      return;
    }
  }

  // Fallback: extract as double then coerce to the target type.
  double dval = std::visit(
      [](const auto& v) -> double {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return 0.0;
        } else {
          return static_cast<double>(v);
        }
      },
      val);

  switch (out_kind) {
    case StorageKind::kFloat32:
      writer.set(tid, col, static_cast<float>(dval));
      break;
    case StorageKind::kFloat64:
      writer.set(tid, col, dval);
      break;
    case StorageKind::kInt32:
      writer.set(tid, col, static_cast<int32_t>(dval));
      break;
    case StorageKind::kInt64:
      writer.set(tid, col, static_cast<int64_t>(dval));
      break;
    case StorageKind::kUint64:
      writer.set(tid, col, static_cast<uint64_t>(dval));
      break;
    case StorageKind::kBool:
      writer.set(tid, col, dval != 0.0);
      break;
    case StorageKind::kString:
      break;  // handled above
  }
}

// ---------------------------------------------------------------------------
// Internal data structures (hidden in .cpp — not exposed in header)
// ---------------------------------------------------------------------------

struct DerivedNode {
  PJ::NodeId id = PJ::kInvalidNodeId;
  bool is_mimo = false;

  // SISO fields
  PJ::TopicId siso_input_topic_id = 0;
  std::size_t siso_input_column_index = 0;  // which leaf column of the input topic feeds the op
  StorageKind siso_input_kind = StorageKind::kFloat64;
  StorageKind siso_output_kind = StorageKind::kFloat64;
  std::unique_ptr<ISISOTransform> siso_op;

  // MIMO fields (flat list; no primary/secondary distinction)
  std::vector<PJ::TopicId> mimo_input_topic_ids;
  std::vector<std::size_t> mimo_input_columns;  // leaf column per input (parallel to mimo_input_topic_ids)
  std::vector<StorageKind> mimo_input_kinds;
  std::vector<StorageKind> mimo_output_kinds;
  std::unique_ptr<IMIMOTransform> mimo_op;
  // When true, the M calculate() results are written as M named columns of a SINGLE
  // output topic (output_topic_ids[0]); when false, each result is its own scalar
  // topic. See DerivedEngine::addMimoTransform's output_topic_group.
  bool mimo_grouped = false;

  // Common
  std::vector<PJ::TopicId> all_input_topic_ids;  // unified input list for all types
  std::vector<PJ::TopicId> output_topic_ids;     // 1 for SISO, M for MIMO
  bool dirty = true;
  PJ::ChunkId last_processed_chunk_id = 0;                                 // SISO: chunk watermark
  PJ::Timestamp mimo_last_ts = std::numeric_limits<PJ::Timestamp>::min();  // MIMO: timestamp watermark
  // Regression detection (out-of-order ingest): a not-yet-processed input
  // chunk landing at or before these watermarks forces a reset + full replay,
  // because transforms have a strict ascending-timestamp contract.
  PJ::Timestamp siso_last_ts = std::numeric_limits<PJ::Timestamp>::min();  // SISO: last input ts fed
  PJ::ChunkId mimo_last_chunk_id = 0;                                      // MIMO: last input chunk considered

  // Reusable decode buffers (avoid per-row allocation)
  VarValue in_val_buf = 0.0;           // SISO input
  VarValue out_val_buf = 0.0;          // SISO output
  std::vector<VarValue> mimo_in_buf;   // MIMO inputs
  std::vector<VarValue> mimo_out_buf;  // MIMO outputs
};

struct DatasetNameHash {
  std::size_t operator()(const std::pair<PJ::DatasetId, std::string>& key) const noexcept {
    std::size_t h1 = std::hash<PJ::DatasetId>{}(key.first);
    std::size_t h2 = std::hash<std::string>{}(key.second);
    return h1 ^ (h2 << 1);
  }
};

struct DerivedEngineImpl {
  tsl::robin_map<PJ::NodeId, DerivedNode> nodes;

  // downstream_of[N] = list of nodes whose inputs include an output of N
  tsl::robin_map<PJ::NodeId, std::vector<PJ::NodeId>> downstream_of;

  // topic_to_nodes[T] = list of nodes that use T as an input
  tsl::robin_map<PJ::TopicId, std::vector<PJ::NodeId>> topic_to_nodes;

  // output_topic_to_node[T] = node that produces T (for cycle detection)
  tsl::robin_map<PJ::TopicId, PJ::NodeId> output_topic_to_node;

  // Name uniqueness within dataset: (dataset_id, topic_name) → topic_id
  tsl::robin_map<std::pair<PJ::DatasetId, std::string>, PJ::TopicId, DatasetNameHash> registered_output_names;
};

// ---------------------------------------------------------------------------
// DerivedEngine — constructor / destructor
// ---------------------------------------------------------------------------

DerivedEngine::DerivedEngine(DataEngine& engine) : engine_(engine), impl_(std::make_unique<DerivedEngineImpl>()) {}

DerivedEngine::~DerivedEngine() = default;

// ---------------------------------------------------------------------------
// Cycle detection (DFS)
// ---------------------------------------------------------------------------
// Returns an error string if adding a node with `input_topics → output_topics`
// would create a cycle. Otherwise returns empty string.
static std::string checkCycle(
    const DerivedEngineImpl& impl, const std::vector<PJ::TopicId>& input_topics,
    const std::vector<PJ::TopicId>& output_topics) {
  tsl::robin_set<PJ::TopicId> outputs(output_topics.begin(), output_topics.end());

  // DFS from each input: follow upstream edges (output → producing node → its inputs).
  // If we ever reach a topic in `outputs`, we have a cycle.
  std::vector<PJ::TopicId> stack(input_topics.begin(), input_topics.end());
  tsl::robin_set<PJ::TopicId> visited;

  while (!stack.empty()) {
    PJ::TopicId t = stack.back();
    stack.pop_back();
    if (!visited.insert(t).second) {
      continue;
    }

    if (outputs.contains(t)) {
      return fmt::format("cycle detected: topic {} is both an input and an output", t);
    }

    auto it = impl.output_topic_to_node.find(t);
    if (it == impl.output_topic_to_node.end()) {
      continue;  // source topic, no upstream
    }

    PJ::NodeId producer = it->second;
    auto nit = impl.nodes.find(producer);
    if (nit == impl.nodes.end()) {
      continue;
    }

    for (PJ::TopicId in : nit->second.all_input_topic_ids) {
      if (!visited.contains(in)) {
        stack.push_back(in);
      }
    }
  }
  return "";  // no cycle
}

// ---------------------------------------------------------------------------
// add_siso_transform
// ---------------------------------------------------------------------------

PJ::Expected<PJ::NodeId> DerivedEngine::addSisoTransform(
    PJ::TopicId input_topic_id, std::string output_topic_name, PJ::DatasetId output_dataset_id,
    std::unique_ptr<ISISOTransform> op, std::size_t input_column_index) {
  // Atomic against worker ingest: the input getTopicStorage() read, the
  // typeRegistry().registerOrGet() write, and createTopic() (rehash) run as one
  // unit. Recursive mutex, so the nested createTopic re-acquires harmlessly.
  auto lock = engine_.lockEngine();
  // 1. Check input topic exists
  const TopicStorage* in_storage = engine_.getTopicStorage(input_topic_id);
  if (!in_storage) {
    return PJ::unexpected(fmt::format("add_siso_transform: input topic {} not found", input_topic_id));
  }

  // 2. Determine the single leaf column's StorageKind.
  // Prefer TypeRegistry (via schema_id). Fall back to the first sealed chunk's
  // column_descriptors when schema_id == 0 (e.g. topics created via
  // register_scalar_series, which stores schema only in the writer's internal state).
  PJ::SchemaId schema_id = in_storage->descriptor().schema_id;

  std::size_t num_cols = 0;
  std::optional<PJ::PrimitiveType> leaf_primitive;

  if (schema_id != 0) {
    const PJ::TypeTreeNode* root = engine_.typeRegistry().lookup(schema_id);
    if (root) {
      num_cols = PJ::countLeafFields(*root);
      if (input_column_index < num_cols) {
        std::size_t seen = 0;
        leaf_primitive = nthLeafPrimitive(*root, input_column_index, seen);
      }
    }
  }

  if (num_cols == 0) {
    // Fall back 1: inline column layout stored in TopicStorage at registration time.
    // This covers schema_id==0 topics (register_scalar_series) with no committed chunks yet.
    const auto& stored = in_storage->columnDescriptors();
    if (!stored.empty()) {
      num_cols = stored.size();
      if (input_column_index < num_cols) {
        leaf_primitive = stored[input_column_index].logical_type;
      }
    }
  }

  if (num_cols == 0) {
    // Fall back 2: first committed chunk's columnDescriptors (legacy path).
    const auto& chunks = in_storage->sealedChunks();
    if (!chunks.empty() && !chunks[0].columns.empty()) {
      num_cols = chunks[0].columns.size();
      if (input_column_index < num_cols) {
        leaf_primitive = chunks[0].columns[input_column_index].descriptor->logical_type;
      }
    }
  }

  if (num_cols == 0) {
    return PJ::unexpected(
        fmt::format(
            "add_siso_transform: cannot determine column layout for topic {}"
            " (no schema_id, no stored column layout, and no committed chunks)",
            input_topic_id));
  }
  if (input_column_index >= num_cols) {
    return PJ::unexpected(
        fmt::format(
            "add_siso_transform: input column {} out of range (topic {} has {} columns)", input_column_index,
            input_topic_id, num_cols));
  }
  if (!leaf_primitive) {
    return PJ::unexpected("add_siso_transform: could not determine leaf primitive type for the selected column");
  }
  StorageKind in_kind = storageKindOf(*leaf_primitive);

  // 3. Determine output kind
  StorageKind out_kind = op->outputKind(in_kind);

  // 4. Check output name uniqueness within dataset
  auto name_key = std::make_pair(output_dataset_id, output_topic_name);
  if (impl_->registered_output_names.contains(name_key)) {
    return PJ::unexpected(
        fmt::format(
            "add_siso_transform: output topic '{}' already registered in dataset {}", output_topic_name,
            output_dataset_id));
  }

  // 5. Cycle detection (structurally impossible for SISO fresh output, but guard correctly)
  std::string cycle_err = checkCycle(*impl_, {input_topic_id}, {});  // output topic doesn't exist yet
  if (!cycle_err.empty()) {
    return PJ::unexpected(cycle_err);
  }

  // 6. Create output schema (single column, output_kind, name = "value")
  PJ::PrimitiveType out_primitive = storageKindToPrimitive(out_kind);
  std::string schema_name = fmt::format("derived_siso_{}_{}", output_topic_name, next_node_id_);
  auto out_type_tree = PJ::makePrimitive("value", out_primitive);
  auto out_schema_or = engine_.typeRegistry().registerOrGet(schema_name, out_type_tree);
  if (!out_schema_or.has_value()) {
    return PJ::unexpected(out_schema_or.error());
  }

  // 7. Create output topic
  auto out_topic_or = engine_.createTopic(
      output_dataset_id,
      TopicDescriptor{.name = output_topic_name, .schema_id = *out_schema_or, .dataset_id = output_dataset_id});
  if (!out_topic_or.has_value()) {
    return PJ::unexpected(out_topic_or.error());
  }
  PJ::TopicId out_topic_id = *out_topic_or;

  // 8. Register node
  PJ::NodeId node_id = next_node_id_++;
  DerivedNode node;
  node.id = node_id;
  node.is_mimo = false;
  node.siso_input_topic_id = input_topic_id;
  node.siso_input_column_index = input_column_index;
  node.siso_input_kind = in_kind;
  node.siso_output_kind = out_kind;
  node.siso_op = std::move(op);
  node.all_input_topic_ids = {input_topic_id};
  node.output_topic_ids = {out_topic_id};
  node.dirty = true;

  impl_->registered_output_names[name_key] = out_topic_id;
  impl_->topic_to_nodes[input_topic_id].push_back(node_id);
  impl_->output_topic_to_node[out_topic_id] = node_id;

  // Update downstream_of: if input_topic_id is produced by another node, register dependency
  auto prod_it = impl_->output_topic_to_node.find(input_topic_id);
  if (prod_it != impl_->output_topic_to_node.end()) {
    impl_->downstream_of[prod_it->second].push_back(node_id);
  }

  impl_->nodes[node_id] = std::move(node);
  return node_id;
}

// ---------------------------------------------------------------------------
// add_mimo_transform
// ---------------------------------------------------------------------------

PJ::Expected<PJ::NodeId> DerivedEngine::addMimoTransform(
    std::vector<PJ::TopicId> input_topic_ids, std::vector<std::string> output_topic_names,
    PJ::DatasetId output_dataset_id, std::unique_ptr<IMIMOTransform> op, std::vector<std::size_t> input_columns,
    std::string output_topic_group) {
  const bool grouped = !output_topic_group.empty();
  // Atomic against concurrent worker ingest (input reads + typeRegistry write +
  // createTopic rehash); recursive mutex, so nested createTopic re-acquires.
  auto lock = engine_.lockEngine();
  if (input_topic_ids.empty()) {
    return PJ::unexpected("add_mimo_transform: requires at least one input topic");
  }
  if (output_topic_names.empty()) {
    return PJ::unexpected("add_mimo_transform: requires at least one output topic name");
  }
  if (!op) {
    return PJ::unexpected("add_mimo_transform: null transform op");
  }
  // Default each input to its first leaf column; otherwise one column per input.
  if (input_columns.empty()) {
    input_columns.assign(input_topic_ids.size(), 0);
  } else if (input_columns.size() != input_topic_ids.size()) {
    return PJ::unexpected(
        fmt::format(
            "add_mimo_transform: input_columns has {} entries but {} input topics", input_columns.size(),
            input_topic_ids.size()));
  }

  // 1. Validate all inputs and determine their StorageKinds.
  //    Same 3-tier fallback as add_siso_transform: type_registry → stored
  //    column_descriptors → first sealed chunk.
  std::vector<StorageKind> input_kinds;
  input_kinds.reserve(input_topic_ids.size());

  for (std::size_t i = 0; i < input_topic_ids.size(); ++i) {
    const PJ::TopicId tid = input_topic_ids[i];
    const std::size_t col = input_columns[i];  // leaf column feeding this input
    const TopicStorage* storage = engine_.getTopicStorage(tid);
    if (!storage) {
      return PJ::unexpected(fmt::format("add_mimo_transform: input topic {} not found", tid));
    }

    PJ::SchemaId schema_id = storage->descriptor().schema_id;
    std::size_t num_cols = 0;
    std::optional<PJ::PrimitiveType> leaf_primitive;

    if (schema_id != 0) {
      const PJ::TypeTreeNode* root = engine_.typeRegistry().lookup(schema_id);
      if (root) {
        num_cols = PJ::countLeafFields(*root);
        if (col < num_cols) {
          std::size_t seen = 0;
          leaf_primitive = nthLeafPrimitive(*root, col, seen);
        }
      }
    }
    if (num_cols == 0) {
      const auto& stored = storage->columnDescriptors();
      if (!stored.empty()) {
        num_cols = stored.size();
        if (col < num_cols) {
          leaf_primitive = stored[col].logical_type;
        }
      }
    }
    if (num_cols == 0) {
      const auto& chunks = storage->sealedChunks();
      if (!chunks.empty() && !chunks[0].columns.empty()) {
        num_cols = chunks[0].columns.size();
        if (col < num_cols) {
          leaf_primitive = chunks[0].columns[col].descriptor->logical_type;
        }
      }
    }

    if (num_cols == 0) {
      return PJ::unexpected(fmt::format("add_mimo_transform: cannot determine column layout for input topic {}", tid));
    }
    if (col >= num_cols) {
      return PJ::unexpected(
          fmt::format(
              "add_mimo_transform: column index {} out of range for input topic {} ({} columns)", col, tid, num_cols));
    }
    if (!leaf_primitive) {
      return PJ::unexpected(fmt::format("add_mimo_transform: cannot determine primitive type for input topic {}", tid));
    }
    input_kinds.push_back(storageKindOf(*leaf_primitive));
  }

  // 2. Check output name uniqueness within dataset. Grouped mode owns a single
  //    topic (output_topic_group); ungrouped owns one per output name.
  {
    std::vector<std::string> owned_topics = grouped ? std::vector<std::string>{output_topic_group} : output_topic_names;
    for (const auto& name : owned_topics) {
      auto key = std::make_pair(output_dataset_id, name);
      if (impl_->registered_output_names.contains(key)) {
        return PJ::unexpected(
            fmt::format(
                "add_mimo_transform: output topic '{}' already registered in dataset {}", name, output_dataset_id));
      }
    }
  }

  // 3. Cycle detection.
  {
    std::string cycle_err = checkCycle(*impl_, input_topic_ids, {});
    if (!cycle_err.empty()) {
      return PJ::unexpected(cycle_err);
    }
  }

  // 4. Query output StorageKinds from the transform.
  std::vector<StorageKind> output_kinds = op->outputKinds(PJ::Span<const StorageKind>(input_kinds));
  if (output_kinds.size() != output_topic_names.size()) {
    return PJ::unexpected(
        fmt::format(
            "add_mimo_transform: op->outputKinds() returned {} kinds but {} output names provided", output_kinds.size(),
            output_topic_names.size()));
  }

  // 5. Create the output topic(s).
  PJ::NodeId node_id = next_node_id_++;
  std::vector<PJ::TopicId> out_topic_ids;

  if (grouped) {
    // ONE topic whose schema is a struct with M named columns (output_topic_names[k]
    // is the field name of column k). The M calculate() results fill columns 0..M-1.
    std::vector<std::shared_ptr<PJ::TypeTreeNode>> fields;
    fields.reserve(output_topic_names.size());
    for (std::size_t k = 0; k < output_topic_names.size(); ++k) {
      fields.push_back(PJ::makePrimitive(output_topic_names[k], storageKindToPrimitive(output_kinds[k])));
    }
    std::string schema_name = fmt::format("derived_mimo_{}_grouped", node_id);
    auto out_schema_or =
        engine_.typeRegistry().registerOrGet(schema_name, PJ::makeStruct(schema_name, std::move(fields)));
    if (!out_schema_or.has_value()) {
      return PJ::unexpected(out_schema_or.error());
    }
    auto out_topic_or = engine_.createTopic(
        output_dataset_id,
        TopicDescriptor{.name = output_topic_group, .schema_id = *out_schema_or, .dataset_id = output_dataset_id});
    if (!out_topic_or.has_value()) {
      return PJ::unexpected(out_topic_or.error());
    }
    out_topic_ids.push_back(*out_topic_or);
  } else {
    // One scalar topic (single "value" column) per output name.
    out_topic_ids.reserve(output_topic_names.size());
    for (std::size_t k = 0; k < output_topic_names.size(); ++k) {
      PJ::PrimitiveType out_primitive = storageKindToPrimitive(output_kinds[k]);
      std::string schema_name = fmt::format("derived_mimo_{}_{}", node_id, k);
      auto out_type_tree = PJ::makePrimitive("value", out_primitive);
      auto out_schema_or = engine_.typeRegistry().registerOrGet(schema_name, out_type_tree);
      if (!out_schema_or.has_value()) {
        return PJ::unexpected(out_schema_or.error());
      }
      auto out_topic_or = engine_.createTopic(
          output_dataset_id,
          TopicDescriptor{.name = output_topic_names[k], .schema_id = *out_schema_or, .dataset_id = output_dataset_id});
      if (!out_topic_or.has_value()) {
        return PJ::unexpected(out_topic_or.error());
      }
      out_topic_ids.push_back(*out_topic_or);
    }
  }

  // 6. Build and register the node.
  DerivedNode node;
  node.id = node_id;
  node.is_mimo = true;
  node.mimo_input_topic_ids = input_topic_ids;
  node.mimo_input_columns = std::move(input_columns);
  node.mimo_input_kinds = std::move(input_kinds);
  node.mimo_output_kinds = std::move(output_kinds);
  node.mimo_op = std::move(op);
  node.mimo_grouped = grouped;
  node.mimo_last_ts = std::numeric_limits<PJ::Timestamp>::min();
  node.all_input_topic_ids = std::move(input_topic_ids);
  node.output_topic_ids = std::move(out_topic_ids);
  node.dirty = true;

  // Register the owned topic name(s) for uniqueness enforcement: the single grouped
  // topic, or one per scalar output.
  if (grouped) {
    impl_->registered_output_names[std::make_pair(output_dataset_id, output_topic_group)] = node.output_topic_ids[0];
  } else {
    for (std::size_t k = 0; k < output_topic_names.size(); ++k) {
      impl_->registered_output_names[std::make_pair(output_dataset_id, output_topic_names[k])] =
          node.output_topic_ids[k];
    }
  }

  // Map input topics to this node (for dirty propagation via on_source_committed).
  for (PJ::TopicId in_tid : node.all_input_topic_ids) {
    impl_->topic_to_nodes[in_tid].push_back(node_id);
  }

  // Map output topics to this node (for cycle detection of downstream nodes).
  for (PJ::TopicId out_tid : node.output_topic_ids) {
    impl_->output_topic_to_node[out_tid] = node_id;
  }

  // Update downstream_of: if any input is produced by another derived node,
  // record that node_id depends on the producer (deduplicated for multi-input).
  for (PJ::TopicId in_tid : node.all_input_topic_ids) {
    auto prod_it = impl_->output_topic_to_node.find(in_tid);
    if (prod_it != impl_->output_topic_to_node.end()) {
      auto& list = impl_->downstream_of[prod_it->second];
      if (std::find(list.begin(), list.end(), node_id) == list.end()) {
        list.push_back(node_id);
      }
    }
  }

  impl_->nodes[node_id] = std::move(node);
  return node_id;
}

// ---------------------------------------------------------------------------
// Node management
// ---------------------------------------------------------------------------

PJ::Status DerivedEngine::removeNode(PJ::NodeId id) {
  auto it = impl_->nodes.find(id);
  if (it == impl_->nodes.end()) {
    return PJ::unexpected(fmt::format("remove_node: node {} not found", id));
  }

  const DerivedNode& node = it->second;

  // Remove from topic_to_nodes
  for (PJ::TopicId in_tid : node.all_input_topic_ids) {
    auto& v = impl_->topic_to_nodes[in_tid];
    v.erase(std::remove(v.begin(), v.end(), id), v.end());
  }

  // Remove from output_topic_to_node and registered_output_names
  for (PJ::TopicId out_tid : node.output_topic_ids) {
    impl_->output_topic_to_node.erase(out_tid);
    // Remove from registered_output_names (scan for the value)
    for (auto sit = impl_->registered_output_names.begin(); sit != impl_->registered_output_names.end(); ++sit) {
      if (sit->second == out_tid) {
        impl_->registered_output_names.erase(sit);
        break;
      }
    }
  }

  // Remove from downstream_of
  impl_->downstream_of.erase(id);
  for (auto dit = impl_->downstream_of.begin(); dit != impl_->downstream_of.end(); ++dit) {
    auto& list = dit.value();
    list.erase(std::remove(list.begin(), list.end(), id), list.end());
  }

  impl_->nodes.erase(it);
  return PJ::okStatus();
}

bool DerivedEngine::hasNode(PJ::NodeId id) const noexcept {
  return impl_->nodes.contains(id);
}

std::vector<PJ::TopicId> DerivedEngine::outputTopics(PJ::NodeId id) const {
  auto it = impl_->nodes.find(id);
  if (it == impl_->nodes.end()) {
    return {};
  }
  return it->second.output_topic_ids;
}

// ---------------------------------------------------------------------------
// topological_order — Kahn's algorithm
// ---------------------------------------------------------------------------

std::vector<PJ::NodeId> DerivedEngine::topologicalOrder() const {
  tsl::robin_map<PJ::NodeId, int> in_degree;
  for (const auto& [id, _] : impl_->nodes) {
    in_degree[id] = 0;
  }

  for (const auto& [upstream, downstream_list] : impl_->downstream_of) {
    for (PJ::NodeId downstream : downstream_list) {
      if (impl_->nodes.contains(downstream)) {
        in_degree[downstream]++;
      }
    }
  }

  // Seed queue with in-degree 0 nodes (sorted for determinism)
  std::vector<PJ::NodeId> ready;
  ready.reserve(in_degree.size());
  for (const auto& [id, deg] : in_degree) {
    if (deg == 0) {
      ready.push_back(id);
    }
  }
  std::sort(ready.begin(), ready.end());

  std::vector<PJ::NodeId> order;
  order.reserve(impl_->nodes.size());
  std::size_t head = 0;

  while (head < ready.size()) {
    PJ::NodeId n = ready[head++];
    order.push_back(n);

    auto it = impl_->downstream_of.find(n);
    if (it == impl_->downstream_of.end()) {
      continue;
    }

    std::vector<PJ::NodeId> newly_ready;
    for (PJ::NodeId m : it->second) {
      if (!impl_->nodes.contains(m)) {
        continue;
      }
      if (--in_degree[m] == 0) {
        newly_ready.push_back(m);
      }
    }
    // Keep deterministic order within the newly ready set
    std::sort(newly_ready.begin(), newly_ready.end());
    for (PJ::NodeId m : newly_ready) {
      ready.push_back(m);
    }
  }

  return order;
}

// ---------------------------------------------------------------------------
// on_source_committed
// ---------------------------------------------------------------------------

void DerivedEngine::onSourceCommitted(PJ::Span<const PJ::TopicId> changed_topics) {
  for (PJ::TopicId tid : changed_topics) {
    auto it = impl_->topic_to_nodes.find(tid);
    if (it == impl_->topic_to_nodes.end()) {
      continue;
    }
    for (PJ::NodeId nid : it->second) {
      auto nit = impl_->nodes.find(nid);
      if (nit != impl_->nodes.end()) {
        nit.value().dirty = true;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// run_node_incremental (private helper)
// ---------------------------------------------------------------------------

static PJ::Status runSisoIncremental(DerivedEngineImpl& /*impl*/, DataEngine& engine, DerivedNode& node) {
  const TopicStorage* in_storage = engine.getTopicStorage(node.siso_input_topic_id);
  if (!in_storage) {
    return PJ::unexpected(fmt::format("run_siso_incremental: input topic {} not found", node.siso_input_topic_id));
  }

  const std::deque<TopicChunk>& all_chunks = in_storage->sealedChunks();

  DataWriter writer = engine.createWriter();
  PJ::TopicId out_tid = node.output_topic_ids[0];
  PJ::ChunkId max_seen = node.last_processed_chunk_id;
  bool wrote_any = false;
  PJ::Timestamp out_ts = 0;
  PJ::Status status = PJ::okStatus();

  auto feed_row = [&](const TopicChunk& chunk, std::size_t row) {
    if (!status.has_value()) {
      return;  // an earlier row already failed; drain remaining callbacks
    }
    // Columns can appear mid-stream: an earlier chunk may have fewer columns
    // than the selected index. Such a chunk carries no sample for this field, so
    // skip the row (no input → no state change for a stateful op).
    if (node.siso_input_column_index >= chunk.columns.size()) {
      return;
    }
    const PJ::Timestamp ts = chunk.timestamps[row];
    node.in_val_buf = decodeAsVarvalue(chunk, node.siso_input_column_index, row, node.siso_input_kind);
    node.siso_last_ts = std::max(node.siso_last_ts, ts);

    if (node.siso_op->calculate(ts, node.in_val_buf, out_ts, node.out_val_buf)) {
      auto s = writer.beginRow(out_tid, out_ts);
      if (!s.has_value()) {
        status = std::move(s);
        return;
      }
      writeVarvalue(writer, out_tid, 0, node.out_val_buf, node.siso_output_kind);
      s = writer.finishRow(out_tid);
      if (!s.has_value()) {
        status = std::move(s);
        return;
      }
      wrote_any = true;
    }
  };

  if (node.last_processed_chunk_id == 0) {
    // Fresh node or post-reset replay: input chunks may overlap in time
    // (out-of-order ingest), so feed rows through the merge cursor to honour
    // the transform's ascending-timestamp contract.
    for (const TopicChunk& chunk : all_chunks) {
      max_seen = std::max(max_seen, chunk.id);
    }
    rangeQuery(all_chunks, std::numeric_limits<PJ::Timestamp>::min(), std::numeric_limits<PJ::Timestamp>::max())
        .forEach([&feed_row](const SampleRow& row) { feed_row(*row.chunk, row.row_index); });
  } else {
    // Incremental: the scheduler verified the unprocessed chunks are
    // time-ordered (it resets + replays otherwise), so commit order is
    // ascending and per-chunk iteration is safe.
    for (const TopicChunk& chunk : all_chunks) {
      if (chunk.id <= node.last_processed_chunk_id) {
        continue;
      }
      max_seen = std::max(max_seen, chunk.id);
      for (uint32_t i = 0; i < chunk.stats.row_count; ++i) {
        feed_row(chunk, i);
      }
    }
  }

  if (!status.has_value()) {
    return status;  // hard writer error: do not commit, do not advance the watermark (retry next commit)
  }
  // A sticky op failure (e.g. a Luau runtime error) makes calculate() return false for every
  // sample. Surface it as a Status error AND drop the staged rows: a batch that ends in failure
  // is an untrustworthy truncation. `writer` is function-local, so simply not flushing it
  // discards the staged rows cleanly (no rollback API needed). On a post-reset replay the old
  // output was already cleared by recomputeBatch, so the output is left empty rather than
  // stale-partial — both correct. Advance the watermark so a sticky-failed node is not re-run
  // every commit.
  if (node.siso_op && node.siso_op->failed()) {
    node.last_processed_chunk_id = max_seen;
    return PJ::unexpected(node.siso_op->error());
  }
  if (wrote_any) {
    auto chunks = writer.flushAll();
    engine.commitChunksLocked(
        std::move(chunks));  // caller (scheduleActiveLocked/recomputeBatchLocked) holds lockEngine()
  }
  node.last_processed_chunk_id = max_seen;
  return PJ::okStatus();
}

// ---------------------------------------------------------------------------
// run_mimo_incremental
// ---------------------------------------------------------------------------

static PJ::Status runMimoIncremental(DerivedEngineImpl& /*impl*/, DataEngine& engine, DerivedNode& node) {
  const std::size_t num_inputs = node.mimo_input_topic_ids.size();
  if (num_inputs == 0) {
    return PJ::okStatus();
  }

  // 1. Collect (timestamp, chunk*, row_index) for each input topic,
  //    only for rows strictly newer than the watermark.
  struct SampleLoc {
    PJ::Timestamp ts;
    const TopicChunk* chunk;
    uint32_t row;
  };
  std::vector<std::vector<SampleLoc>> per_topic(num_inputs);

  PJ::ChunkId max_chunk_seen = node.mimo_last_chunk_id;
  for (std::size_t i = 0; i < num_inputs; ++i) {
    const TopicStorage* storage = engine.getTopicStorage(node.mimo_input_topic_ids[i]);
    if (!storage) {
      return PJ::unexpected(
          fmt::format("run_mimo_incremental: input topic {} not found", node.mimo_input_topic_ids[i]));
    }
    for (const TopicChunk& chunk : storage->sealedChunks()) {
      max_chunk_seen = std::max(max_chunk_seen, chunk.id);
      if (chunk.stats.t_max <= node.mimo_last_ts) {
        continue;  // entire chunk already processed
      }
      for (uint32_t r = 0; r < chunk.stats.row_count; ++r) {
        PJ::Timestamp ts = chunk.timestamps[r];
        if (ts <= node.mimo_last_ts) {
          continue;
        }
        per_topic[i].push_back({ts, &chunk, r});
      }
    }
  }
  // Every committed chunk has now been considered — regression detection in
  // the scheduler compares against this watermark. Updated even when the run
  // produces no joins, so a fruitless chunk is not re-flagged forever.
  node.mimo_last_chunk_id = max_chunk_seen;
  for (std::size_t i = 0; i < num_inputs; ++i) {
    // If any topic has no new data, no new join is possible.
    if (per_topic[i].empty()) {
      return PJ::okStatus();
    }
  }
  // Chunks are gathered in commit order, which under out-of-order ingest is
  // not time order; joined_ts is derived from topic 0, so sort it (stable:
  // duplicate timestamps keep commit order for last-write-wins lookups).
  std::stable_sort(
      per_topic[0].begin(), per_topic[0].end(), [](const SampleLoc& a, const SampleLoc& b) { return a.ts < b.ts; });

  // 2. N-way timestamp intersection: find timestamps present in ALL input topics.
  //    Start from topic 0's sorted timestamps, remove any not in subsequent topics.
  std::vector<PJ::Timestamp> joined_ts;
  joined_ts.reserve(per_topic[0].size());
  for (const auto& s : per_topic[0]) {
    joined_ts.push_back(s.ts);
  }

  for (std::size_t i = 1; i < num_inputs; ++i) {
    tsl::robin_set<PJ::Timestamp> topic_set;
    topic_set.reserve(per_topic[i].size());
    for (const auto& s : per_topic[i]) {
      topic_set.insert(s.ts);
    }
    auto new_end =
        std::remove_if(joined_ts.begin(), joined_ts.end(), [&](PJ::Timestamp t) { return !topic_set.contains(t); });
    joined_ts.erase(new_end, joined_ts.end());
    if (joined_ts.empty()) {
      return PJ::okStatus();
    }
  }

  // 2b. Deduplicate joined_ts: if topic[0] has two rows at the same timestamp,
  //     that timestamp appears twice in joined_ts. We must process it exactly
  //     once (joined_ts is sorted — per_topic[0] was sorted above).
  {
    auto new_end = std::unique(joined_ts.begin(), joined_ts.end());
    joined_ts.erase(new_end, joined_ts.end());
  }
  if (joined_ts.empty()) {
    return PJ::okStatus();
  }

  // 3. Build per-topic lookup: timestamp → (chunk*, row_index).
  //    insert_or_assign gives last-write-wins semantics for duplicate timestamps
  //    within a topic, producing a well-defined and consistent result.
  std::vector<tsl::robin_map<PJ::Timestamp, std::pair<const TopicChunk*, uint32_t>>> lookups(num_inputs);
  for (std::size_t i = 0; i < num_inputs; ++i) {
    lookups[i].reserve(per_topic[i].size());
    for (const auto& s : per_topic[i]) {
      lookups[i].insert_or_assign(s.ts, std::make_pair(s.chunk, s.row));
    }
  }

  // 4. Process each joined timestamp: decode, call transform, emit output.
  //    num_outputs is the count of calculate() results (one per output kind). In
  //    grouped mode they are M columns of one topic; otherwise M scalar topics.
  const std::size_t num_outputs = node.mimo_output_kinds.size();
  node.mimo_in_buf.resize(num_inputs);
  node.mimo_out_buf.resize(num_outputs);

  DataWriter writer = engine.createWriter();
  bool wrote_any = false;

  for (PJ::Timestamp ts : joined_ts) {
    // Columns can appear mid-stream: a chunk sealed BEFORE an input's column existed
    // has fewer columns than mimo_input_columns[i]. Such a chunk carries no sample for
    // that input at this timestamp, so skip the whole joined row (mirrors the SISO
    // guard above). Without this, decodeAsVarvalue reads columns[col] out of bounds.
    bool row_has_all_inputs = true;
    for (std::size_t i = 0; i < num_inputs; ++i) {
      const auto& [chp, row] = lookups[i].at(ts);
      if (node.mimo_input_columns[i] >= chp->columns.size()) {
        row_has_all_inputs = false;
        break;
      }
    }
    if (!row_has_all_inputs) {
      continue;
    }
    for (std::size_t i = 0; i < num_inputs; ++i) {
      const auto& [chp, row] = lookups[i].at(ts);
      node.mimo_in_buf[i] = decodeAsVarvalue(*chp, node.mimo_input_columns[i], row, node.mimo_input_kinds[i]);
    }

    PJ::Timestamp out_ts = ts;
    if (!node.mimo_op->calculate(ts, node.mimo_in_buf, out_ts, node.mimo_out_buf)) {
      continue;
    }
    if (node.mimo_grouped) {
      // One row, M columns, into the single grouped topic.
      const PJ::TopicId out_tid = node.output_topic_ids[0];
      auto s = writer.beginRow(out_tid, out_ts);
      if (!s.has_value()) {
        return s;
      }
      for (std::size_t k = 0; k < num_outputs; ++k) {
        writeVarvalue(writer, out_tid, k, node.mimo_out_buf[k], node.mimo_output_kinds[k]);
      }
      s = writer.finishRow(out_tid);
      if (!s.has_value()) {
        return s;
      }
      wrote_any = true;
    } else {
      for (std::size_t k = 0; k < num_outputs; ++k) {
        auto s = writer.beginRow(node.output_topic_ids[k], out_ts);
        if (!s.has_value()) {
          return s;
        }
        writeVarvalue(writer, node.output_topic_ids[k], 0, node.mimo_out_buf[k], node.mimo_output_kinds[k]);
        s = writer.finishRow(node.output_topic_ids[k]);
        if (!s.has_value()) {
          return s;
        }
      }
      wrote_any = true;
    }
  }

  if (wrote_any) {
    engine.commitChunksLocked(writer.flushAll());  // caller holds lockEngine()
  }

  // Advance watermark to the last joined input timestamp. Later input at or
  // before this watermark is a regression — the scheduler detects it via
  // mimo_last_chunk_id and resets + replays the node instead of running this
  // incremental path.
  node.mimo_last_ts = joined_ts.back();

  return PJ::okStatus();
}

// ---------------------------------------------------------------------------
// Out-of-order input detection
// ---------------------------------------------------------------------------

// True when a not-yet-processed input chunk lands at or before data this node
// already consumed. Applying it incrementally would feed the transform out of
// ascending-timestamp order (SISO) or silently skip joined rows (MIMO), so
// the scheduler must reset + fully replay the node instead.
static bool nodeInputRegressed(DataEngine& engine, const DerivedNode& node) {
  if (!node.is_mimo) {
    const TopicStorage* in_storage = engine.getTopicStorage(node.siso_input_topic_id);
    if (in_storage == nullptr) {
      return false;
    }
    // Walk unprocessed chunks in commit order: each must start at or after
    // everything fed so far (including its predecessors in this same batch).
    PJ::Timestamp watermark = node.siso_last_ts;
    for (const TopicChunk& chunk : in_storage->sealedChunks()) {
      if (chunk.id <= node.last_processed_chunk_id || chunk.stats.row_count == 0) {
        continue;
      }
      if (chunk.stats.t_min < watermark) {
        return true;
      }
      watermark = std::max(watermark, chunk.stats.t_max);
    }
    return false;
  }
  for (PJ::TopicId in_tid : node.mimo_input_topic_ids) {
    const TopicStorage* in_storage = engine.getTopicStorage(in_tid);
    if (in_storage == nullptr) {
      continue;
    }
    for (const TopicChunk& chunk : in_storage->sealedChunks()) {
      // <= : the MIMO gather skips rows at the watermark timestamp, so a late
      // row exactly at it would otherwise be lost.
      if (chunk.id > node.mimo_last_chunk_id && chunk.stats.row_count > 0 && chunk.stats.t_min <= node.mimo_last_ts) {
        return true;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// scheduleAll / scheduleActive
// ---------------------------------------------------------------------------

PJ::Status DerivedEngine::scheduleAll() {
  return scheduleActive({});
}

PJ::Status DerivedEngine::scheduleActive(const std::unordered_set<PJ::NodeId>& active_nodes) {
  // Hold the engine exclusively for the whole pass: the leaf run/regress/batch
  // helpers read source chunks and commit derived outputs via NON-locking engine
  // ops, so one unique lock makes the recompute atomic against the streaming
  // worker (DerivedEngine runs only on the GUI thread, so this blocks only the
  // worker, briefly). See engine.hpp lockEngine() / commitChunksLocked().
  auto lock = engine_.lockEngine();
  return scheduleActiveLocked(active_nodes);
}

PJ::Status DerivedEngine::scheduleActiveLocked(const std::unordered_set<PJ::NodeId>& active_nodes) {
  auto order = topologicalOrder();

  // Compute the set of nodes to consider (active_nodes ∪ their transitive upstream deps).
  tsl::robin_set<PJ::NodeId> filter;
  if (!active_nodes.empty()) {
    std::queue<PJ::NodeId> bfs;
    for (PJ::NodeId n : active_nodes) {
      if (impl_->nodes.contains(n)) {
        filter.insert(n);
        bfs.push(n);
      }
    }
    while (!bfs.empty()) {
      PJ::NodeId curr = bfs.front();
      bfs.pop();
      auto nit = impl_->nodes.find(curr);
      if (nit == impl_->nodes.end()) {
        continue;
      }
      for (PJ::TopicId in_tid : nit->second.all_input_topic_ids) {
        auto prod_it = impl_->output_topic_to_node.find(in_tid);
        if (prod_it == impl_->output_topic_to_node.end()) {
          continue;
        }
        PJ::NodeId prod = prod_it->second;
        if (filter.insert(prod).second) {
          bfs.push(prod);
        }
      }
    }
  }

  // The first per-node failure is remembered and returned, but a failed node must
  // NOT abort the others — one bad filter must not freeze every derived series.
  PJ::Status first_error = PJ::okStatus();
  for (PJ::NodeId node_id : order) {
    if (!active_nodes.empty() && !filter.contains(node_id)) {
      continue;
    }

    auto& node = impl_->nodes.at(node_id);
    if (!node.dirty) {
      continue;
    }

    PJ::Status s = PJ::okStatus();
    if (nodeInputRegressed(engine_, node)) {
      // Late (out-of-order) input behind the node's watermark: reset + full
      // replay over the now time-merged input instead of incremental work.
      s = recomputeBatchLocked(node_id);  // already holding engine_.lockEngine()
    } else if (!node.is_mimo) {
      s = runSisoIncremental(*impl_, engine_, node);
    } else {
      s = runMimoIncremental(*impl_, engine_, node);
    }

    node.dirty = false;  // processed even on failure, so a sticky-failed node does
                         // not re-run (and block healthy nodes) every commit
    if (!s.has_value()) {
      // Surface the failure via the returned Status, but keep running the other
      // nodes. The failed node's downstream is left as-is (its input is bad).
      if (first_error.has_value()) {
        first_error = std::move(s);  // remember only the first
      }
      continue;
    }

    // Propagate dirty to downstream nodes
    auto dit = impl_->downstream_of.find(node_id);
    if (dit != impl_->downstream_of.end()) {
      for (PJ::NodeId downstream : dit->second) {
        auto dnit = impl_->nodes.find(downstream);
        if (dnit != impl_->nodes.end()) {
          dnit.value().dirty = true;
        }
      }
    }
  }

  return first_error;
}

// ---------------------------------------------------------------------------
// recompute_batch
// ---------------------------------------------------------------------------

// Clear ONE node's output, reset its op + watermarks, and fully replay its input. The
// per-node primitive behind recomputeBatch's self+downstream cascade — does NOT touch
// downstream nodes. clearChunks (not retireTopic) keeps every TopicStorage alive so cached
// reader pointers stay valid.
static PJ::Status recomputeNodeSelfOnly(DerivedEngineImpl& impl, DataEngine& engine, DerivedNode& node) {
  // 1. Clear all output chunks unconditionally.
  for (PJ::TopicId out_tid : node.output_topic_ids) {
    TopicStorage* storage = engine.getTopicStorage(out_tid);
    if (storage) {
      storage->clearChunks();
    }
  }

  // 2. Reset transform state
  if (!node.is_mimo) {
    if (node.siso_op) {
      node.siso_op->reset();
    }
  } else {
    if (node.mimo_op) {
      node.mimo_op->reset();
    }
  }

  // 3. Reset processed watermarks (chunk ids and timestamps)
  node.last_processed_chunk_id = 0;
  node.siso_last_ts = std::numeric_limits<PJ::Timestamp>::min();
  if (node.is_mimo) {
    node.mimo_last_ts = std::numeric_limits<PJ::Timestamp>::min();
    node.mimo_last_chunk_id = 0;
  }

  // 4. Full replay
  PJ::Status s = PJ::okStatus();
  if (!node.is_mimo) {
    s = runSisoIncremental(impl, engine, node);
  } else {
    s = runMimoIncremental(impl, engine, node);
  }

  if (!s.has_value()) {
    return s;
  }
  node.dirty = false;
  return PJ::okStatus();
}

PJ::Status DerivedEngine::recomputeBatch(PJ::NodeId node_id) {
  auto lock = engine_.lockEngine();  // exclusive for the whole clear+replay cascade
  return recomputeBatchLocked(node_id);
}

PJ::Status DerivedEngine::recomputeBatchLocked(PJ::NodeId node_id) {
  auto it = impl_->nodes.find(node_id);
  if (it == impl_->nodes.end()) {
    return PJ::unexpected(fmt::format("recompute_batch: node {} not found", node_id));
  }

  // Recompute the target node itself first. If it fails, its downstream inputs are bad —
  // stop and surface the error (don't cascade onto an empty/failed upstream).
  PJ::Status self = recomputeNodeSelfOnly(*impl_, engine_, it.value());
  if (!self.has_value()) {
    return self;
  }

  // Cascade: a rewritten output must propagate to every chained filter downstream (a filter
  // of a filter), else editing/reloading the upstream leaves the downstream stale. Collect
  // the transitive downstream set (BFS over downstream_of), then recompute each in
  // topological order via a FULL reset+replay — never the incremental path, whose watermark
  // would skip the rewritten upstream output. Mirror scheduleActive's resilience: keep
  // cascading the rest of the set and return only the first error.
  tsl::robin_set<PJ::NodeId> reachable;
  std::queue<PJ::NodeId> bfs;
  bfs.push(node_id);
  while (!bfs.empty()) {
    PJ::NodeId curr = bfs.front();
    bfs.pop();
    auto dit = impl_->downstream_of.find(curr);
    if (dit == impl_->downstream_of.end()) {
      continue;
    }
    for (PJ::NodeId down : dit->second) {
      if (impl_->nodes.contains(down) && reachable.insert(down).second) {
        bfs.push(down);
      }
    }
  }
  if (reachable.empty()) {
    return PJ::okStatus();
  }

  PJ::Status first_error = PJ::okStatus();
  for (PJ::NodeId nid : topologicalOrder()) {
    if (!reachable.contains(nid)) {
      continue;
    }
    PJ::Status s = recomputeNodeSelfOnly(*impl_, engine_, impl_->nodes.at(nid));
    if (!s.has_value() && first_error.has_value()) {
      first_error = std::move(s);  // remember only the first downstream error
    }
  }
  return first_error;
}

// ---------------------------------------------------------------------------
// replace_siso_transform
// ---------------------------------------------------------------------------

PJ::Status DerivedEngine::replaceSisoTransform(PJ::NodeId node_id, std::unique_ptr<ISISOTransform> op) {
  if (!op) {
    return PJ::unexpected("replace_siso_transform: null transform op");
  }
  auto it = impl_->nodes.find(node_id);
  if (it == impl_->nodes.end()) {
    return PJ::unexpected(fmt::format("replace_siso_transform: node {} not found", node_id));
  }
  DerivedNode& node = it.value();
  if (node.is_mimo) {
    return PJ::unexpected(fmt::format("replace_siso_transform: node {} is not a SISO node", node_id));
  }
  node.siso_op = std::move(op);
  // recomputeBatch clears the (same) output topic, resets state, and replays.
  return recomputeBatch(node_id);
}

}  // namespace PJ
