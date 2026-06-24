#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <string>
#include <vector>

#include "pj_base/types.hpp"

namespace PJ {

/// One source dataset to fold into the anchor in DataEngine::mergeDatasets, with
/// the raw timestamp shift to add to every one of its samples before folding.
/// The engine is offset-agnostic: the host bakes display-alignment into this
/// shift (typically `display_offset(anchor) - display_offset(source)`), so the
/// merged data lands exactly where the user arranged it.
struct DatasetMergeSource {
  DatasetId dataset_id = 0;
  Timestamp raw_shift_ns = 0;
};

/// Outcome of DataEngine::mergeDatasets. All topic ids are ANCHOR ids — the
/// anchor's topics keep their ids so existing curve keys survive the merge.
struct DatasetMergeReport {
  /// Existing anchor topics whose data was rebuilt to include folded sources.
  std::vector<TopicId> modified_topics;
  /// Topics newly created under the anchor (a name only the sources had).
  std::vector<TopicId> added_topics;
  /// Source datasets that were emptied (their topics' chunks cleared). The
  /// datasets stay registered; the host removes them from its catalog.
  std::vector<DatasetId> consumed_datasets;
  /// Names of shared topics where a source disagreed with the anchor on a
  /// field's storage type: that source's contribution to the topic was skipped
  /// (the anchor's version is preserved) rather than silently corrupting reads.
  std::vector<std::string> skipped_topics;
};

}  // namespace PJ
