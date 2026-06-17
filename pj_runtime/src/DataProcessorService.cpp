// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/DataProcessorService.h"

#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pj_datastore/data_processor.hpp"
#include "pj_datastore/derived_engine.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/processor_siso_adapter.hpp"
#include "pj_scripting/filter_catalogue.h"
#include "pj_scripting/lua_siso_transform.h"
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
  if (changed_inputs.empty() || recipes_.empty()) {
    return {};
  }
  // Mark dependent nodes dirty for the just-committed inputs, then run them.
  // scheduleAll only re-runs dirty nodes (incremental watermark), so a stateful
  // node folds just the new tail and its accumulator persists across commits.
  derived_->onSourceCommitted(changed_inputs);
  // Best-effort: a processor/script failure must not abort ingest. The node
  // surfaces its own failed state; we still notify whatever advanced.
  (void)derived_->scheduleAll();

  // Re-notify every filter output (covers chained filters too); plots re-read.
  std::vector<TopicId> outputs;
  outputs.reserve(recipes_.size());
  for (const auto& [out_tid, recipe] : recipes_) {
    (void)recipe;
    outputs.push_back(out_tid);
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

}  // namespace PJ
