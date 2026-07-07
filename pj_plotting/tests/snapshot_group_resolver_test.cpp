// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Tests for the snapshot wildcard resolver: expanding "<array>[:]<leaf>" patterns
// over a topic's flattened columns into ordered (element index -> column index)
// lists.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "pj_plotting/SnapshotGroupResolver.h"

namespace PJ {
namespace {

// Build the flattened columns of a spline-like topic:
//   predicted_trajectory[i].positions[j]   (i in [0,n_elements), j in [0,n_pos))
//   predicted_trajectory[i].time_from_start_s
// laid out element-major (all of element i's columns before element i+1's), the
// same order the datastore's flattener produces. Returns columns with ascending
// column indices. `sep` selects the struct separator so we can prove the resolver
// is separator-agnostic.
std::vector<SnapshotColumn> makeSplineColumns(int n_elements, int n_pos, const std::string& sep = ".") {
  std::vector<SnapshotColumn> columns;
  std::size_t col = 0;
  for (int i = 0; i < n_elements; ++i) {
    const std::string element = "predicted_trajectory[" + std::to_string(i) + "]";
    for (int j = 0; j < n_pos; ++j) {
      columns.push_back({col++, element + sep + "positions[" + std::to_string(j) + "]"});
    }
    columns.push_back({col++, element + sep + "time_from_start_s"});
  }
  return columns;
}

TEST(SnapshotGroupResolver, SplitWildcardPattern) {
  const auto split = splitWildcardPattern("predicted_trajectory[:].positions[3]");
  ASSERT_TRUE(split.has_value());
  EXPECT_EQ(split->first, "predicted_trajectory");
  EXPECT_EQ(split->second, ".positions[3]");

  EXPECT_FALSE(splitWildcardPattern("no_wildcard_here").has_value());
}

TEST(SnapshotGroupResolver, ResolvesOneLeafAcrossAllElementsInOrder) {
  const auto columns = makeSplineColumns(29, 8);
  const auto resolved = resolveSnapshotPattern(columns, "predicted_trajectory[:].positions[3]");

  ASSERT_EQ(resolved.size(), 29U);
  for (uint32_t i = 0; i < 29; ++i) {
    EXPECT_EQ(resolved[i].element_index, i);
    // Element stride = 8 positions + 1 stamp = 9; positions[3] sits at offset 3.
    EXPECT_EQ(resolved[i].column_index, static_cast<std::size_t>(i) * 9 + 3);
  }
}

TEST(SnapshotGroupResolver, ResolvesTheXLeaf) {
  const auto columns = makeSplineColumns(29, 8);
  const auto resolved = resolveSnapshotPattern(columns, "predicted_trajectory[:].time_from_start_s");
  ASSERT_EQ(resolved.size(), 29U);
  EXPECT_EQ(resolved[0].column_index, 8U);          // first element's stamp
  EXPECT_EQ(resolved[28].column_index, 28U * 9 + 8);  // last element's stamp
}

TEST(SnapshotGroupResolver, SeparatorAgnostic) {
  // Same data spelled with '/' between struct levels — resolver must still match,
  // because the caller-supplied suffix carries the separator verbatim.
  const auto columns = makeSplineColumns(4, 2, "/");
  const auto resolved = resolveSnapshotPattern(columns, "predicted_trajectory[:]/positions[1]");
  ASSERT_EQ(resolved.size(), 4U);
  EXPECT_EQ(resolved[0].column_index, 1U);  // element 0: positions[0]=0, positions[1]=1
}

TEST(SnapshotGroupResolver, ToleratesGaps) {
  // Elements 0, 1, 3 present; element 2's leaf column is missing (never expanded).
  std::vector<SnapshotColumn> columns = {
      {0, "arr[0].x"},
      {1, "arr[1].x"},
      {2, "arr[3].x"},
  };
  const auto resolved = resolveSnapshotPattern(columns, "arr[:].x");
  ASSERT_EQ(resolved.size(), 3U);
  EXPECT_EQ(resolved[0].element_index, 0U);
  EXPECT_EQ(resolved[1].element_index, 1U);
  EXPECT_EQ(resolved[2].element_index, 3U);  // gap at 2 preserved, not renumbered
  EXPECT_EQ(resolved[2].column_index, 2U);
}

TEST(SnapshotGroupResolver, SortsUnorderedInput) {
  std::vector<SnapshotColumn> columns = {
      {10, "arr[2].x"},
      {11, "arr[0].x"},
      {12, "arr[1].x"},
  };
  const auto resolved = resolveSnapshotPattern(columns, "arr[:].x");
  ASSERT_EQ(resolved.size(), 3U);
  EXPECT_EQ(resolved[0].element_index, 0U);
  EXPECT_EQ(resolved[0].column_index, 11U);
  EXPECT_EQ(resolved[1].element_index, 1U);
  EXPECT_EQ(resolved[2].element_index, 2U);
}

TEST(SnapshotGroupResolver, DoesNotMatchWrongPrefixOrSuffix) {
  std::vector<SnapshotColumn> columns = {
      {0, "predicted_trajectory[0].positions[3]"},
      {1, "predicted_trajectory_extra[0].positions[3]"},  // different prefix (no bracket after prefix)
      {2, "predicted_trajectory[0].positions[30]"},        // suffix differs (positions[30] != [3])
      {3, "predicted_trajectory[0].velocities[3]"},        // different leaf
  };
  const auto resolved = resolveSnapshotPattern(columns, "predicted_trajectory[:].positions[3]");
  ASSERT_EQ(resolved.size(), 1U);
  EXPECT_EQ(resolved[0].column_index, 0U);
}

TEST(SnapshotGroupResolver, WildcardAtRootPrefixEmpty) {
  std::vector<SnapshotColumn> columns = {
      {0, "[0].x"},
      {1, "[1].x"},
  };
  const auto resolved = resolveSnapshotPattern(columns, "[:].x");
  ASSERT_EQ(resolved.size(), 2U);
  EXPECT_EQ(resolved[0].element_index, 0U);
  EXPECT_EQ(resolved[1].element_index, 1U);
}

TEST(SnapshotGroupResolver, RejectsNonNumericIndex) {
  std::vector<SnapshotColumn> columns = {
      {0, "arr[].x"},    // empty brackets
      {1, "arr[x].x"},   // non-numeric
      {2, "arr[0].x"},   // valid
  };
  const auto resolved = resolveSnapshotPattern(columns, "arr[:].x");
  ASSERT_EQ(resolved.size(), 1U);
  EXPECT_EQ(resolved[0].element_index, 0U);
}

TEST(SnapshotGroupResolver, PatternWithoutWildcardResolvesEmpty) {
  const auto columns = makeSplineColumns(4, 2);
  EXPECT_TRUE(resolveSnapshotPattern(columns, "predicted_trajectory[0].positions[1]").empty());
}

}  // namespace
}  // namespace PJ
