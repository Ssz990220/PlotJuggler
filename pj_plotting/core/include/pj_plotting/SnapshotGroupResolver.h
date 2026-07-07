#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Wildcard column resolver for snapshot ("current message") plots. A snapshot
// curve reads one leaf field across every element of an array in a single
// message — e.g. positions[3] of predicted_trajectory[0..28]. This resolver
// expands a "<array>[:]<leaf>" pattern over a topic's flattened columns into an
// ordered (element index -> datastore column index) list. It is a pure function
// over column names and is deliberately separator-agnostic: the "[:]" wildcard is
// matched literally and whatever precedes/follows it (the caller's prefix and
// leaf suffix, '.' or '/' separators included) is compared verbatim. So it works
// regardless of how the catalog spells nested fields.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PJ {

// One flattened column of a topic as the resolver sees it: its datastore column
// index and full flattened field path (e.g. "predicted_trajectory[3].positions[5]").
struct SnapshotColumn {
  std::size_t column_index = 0;
  std::string field_path;
};

// One resolved array element: its index under the wildcard dimension and the
// datastore column index of the matched leaf.
struct SnapshotElement {
  uint32_t element_index = 0;
  std::size_t column_index = 0;
};

// Split `pattern` at its first "[:]" wildcard token into (prefix, suffix).
// nullopt when there is no "[:]" token. Example:
//   "predicted_trajectory[:].positions[3]" -> {"predicted_trajectory", ".positions[3]"}
[[nodiscard]] std::optional<std::pair<std::string_view, std::string_view>> splitWildcardPattern(
    std::string_view pattern);

// Expand a single "<prefix>[:]<suffix>" pattern over `columns`: match every column
// whose field_path is exactly "<prefix>[<i>]<suffix>" for some non-negative integer
// i, returning {element_index=i, column_index} for each match. The result is sorted
// ascending by element index and tolerant of gaps (a missing i is simply absent). A
// pattern with no "[:]" token yields an empty result. On a duplicate element index
// the smallest column index wins (deterministic; duplicates should not occur for a
// well-formed flattened layout).
[[nodiscard]] std::vector<SnapshotElement> resolveSnapshotPattern(
    const std::vector<SnapshotColumn>& columns, std::string_view pattern);

// Same, but with prefix/suffix already separated (the form callers use when they
// share one array prefix across an X leaf and several Y leaves).
[[nodiscard]] std::vector<SnapshotElement> resolveSnapshotPattern(
    const std::vector<SnapshotColumn>& columns, std::string_view prefix, std::string_view suffix);

// Enumerate every candidate "<prefix>[:]<suffix>" wildcard pattern implied by
// `columns`: for each "[<digits>]" bracket in each column's field_path, replace that
// one bracket with "[:]". Deduplicated and sorted. A column with several array
// dimensions (e.g. "predicted_trajectory[3]/positions[5]") yields one candidate per
// dimension, so the snapshot-group dialog can offer both "wildcard the trajectory"
// and "wildcard the joint" patterns. Used to populate the creation UI; the render
// path uses resolveSnapshotPattern once a specific pattern is chosen.
[[nodiscard]] std::vector<std::string> enumerateSnapshotPatterns(const std::vector<SnapshotColumn>& columns);

}  // namespace PJ
