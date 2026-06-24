#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <optional>
#include <vector>

#include "pj_base/types.hpp"  // PJ::DatasetId

class QWidget;

namespace PJ {

class AppSession;

/// Human-readable destructive-merge warning for `datasets`: a base "this is
/// destructive" line plus, when applicable, which datasets overlap in displayed
/// time, which of those also collide on a shared topic name, and which carry
/// object topics that the scalar-only v1 merge will drop. Reconstructed from the
/// runtime (catalog names, raw bounds, per-source display offset, engine topics),
/// so it is independent of any widget's local track list.
[[nodiscard]] QString composeDatasetMergeWarning(const AppSession& session, const std::vector<DatasetId>& datasets);

/// Confirm-then-merge entry point shared by the Source Timeline footer and the
/// curve tree's context menu. Filters `ids` to the data-bearing datasets; if ≥2
/// remain, shows the destructive-merge warning (composeDatasetMergeWarning) as a
/// modal question parented to `parent`. On confirm, runs AppSession::mergeDatasets
/// and returns the surviving anchor id. Returns nullopt on fewer than two
/// data-bearing datasets, on cancel/Esc, or a no-op merge.
[[nodiscard]] std::optional<DatasetId> confirmAndMergeDatasets(
    QWidget* parent, AppSession& session, const std::vector<DatasetId>& ids);

}  // namespace PJ
