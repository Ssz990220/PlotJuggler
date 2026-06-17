#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"

namespace PJ {

class DataEngine;
class DerivedEngine;
namespace proc {
class DataProcessor;
}
namespace scripting {
class FilterCatalogue;
}

/// Host service that applies "Data Processors" (filters / transforms) to
/// datastore topics by running them as EAGER `pj_datastore::DerivedEngine` nodes
/// (a `PJ::proc::ProcessorSisoAdapter` wraps each native processor). Owns one
/// `DerivedEngine` bound to the session's `DataEngine`.
///
/// v1 scope (filters-first, file data): `applyFilter` registers a node, runs the
/// processor over all currently-committed input rows, and materializes a new
/// output topic. Streaming/pause (incremental `advanceOnCommit`) and recipe
/// persistence (layout save/restore) are handled; hidden-output tiers and
/// promotion remain later milestones.
class DataProcessorService {
 public:
  /// A filter applied to one input topic: the engine node id plus the
  /// materialized output topic it produced.
  struct FilterHandle {
    NodeId node_id = 0;
    TopicId output_topic_id = 0;
    DatasetId dataset_id = 0;
    std::string output_name;
  };

  /// Stored description of a live filter, keyed by its output topic — enough to
  /// re-open the editor on the output, repopulate the parameter panels, and edit
  /// the filter in place. Holds the live configured processor (see D2/§9).
  struct FilterRecipe {
    NodeId node_id = 0;
    TopicId output_topic_id = 0;
    TopicId input_topic_id = 0;
    std::size_t input_column_index = 0;
    DatasetId dataset_id = 0;
    std::string processor_id;  ///< builtin id (e.g. "scale") — selects the panel
    std::string output_name;
    std::shared_ptr<proc::DataProcessor> processor;  ///< live processor, for re-edit
    /// Full Luau module source the filter class was parsed from, captured from the
    /// catalogue so a layout can embed it as `<source_fallback>` (M7) and reopen on
    /// a machine without the filter installed. Empty for native C++ builtins (which
    /// round-trip by `processor_id` alone).
    std::string filter_source;
  };

  explicit DataProcessorService(DataEngine& engine);
  ~DataProcessorService();
  DataProcessorService(const DataProcessorService&) = delete;
  DataProcessorService& operator=(const DataProcessorService&) = delete;

  /// Apply a filter by its catalogue `processor_id` (e.g. "absolute",
  /// "moving_average") to column `input_column_index` (default 0) of
  /// `input_topic_id`, materializing a new output topic named `output_name` in
  /// `dataset_id`, then running it over all committed input rows. The column
  /// index selects one field of a multi-column topic (e.g. `/imu/.../x`).
  /// Returns the handle, or an error string (unknown id / engine rejection /
  /// empty output / column out of range).
  [[nodiscard]] Expected<FilterHandle> applyFilter(
      TopicId input_topic_id, DatasetId dataset_id, std::string_view processor_id, std::string output_name,
      std::size_t input_column_index = 0);

  /// Apply an already-CONFIGURED processor (params set by the caller, e.g. the
  /// Filter Editor dialog) to column `input_column_index` of the input topic.
  /// Same materialize-and-run semantics as the by-id overload; takes ownership
  /// of `processor`.
  [[nodiscard]] Expected<FilterHandle> applyFilter(
      TopicId input_topic_id, DatasetId dataset_id, std::unique_ptr<proc::DataProcessor> processor,
      std::string output_name, std::size_t input_column_index = 0);

  /// Drop a previously applied filter's engine node. (Output-topic teardown in
  /// the DataEngine is a later milestone; the materialized topic currently stays.)
  Status removeFilter(NodeId node_id);

  /// Remove EVERY live filter at once: for each recipe, drop its engine node AND
  /// `retireTopic` its materialized output (so `CatalogModel::rebuildFromDatastore`
  /// stops listing it — `removeNode` alone leaves a catalog zombie), then forget all
  /// recipes. The reconcile primitive behind snapshot restore (undo/redo + layout
  /// load): clear, then re-apply the snapshot's set, so a restored state can never
  /// duplicate or leak a filter. The output `TopicStorage` is kept alive (only
  /// retired), so any cached reader pointer sees an empty deque, not freed memory.
  /// Idempotent; a no-op when no filters are registered.
  void clearAllFilters();

  /// The recipe behind a filter output topic, or nullptr if `output_topic_id` is
  /// not a known filter output. Lets the Filter Editor re-open on an existing
  /// filtered curve and restore its parameters.
  [[nodiscard]] const FilterRecipe* filterConfig(TopicId output_topic_id) const;

  /// Replace a live filter's processor IN PLACE (same node + same output topic)
  /// and recompute — the editor's "apply an edit" path, so the plotted curve
  /// updates without re-binding. Updates the stored recipe. Takes a shared
  /// processor so the recipe can keep editing it later.
  [[nodiscard]] Status updateFilter(NodeId node_id, std::shared_ptr<proc::DataProcessor> processor);

  /// Drive the eager engine after a commit and BEFORE retention eviction — the
  /// streaming correctness order (plan §5/D6). Advances all live filter nodes
  /// over the newly committed input so stateful processors (integral, …) consume
  /// every sample before it can be evicted, then returns the filter output topics
  /// for the caller to re-notify (so plots refresh). MUST be called between the
  /// raw commit and `DataEngine::enforceRetention`. Best-effort: never throws
  /// mid-ingest. Returns empty if no filters are registered.
  [[nodiscard]] std::vector<TopicId> advanceOnCommit(const std::vector<TopicId>& changed_inputs);

  /// Reset + replay every filter whose INPUT topic is in `replaced_inputs`. A dataset
  /// reload swaps the input chunks WHOLESALE, so the derived output must be cleared
  /// and recomputed from scratch, NOT appended (which would leave stale old output).
  /// Returns the affected output topic ids for the caller to re-notify (so plots
  /// refresh). No-op if no filter reads a replaced input.
  [[nodiscard]] std::vector<TopicId> recomputeForReplacedSources(const std::vector<TopicId>& replaced_inputs);

  /// Snapshot of every live filter recipe (unspecified order) — for layout
  /// persistence: the host serializes these and re-applies them on load.
  [[nodiscard]] std::vector<FilterRecipe> recipes() const;

  /// (id, human-label) for every catalogued filter, in catalogue order — for
  /// populating a menu/dialog without the UI touching the script engine directly.
  /// Empty when no catalogue is installed.
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> availableFilters() const;

  /// Install the Luau filter catalogue (set by the app at startup). The by-id
  /// `applyFilter` and `availableFilters` resolve through it; without it the service
  /// has no filters (the bundled resource always installs one in the real app).
  void setFilterCatalogue(std::shared_ptr<scripting::FilterCatalogue> catalogue);

  /// Resolve a persisted layout entry to a processor for restore, in priority
  /// order (never silently mis-binds): the live catalogue by `id` → the layout's
  /// embedded `source_fallback` (compiled through the sandboxed engine, even with
  /// no catalogue installed) → `nullptr` (the caller logs a skip). The embedded-
  /// source leg lets a layout open on a machine without the filter installed.
  [[nodiscard]] std::unique_ptr<proc::DataProcessor> makeRestoredProcessor(
      const std::string& id, const std::string& params_json, const std::string& source_fallback) const;

  [[nodiscard]] DerivedEngine& derivedEngine() noexcept {
    return *derived_;
  }

 private:
  DataEngine& engine_;  ///< engine derived_ writes into; needed to retire orphaned filter outputs on clear
  std::unique_ptr<DerivedEngine> derived_;
  std::unordered_map<TopicId, FilterRecipe> recipes_;             ///< output topic id -> live recipe
  std::shared_ptr<scripting::FilterCatalogue> filter_catalogue_;  ///< optional Luau filter source
};

}  // namespace PJ
