// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/DataProcessorService.h"

#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pj_base/type_tree.hpp"
#include "pj_datastore/data_processor.hpp"
#include "pj_datastore/derived_engine.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/processor_siso_adapter.hpp"
#include "pj_datastore/sample.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_scripting/filter_catalogue.h"
#include "pj_scripting/lua_mimo_transform.h"
#include "pj_scripting/lua_siso_transform.h"
#include "pj_scripting/python_engine.h"
#include "pj_scripting/script_engine.h"

namespace PJ {

DataProcessorService::DataProcessorService(DataEngine& engine)
    : engine_(engine), derived_(std::make_unique<DerivedEngine>(engine)) {}

DataProcessorService::~DataProcessorService() = default;

Expected<DataProcessorService::FilterHandle> DataProcessorService::applyFilter(
    TopicId input_topic_id, DatasetId dataset_id, std::string_view processor_id, std::string output_name,
    std::size_t input_column_index) {
  // Resolve the id to a Luau filter class through the installed catalogue.
  std::unique_ptr<proc::DataProcessor> processor;
  if (filter_catalogue_ && filter_catalogue_->find(processor_id) != nullptr) {
    if (auto made = filter_catalogue_->makeProcessor(processor_id, "{}"); made.has_value()) {
      processor = std::move(made).value();
    }
  }
  if (!processor) {
    return PJ::unexpected("DataProcessorService: unknown processor id '" + std::string(processor_id) + "'");
  }
  return applyFilter(input_topic_id, dataset_id, std::move(processor), std::move(output_name), input_column_index);
}

Expected<DataProcessorService::FilterHandle> DataProcessorService::applyFilter(
    TopicId input_topic_id, DatasetId dataset_id, std::unique_ptr<proc::DataProcessor> processor,
    std::string output_name, std::size_t input_column_index) {
  if (!processor) {
    return PJ::unexpected("DataProcessorService: null processor");
  }

  // Shared so the host could later keep its own handle (preview/reconfigure);
  // the engine owns the adapter, the adapter shares the processor.
  std::shared_ptr<proc::DataProcessor> shared = std::move(processor);
  auto node = derived_->addSisoTransform(
      input_topic_id, output_name, dataset_id, std::make_unique<proc::ProcessorSisoAdapter>(shared),
      input_column_index);
  if (!node) {
    return PJ::unexpected(node.error());
  }

  // Eager run over the already-committed input (file path: a single full pass).
  derived_->onSourceCommitted(std::vector<TopicId>{input_topic_id});
  if (Status scheduled = derived_->scheduleAll(); !scheduled.has_value()) {
    // Roll back the just-registered node + output topic. Leaving them reserves the output
    // name forever (no recipe is stored yet, so clearAllFilters can't reach it). Capture the
    // outputs BEFORE removeNode — afterwards outputTopics(*node) returns {}.
    const std::vector<TopicId> outs = derived_->outputTopics(*node);
    (void)derived_->removeNode(*node);
    for (TopicId out : outs) {
      engine_.retireTopic(out);
    }
    return PJ::unexpected(scheduled.error());
  }

  const std::vector<TopicId> outputs = derived_->outputTopics(*node);
  if (outputs.empty()) {
    (void)derived_->removeNode(*node);
    return PJ::unexpected("DataProcessorService: processor produced no output topic");
  }

  // Record the recipe so the editor can re-open on this output and edit it.
  FilterRecipe recipe;
  recipe.node_id = *node;
  recipe.output_topic_id = outputs.front();
  recipe.input_topic_id = input_topic_id;
  recipe.input_column_index = input_column_index;
  recipe.dataset_id = dataset_id;
  recipe.processor_id = shared->id();
  recipe.output_name = output_name;
  recipe.processor = shared;
  // Capture the filter's defining module source from the LIVE processor so a saved
  // layout embeds it as <source_fallback> (M7) and reopens on a machine without this
  // filter installed. Reading it off the processor (not re-deriving from the
  // catalogue) keeps it authoritative even for a filter restored from an embedded
  // source whose id is absent from this machine's catalogue — so re-saving keeps it.
  // Native C++ builtins are not LuaSisoTransforms (no source) and round-trip by id.
  if (const auto* lua = dynamic_cast<const scripting::LuaSisoTransform*>(shared.get())) {
    recipe.filter_source = lua->sourceText();
  }
  recipes_[recipe.output_topic_id] = std::move(recipe);

  return FilterHandle{*node, outputs.front(), dataset_id, std::move(output_name)};
}

Status DataProcessorService::removeFilter(NodeId node_id) {
  for (auto it = recipes_.begin(); it != recipes_.end(); ++it) {
    if (it->second.node_id == node_id) {
      recipes_.erase(it);
      break;
    }
  }
  return derived_->removeNode(node_id);
}

void DataProcessorService::clearAllFilters() {
  for (const auto& [output_topic_id, recipe] : recipes_) {
    (void)output_topic_id;  // key == recipe.output_topic_id; iterate by recipe for clarity
    // Drop the engine node, then retire the materialized output so the next
    // CatalogModel::rebuildFromDatastore stops listing it. removeNode alone would
    // leave a catalog zombie (it does not touch DataEngine::retired_topic_ids).
    (void)derived_->removeNode(recipe.node_id);
    engine_.retireTopic(recipe.output_topic_id);
  }
  recipes_.clear();
}

std::vector<TopicId> DataProcessorService::advanceOnCommit(const std::vector<TopicId>& changed_inputs) {
  // Run whenever there is ANY eager node — per-curve filters OR named transforms
  // (createTransform). Streaming a transform-only session still needs this path,
  // so the early-out must consider transform_recipes_ too, not just recipes_.
  if (changed_inputs.empty() || (recipes_.empty() && transform_recipes_.empty())) {
    return {};
  }
  // Mark dependent nodes dirty for the just-committed inputs, then run them.
  // scheduleAll only re-runs dirty nodes (incremental watermark), so a stateful
  // node folds just the new tail and its accumulator persists across commits.
  derived_->onSourceCommitted(changed_inputs);
  // Best-effort: a processor/script failure must not abort ingest. The node
  // surfaces its own failed state; we still notify whatever advanced.
  (void)derived_->scheduleAll();

  // Re-notify every filter AND transform output (covers chained nodes too); plots re-read.
  std::vector<TopicId> outputs;
  outputs.reserve(recipes_.size() + transform_recipes_.size());
  for (const auto& [out_tid, recipe] : recipes_) {
    (void)recipe;
    outputs.push_back(out_tid);
  }
  for (const auto& [key, recipe] : transform_recipes_) {
    (void)key;
    for (const TopicId out_tid : recipe.output_topic_ids) {
      outputs.push_back(out_tid);
    }
  }
  return outputs;
}

std::vector<TopicId> DataProcessorService::recomputeForReplacedSources(const std::vector<TopicId>& replaced_inputs) {
  std::vector<TopicId> affected_outputs;
  if (replaced_inputs.empty() || recipes_.empty()) {
    return affected_outputs;
  }
  // A reload swaps a raw input's chunks wholesale; reset + replay every recipe whose input is
  // affected — TRANSITIVELY, so a filter-of-a-filter follows its parent (the chained recipe's
  // input is the parent's output topic, not a raw input). recipes_ is keyed by output_topic_id
  // and acyclic, so a fixpoint recomputes parents before children regardless of map order;
  // `done` bounds it. Every affected output is REPORTED so each plot refreshes (recomputeBatch
  // also cascades engine-downstream, but reporting the full set is the load-bearing guarantee).
  std::unordered_set<TopicId> affected(replaced_inputs.begin(), replaced_inputs.end());
  std::unordered_set<PJ::NodeId> done;
  bool progress = true;
  while (progress) {
    progress = false;
    for (auto& [out_tid, recipe] : recipes_) {
      if (done.count(recipe.node_id) > 0 || affected.count(recipe.input_topic_id) == 0) {
        continue;  // already done, or its input is not (yet) affected — a later pass may pick it up
      }
      (void)derived_->recomputeBatch(recipe.node_id);
      done.insert(recipe.node_id);
      affected.insert(out_tid);
      affected_outputs.push_back(out_tid);
      progress = true;
    }
  }
  return affected_outputs;
}

const DataProcessorService::FilterRecipe* DataProcessorService::filterConfig(TopicId output_topic_id) const {
  auto it = recipes_.find(output_topic_id);
  return it == recipes_.end() ? nullptr : &it->second;
}

std::vector<DataProcessorService::FilterRecipe> DataProcessorService::recipes() const {
  std::vector<FilterRecipe> out;
  out.reserve(recipes_.size());
  for (const auto& [out_tid, recipe] : recipes_) {
    (void)out_tid;
    out.push_back(recipe);
  }
  return out;
}

Status DataProcessorService::updateFilter(NodeId node_id, std::shared_ptr<proc::DataProcessor> processor) {
  if (!processor) {
    return PJ::unexpected("DataProcessorService: null processor");
  }
  if (Status replaced =
          derived_->replaceSisoTransform(node_id, std::make_unique<proc::ProcessorSisoAdapter>(processor));
      !replaced.has_value()) {
    return replaced;
  }
  for (auto& [out_tid, recipe] : recipes_) {
    if (recipe.node_id == node_id) {
      recipe.processor = processor;
      recipe.processor_id = processor->id();
      // Re-capture the defining source (like applyFilter) so a layout re-saved after
      // an in-place edit embeds the NEW filter's <source_fallback>, not the old one.
      if (const auto* lua = dynamic_cast<const scripting::LuaSisoTransform*>(processor.get())) {
        recipe.filter_source = lua->sourceText();
      } else {
        recipe.filter_source.clear();
      }
      break;
    }
  }
  return PJ::okStatus();
}

std::vector<std::pair<std::string, std::string>> DataProcessorService::availableFilters() const {
  std::vector<std::pair<std::string, std::string>> out;
  if (filter_catalogue_) {
    for (const scripting::CatalogueEntry& e : filter_catalogue_->entries()) {
      out.emplace_back(e.cls.id, e.cls.name);
    }
  }
  return out;
}

void DataProcessorService::setFilterCatalogue(std::shared_ptr<scripting::FilterCatalogue> catalogue) {
  filter_catalogue_ = std::move(catalogue);
}

std::unique_ptr<proc::DataProcessor> DataProcessorService::makeRestoredProcessor(
    const std::string& id, const std::string& params_json, const std::string& source_fallback) const {
  // 1. Live catalogue by id (the common case — bundled filters always resolve here).
  if (filter_catalogue_ && filter_catalogue_->find(id) != nullptr) {
    if (auto made = filter_catalogue_->makeProcessor(id, params_json); made.has_value()) {
      return std::move(made).value();
    }
  }
  // 2. The layout's embedded source — self-sufficient, so it must restore even with
  //    NO catalogue installed (a layout that carries its own filter source). Compile
  //    it through the installed catalogue's engine when present, else a transient one
  //    (the built LuaSisoTransform shares ownership of the engine, so it outlives the
  //    temporary catalogue).
  if (!source_fallback.empty()) {
    auto made = filter_catalogue_ ? filter_catalogue_->makeProcessorFromSource(source_fallback, id, params_json)
                                  : scripting::FilterCatalogue(scripting::makeLuauEngine())
                                        .makeProcessorFromSource(source_fallback, id, params_json);
    if (made.has_value()) {
      return std::move(made).value();
    }
  }
  // Unresolved: the caller logs a skip-with-diagnostic.
  return nullptr;
}

// --- Plugin-created transforms (pj.data_processors.v1 host side) ---

namespace {
// Infer the backend from the payload's leading bytes / first-line directive
// (data-only ABI: nothing executable crosses, so the host picks the backend from
// the bytes). WASM/Python are recognized but diagnosed as unavailable in this build.
Expected<std::string> inferTransformBackend(const std::string& script) {
  if (script.size() >= 4 && script.compare(0, 4, "\0asm", 4) == 0) {
    return PJ::unexpected("pj.data_processors: WASM backend is reserved for a future host build");
  }
  const std::size_t nl = script.find('\n');
  const std::string first = script.substr(0, nl == std::string::npos ? script.size() : nl);
  if (first.find("pj-script: luau") != std::string::npos || first.find("pj-script: lua") != std::string::npos) {
    return std::string("luau");
  }
  if (first.find("pj-script: python") != std::string::npos) {
    return std::string("python");
  }
  return PJ::unexpected(
      "pj.data_processors: unrecognized script backend directive (expected '-- pj-script: <lang>' on line 1)");
}
}  // namespace

std::string DataProcessorService::makeTransformKey(std::string_view plugin_id, std::string_view id) {
  std::string key;
  key.reserve(plugin_id.size() + 1 + id.size());
  key.append(plugin_id);
  key.push_back('/');
  key.append(id);
  return key;
}

std::optional<std::pair<TopicId, DatasetId>> DataProcessorService::resolveInputTopic(const std::string& name) const {
  // No name->id index on the engine; scan live topics (retired ones are excluded
  // from listTopics, which is what we want — a stale output never resolves).
  for (const DatasetId ds : engine_.listDatasets()) {
    for (const TopicId tid : engine_.listTopics(ds)) {
      const TopicStorage* storage = engine_.getTopicStorage(tid);
      if (storage != nullptr && storage->descriptor().name == name) {
        return std::make_pair(tid, ds);
      }
    }
  }
  return std::nullopt;
}

std::optional<DataProcessorService::ResolvedInput> DataProcessorService::resolveInputField(
    const std::string& name) const {
  // Exact whole-topic name → the topic's first leaf column.
  if (const auto topic = resolveInputTopic(name)) {
    return ResolvedInput{topic->first, topic->second, 0};
  }
  // Otherwise treat the name as "<topic>/<field-path>": find the LONGEST live topic
  // whose name prefixes `name` (topic names can themselves contain '/'), then map the
  // trailing field path to its flattened-leaf column index.
  const TopicStorage* best_storage = nullptr;
  TopicId best_tid = 0;
  DatasetId best_ds = 0;
  std::string best_field;
  std::size_t best_topic_len = 0;
  for (const DatasetId ds : engine_.listDatasets()) {
    for (const TopicId tid : engine_.listTopics(ds)) {
      const TopicStorage* storage = engine_.getTopicStorage(tid);
      if (storage == nullptr) {
        continue;
      }
      const std::string& tn = storage->descriptor().name;
      if (name.size() > tn.size() + 1 && name.compare(0, tn.size(), tn) == 0 && name[tn.size()] == '/' &&
          tn.size() > best_topic_len) {
        best_topic_len = tn.size();
        best_storage = storage;
        best_tid = tid;
        best_ds = ds;
        best_field = name.substr(tn.size() + 1);
      }
    }
  }
  if (best_storage == nullptr) {
    return std::nullopt;
  }
  // Map the field path to its leaf column index. The dropped/display name separates
  // nested fields with '/', but both the type tree (flattenFieldPaths) and the stored
  // column descriptors use '.'-separated paths (e.g. "orientation.w"); normalize the
  // queried field to '.' before matching so a nested input resolves, not just a flat
  // one. The returned index is the engine's leaf-column order — exactly what
  // add_mimo_transform reads per input.
  std::string field_dotted = best_field;
  std::replace(field_dotted.begin(), field_dotted.end(), '/', '.');
  const auto matches = [&](const std::string& path) { return path == best_field || path == field_dotted; };

  // Schema-bearing topics (e.g. ROS/CDR): resolve through the TypeRegistry tree.
  const PJ::SchemaId schema_id = best_storage->descriptor().schema_id;
  if (schema_id != 0) {
    if (const PJ::TypeTreeNode* root = engine_.typeRegistry().lookup(schema_id)) {
      const std::vector<std::string> paths = PJ::flattenFieldPaths(*root);
      for (std::size_t i = 0; i < paths.size(); ++i) {
        if (matches(paths[i])) {
          return ResolvedInput{best_tid, best_ds, i};
        }
      }
    }
  }
  // Schema-less topics (schema_id == 0, e.g. JSON scalar-only ingest): the leaf names
  // live in the stored column descriptors, whose order is the engine column index.
  const auto& columns = best_storage->columnDescriptors();
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (matches(columns[i].field_path)) {
      return ResolvedInput{best_tid, best_ds, i};
    }
  }
  return std::nullopt;
}

bool DataProcessorService::outputNameInUse(const std::string& name, const std::string& except_key) const {
  // Another transform already outputs this name?
  for (const auto& [key, recipe] : transform_recipes_) {
    if (key == except_key) {
      continue;
    }
    if (std::find(recipe.outputs.begin(), recipe.outputs.end(), name) != recipe.outputs.end()) {
      return true;
    }
  }
  // A live engine topic (source / filter output / other node) has this name, and it
  // is not this transform's own current output (so an in-place replace is allowed)?
  std::unordered_set<TopicId> own_outputs;
  if (const auto it = transform_recipes_.find(except_key); it != transform_recipes_.end()) {
    own_outputs.insert(it->second.output_topic_ids.begin(), it->second.output_topic_ids.end());
  }
  for (const DatasetId ds : engine_.listDatasets()) {
    for (const TopicId tid : engine_.listTopics(ds)) {
      if (own_outputs.count(tid) > 0) {
        continue;
      }
      const TopicStorage* storage = engine_.getTopicStorage(tid);
      if (storage != nullptr && storage->descriptor().name == name) {
        return true;
      }
    }
  }
  return false;
}

Status DataProcessorService::validateScript(
    std::string_view script, std::string_view language, std::string_view params_json) {
  // Luau and Python backends are available; anything else is rejected.
  const std::string lang = language.empty() ? "luau" : std::string(language);
  if (lang != "luau" && lang != "python") {
    return PJ::unexpected("unsupported script language '" + lang + "' (only 'luau' or 'python' are available)");
  }
  const std::string src(script);
  const std::string params(params_json.empty() ? "{}" : std::string(params_json));
  // Compile through the matching backend. Python uses a transient catalogue (the
  // embedded interpreter is process-global); Luau reuses the installed catalogue.
  Expected<std::unique_ptr<proc::DataProcessor>> compiled =
      (lang == "python")  ? scripting::FilterCatalogue(scripting::makePythonEngine())
                                .makeProcessorFromSource(src, "__validate__", params)
      : filter_catalogue_ ? filter_catalogue_->makeProcessorFromSource(src, "__validate__", params)
                          : scripting::FilterCatalogue(scripting::makeLuauEngine())
                                .makeProcessorFromSource(src, "__validate__", params);
  if (!compiled.has_value()) {
    return PJ::unexpected(compiled.error());  // syntax / module-load error
  }
  // Compilation-only: do NOT run a synthetic test point. A SISO test sample carries
  // only (t, value), so a valid MULTI-INPUT function (using v1..vN) would see those
  // as nil and falsely report "no output". Runtime/empty-output is instead caught by
  // the live preview, which runs the real node with the real inputs.
  return PJ::okStatus();
}

namespace {

// A grouped MIMO output is requested EXPLICITLY as a single structured `outputs`
// entry "<topic>:<field1>,<field2>,...,<fieldN>": the host then materializes ONE
// topic named <topic> with those N named columns (column k = the script's k-th
// result), instead of N separate scalar topics. Returns {topic, [fields]} when the
// single output entry has that shape (non-empty topic + >= 1 non-empty field);
// otherwise {"", {}} and each output stays its own scalar topic. This keeps the
// intent unambiguous (unlike inferring a shared prefix) and entirely host-side —
// the plugin opts in by naming, e.g. outputs = {"rpy:roll,pitch,yaw"}.
std::pair<std::string, std::vector<std::string>> parseGroupedOutput(const std::vector<std::string>& outputs) {
  if (outputs.size() != 1) {
    return {"", {}};
  }
  const std::string& spec = outputs.front();
  const auto colon = spec.find(':');
  if (colon == std::string::npos || colon == 0) {
    return {"", {}};
  }
  const std::string topic = spec.substr(0, colon);
  std::vector<std::string> fields;
  std::size_t start = colon + 1;
  while (start <= spec.size()) {
    const std::size_t comma = spec.find(',', start);
    const std::size_t end = (comma == std::string::npos) ? spec.size() : comma;
    std::string field = spec.substr(start, end - start);
    const auto b = field.find_first_not_of(" \t");
    const auto e = field.find_last_not_of(" \t");
    if (b != std::string::npos) {
      fields.push_back(field.substr(b, e - b + 1));
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  if (fields.empty()) {
    return {"", {}};
  }
  return {topic, std::move(fields)};
}

}  // namespace

Expected<DataProcessorService::TransformRecipe> DataProcessorService::installTransform(TransformRecipe recipe) {
  // 1. Shape: N inputs -> M outputs (SISO is the 1->1 case). Field-level binding
  //    (a column within a multi-column input) is still a follow-up: inputs resolve
  //    to whole scalar topics here.
  if (recipe.inputs.empty() || recipe.outputs.empty()) {
    return PJ::unexpected("pj.data_processors: transform requires at least one input and one output");
  }
  for (const std::string& out_name : recipe.outputs) {
    if (out_name.empty()) {
      return PJ::unexpected("pj.data_processors: transform output name must be non-empty");
    }
  }
  // 2. Backend (also the missing/unavailable-backend diagnostic on restore).
  auto backend = inferTransformBackend(recipe.script);
  if (!backend.has_value()) {
    return PJ::unexpected(backend.error());
  }
  recipe.backend = std::move(backend).value();
  // 3. Output-name collision, per output (excludes this transform's own outputs, so a replace is allowed).
  for (const std::string& out_name : recipe.outputs) {
    if (outputNameInUse(out_name, recipe.key)) {
      return PJ::unexpected("pj.data_processors: output topic name already in use: " + out_name);
    }
  }
  // 4. Resolve every input by name; the output topics are created in the first input's dataset.
  std::vector<TopicId> input_topic_ids;
  std::vector<std::size_t> input_columns;  // leaf column per input (field-level binding)
  input_topic_ids.reserve(recipe.inputs.size());
  input_columns.reserve(recipe.inputs.size());
  for (const std::string& in_name : recipe.inputs) {
    const auto resolved = resolveInputField(in_name);
    if (!resolved.has_value()) {
      return PJ::unexpected("pj.data_processors: input topic/field not found: " + in_name);
    }
    if (input_topic_ids.empty()) {
      recipe.dataset_id = resolved->dataset_id;
    }
    input_topic_ids.push_back(resolved->topic_id);
    input_columns.push_back(resolved->column);
  }
  // Grouped output: a single structured "topic:f1,f2,..." entry collapses into ONE
  // multi-column topic (parsed host-side, explicit — no prefix guessing). A grouped
  // output is MIMO even when it is the only output entry, so it must not be mistaken
  // for SISO, and its arity (M) is the count of declared fields.
  const auto [mimo_group, mimo_fields] = parseGroupedOutput(recipe.outputs);
  const bool grouped = !mimo_group.empty();
  const bool is_siso = recipe.inputs.size() == 1 && recipe.outputs.size() == 1 && !grouped;
  const std::size_t mimo_num_outputs = grouped ? mimo_fields.size() : recipe.outputs.size();
  // 5. Compile BEFORE tearing down any old node, so a bad script never destroys a
  //    working transform. Use the catalogue's engine when installed, else a transient
  //    one (the built transform shares ownership of the engine, so it outlives it).
  std::shared_ptr<proc::DataProcessor> siso_op;          // set iff is_siso
  std::unique_ptr<scripting::LuaMimoTransform> mimo_op;  // set iff !is_siso
  // Python transforms compile through a transient Python catalogue (the embedded
  // interpreter is process-global). Luau reuses the installed catalogue.
  const bool is_python = recipe.backend == "python";
  if (is_siso) {
    auto compiled = is_python ? scripting::FilterCatalogue(scripting::makePythonEngine())
                                    .makeProcessorFromSource(recipe.script, recipe.user_id, recipe.params_json)
                    : filter_catalogue_
                        ? filter_catalogue_->makeProcessorFromSource(recipe.script, recipe.user_id, recipe.params_json)
                        : scripting::FilterCatalogue(scripting::makeLuauEngine())
                              .makeProcessorFromSource(recipe.script, recipe.user_id, recipe.params_json);
    if (!compiled.has_value()) {
      return PJ::unexpected(compiled.error());
    }
    siso_op = std::move(compiled).value();
  } else {
    auto compiled =
        is_python ? scripting::FilterCatalogue(scripting::makePythonEngine())
                        .makeMimoFromSource(recipe.script, recipe.user_id, recipe.params_json, mimo_num_outputs)
        : filter_catalogue_
            ? filter_catalogue_->makeMimoFromSource(recipe.script, recipe.user_id, recipe.params_json, mimo_num_outputs)
            : scripting::FilterCatalogue(scripting::makeLuauEngine())
                  .makeMimoFromSource(recipe.script, recipe.user_id, recipe.params_json, mimo_num_outputs);
    if (!compiled.has_value()) {
      return PJ::unexpected(compiled.error());
    }
    mimo_op = std::move(compiled).value();
    if (mimo_op->failed()) {  // a create()/compile error puts the instance in a sticky failed state
      return PJ::unexpected(mimo_op->error());
    }
  }
  // 6. Replace in place: drop the old node now that the new script compiled.
  if (transform_recipes_.count(recipe.key) > 0) {
    (void)removeTransform(recipe.key);
  }
  // 7. Install the eager node and run it over the committed input(s).
  // SISO column: an explicit recipe.input_column_index (the field the editor's drag
  // selected on a whole-topic input) takes precedence; otherwise fall back to the
  // column resolved from a "topic/field" input name (the plugin path embeds the field
  // in the name). The two are mutually exclusive in practice — only one caller sets a
  // non-zero column.
  const std::size_t siso_column = recipe.input_column_index != 0 ? recipe.input_column_index : input_columns.front();
  // A grouped MIMO output materializes ONE topic (mimo_group) with mimo_fields as named
  // columns; otherwise each recipe.output is its own scalar topic.
  const std::vector<std::string>& mimo_output_names = grouped ? mimo_fields : recipe.outputs;
  auto node = is_siso ? derived_->addSisoTransform(
                            input_topic_ids.front(), recipe.outputs.front(), recipe.dataset_id,
                            std::make_unique<proc::ProcessorSisoAdapter>(siso_op), siso_column)
                      : derived_->addMimoTransform(
                            input_topic_ids, mimo_output_names, recipe.dataset_id, std::move(mimo_op), input_columns,
                            mimo_group);
  if (!node.has_value()) {
    return PJ::unexpected(node.error());
  }
  derived_->onSourceCommitted(input_topic_ids);
  if (const Status scheduled = derived_->scheduleAll(); !scheduled.has_value()) {
    // Roll back the half-installed node + its output topic (capture outputs first).
    const std::vector<TopicId> outs = derived_->outputTopics(*node);
    (void)derived_->removeNode(*node);
    for (const TopicId out : outs) {
      engine_.retireTopic(out);
    }
    return PJ::unexpected(scheduled.error());
  }
  const std::vector<TopicId> outputs = derived_->outputTopics(*node);
  if (outputs.empty()) {
    (void)derived_->removeNode(*node);
    return PJ::unexpected("pj.data_processors: transform produced no output topic");
  }
  recipe.node_id = *node;
  recipe.output_topic_ids = outputs;
  transform_recipes_[recipe.key] = recipe;
  return recipe;
}

Expected<DataProcessorService::TransformRecipe> DataProcessorService::upsertTransform(
    std::string_view plugin_id, std::string_view id, std::vector<std::string> inputs, std::vector<std::string> outputs,
    std::string_view script, std::string_view params_json, bool ephemeral, std::size_t input_column_index) {
  TransformRecipe recipe;
  recipe.key = makeTransformKey(plugin_id, id);
  recipe.owner_plugin = std::string(plugin_id);
  recipe.user_id = std::string(id);
  recipe.inputs = std::move(inputs);
  recipe.outputs = std::move(outputs);
  recipe.script = std::string(script);
  recipe.params_json = std::string(params_json);
  recipe.ephemeral = ephemeral;
  recipe.input_column_index = input_column_index;
  return installTransform(std::move(recipe));
}

Status DataProcessorService::removeTransform(std::string_view namespaced_key) {
  const auto it = transform_recipes_.find(std::string(namespaced_key));
  if (it == transform_recipes_.end()) {
    return PJ::unexpected("pj.data_processors: unknown transform '" + std::string(namespaced_key) + "'");
  }
  const NodeId node = it->second.node_id;
  const std::vector<TopicId> outputs = it->second.output_topic_ids;
  transform_recipes_.erase(it);
  (void)derived_->removeNode(node);
  // Retire the materialized output so the catalog drops it (removeNode alone leaves a zombie).
  for (const TopicId out : outputs) {
    engine_.retireTopic(out);
  }
  return PJ::okStatus();
}

void DataProcessorService::clearTransformsForPlugin(std::string_view plugin_id) {
  std::vector<std::string> keys;
  for (const auto& [key, recipe] : transform_recipes_) {
    if (recipe.owner_plugin == plugin_id) {
      keys.push_back(key);
    }
  }
  for (const auto& key : keys) {
    (void)removeTransform(key);
  }
}

void DataProcessorService::clearAllTransforms() {
  std::vector<std::string> keys;
  keys.reserve(transform_recipes_.size());
  for (const auto& [key, recipe] : transform_recipes_) {
    (void)recipe;
    keys.push_back(key);
  }
  for (const auto& key : keys) {
    (void)removeTransform(key);
  }
}

void DataProcessorService::clearTransformsForDataset(DatasetId dataset_id) {
  std::vector<std::string> keys;
  for (const auto& [key, recipe] : transform_recipes_) {
    if (recipe.dataset_id == dataset_id) {
      keys.push_back(key);
    }
  }
  for (const auto& key : keys) {
    (void)removeTransform(key);
  }
}

std::vector<std::string> DataProcessorService::transformIdsForPlugin(std::string_view plugin_id) const {
  std::vector<std::string> ids;
  for (const auto& [key, recipe] : transform_recipes_) {
    (void)key;
    if (recipe.owner_plugin == plugin_id) {
      ids.push_back(recipe.user_id);
    }
  }
  return ids;
}

std::optional<std::string> DataProcessorService::transformRecipeJson(std::string_view namespaced_key) const {
  const auto it = transform_recipes_.find(std::string(namespaced_key));
  if (it == transform_recipes_.end()) {
    return std::nullopt;
  }
  const TransformRecipe& recipe = it->second;
  nlohmann::json doc;
  doc["inputs"] = recipe.inputs;
  doc["outputs"] = recipe.outputs;
  doc["backend"] = recipe.backend;
  // params is structured; embed it as an object (fall back to {} on malformed input).
  nlohmann::json params = nlohmann::json::parse(recipe.params_json, nullptr, /*allow_exceptions=*/false);
  doc["params"] = params.is_discarded() ? nlohmann::json::object() : params;
  return doc.dump();
}

std::vector<DataProcessorService::TransformRecipe> DataProcessorService::transformRecipes() const {
  std::vector<TransformRecipe> out;
  out.reserve(transform_recipes_.size());
  for (const auto& [key, recipe] : transform_recipes_) {
    (void)key;
    if (recipe.ephemeral) {
      continue;  // preview node — never persisted
    }
    out.push_back(recipe);
  }
  return out;
}

std::unordered_set<TopicId> DataProcessorService::ephemeralOutputTopics() const {
  std::unordered_set<TopicId> out;
  for (const auto& [key, recipe] : transform_recipes_) {
    (void)key;
    if (recipe.ephemeral) {
      out.insert(recipe.output_topic_ids.begin(), recipe.output_topic_ids.end());
    }
  }
  return out;
}

std::vector<DataProcessorService::TransformRecipe> DataProcessorService::transformsDependingOn(
    const std::vector<std::string>& removed_series) const {
  // Two names "touch" if one is the other, or one is a topic and the other a
  // "topic/field" under it (handles both topic-vs-field directions).
  const auto touches = [](const std::string& a, const std::string& b) {
    if (a == b) {
      return true;
    }
    if (b.size() > a.size() && b.compare(0, a.size(), a) == 0 && b[a.size()] == '/') {
      return true;
    }
    return a.size() > b.size() && a.compare(0, b.size(), b) == 0 && a[b.size()] == '/';
  };

  std::vector<std::string> affected(removed_series.begin(), removed_series.end());
  std::vector<TransformRecipe> result;
  std::unordered_set<std::string> seen_keys;

  // Fixpoint: a transform is pulled in when an input touches any affected name;
  // its own outputs then become affected, so children (derivative-of-derivative)
  // are caught on a later pass. Bounded by the number of recipes.
  bool grew = true;
  while (grew) {
    grew = false;
    for (const auto& [key, recipe] : transform_recipes_) {
      if (recipe.ephemeral || seen_keys.count(key) > 0) {
        continue;
      }
      bool depends = false;
      for (const auto& in : recipe.inputs) {
        for (const auto& name : affected) {
          if (touches(in, name)) {
            depends = true;
            break;
          }
        }
        if (depends) {
          break;
        }
      }
      if (depends) {
        result.push_back(recipe);
        seen_keys.insert(key);
        for (const auto& out : recipe.outputs) {
          affected.push_back(out);
        }
        grew = true;
      }
    }
  }
  return result;
}

Expected<DataProcessorService::TransformRecipe> DataProcessorService::restoreTransform(const TransformRecipe& recipe) {
  TransformRecipe restored = recipe;  // copy; installTransform re-derives the runtime fields
  if (restored.key.empty()) {
    restored.key = makeTransformKey(restored.owner_plugin, restored.user_id);
  }
  return installTransform(std::move(restored));
}

}  // namespace PJ
