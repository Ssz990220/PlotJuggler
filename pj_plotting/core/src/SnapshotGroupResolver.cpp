// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/SnapshotGroupResolver.h"

#include <algorithm>
#include <limits>

namespace PJ {
namespace {

constexpr std::string_view kWildcard = "[:]";

// If `field_path` == "<prefix>[<i>]<suffix>" for some non-negative integer i,
// return i; otherwise nullopt. Parsing is exact (no substring/contains): the
// bracket must sit immediately after `prefix` and `suffix` must run to the end.
[[nodiscard]] std::optional<uint32_t> matchElementIndex(
    std::string_view field_path, std::string_view prefix, std::string_view suffix) {
  // Minimum shape is prefix + "[d]" + suffix.
  if (field_path.size() < prefix.size() + suffix.size() + 3) {
    return std::nullopt;
  }
  if (field_path.substr(0, prefix.size()) != prefix) {
    return std::nullopt;
  }
  std::size_t pos = prefix.size();
  if (field_path[pos] != '[') {
    return std::nullopt;
  }
  ++pos;

  const std::size_t digits_begin = pos;
  uint64_t value = 0;
  while (pos < field_path.size() && field_path[pos] >= '0' && field_path[pos] <= '9') {
    value = value * 10 + static_cast<uint64_t>(field_path[pos] - '0');
    if (value > std::numeric_limits<uint32_t>::max()) {
      return std::nullopt;  // implausibly large index — not a real element
    }
    ++pos;
  }
  if (pos == digits_begin) {
    return std::nullopt;  // no digits between the brackets
  }
  if (pos >= field_path.size() || field_path[pos] != ']') {
    return std::nullopt;
  }
  ++pos;

  if (field_path.substr(pos) != suffix) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(value);
}

}  // namespace

std::optional<std::pair<std::string_view, std::string_view>> splitWildcardPattern(std::string_view pattern) {
  const std::size_t at = pattern.find(kWildcard);
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  return std::pair<std::string_view, std::string_view>{
      pattern.substr(0, at), pattern.substr(at + kWildcard.size())};
}

std::vector<SnapshotElement> resolveSnapshotPattern(
    const std::vector<SnapshotColumn>& columns, std::string_view prefix, std::string_view suffix) {
  std::vector<SnapshotElement> resolved;
  resolved.reserve(columns.size());
  for (const SnapshotColumn& column : columns) {
    if (const auto index = matchElementIndex(column.field_path, prefix, suffix); index.has_value()) {
      resolved.push_back(SnapshotElement{.element_index = *index, .column_index = column.column_index});
    }
  }

  // Order by element index; on a duplicate element index keep the smallest column
  // index (a stable, deterministic choice — duplicates should not arise from a
  // well-formed flattened layout, but we don't want to depend on that).
  std::sort(resolved.begin(), resolved.end(), [](const SnapshotElement& a, const SnapshotElement& b) {
    if (a.element_index != b.element_index) {
      return a.element_index < b.element_index;
    }
    return a.column_index < b.column_index;
  });
  resolved.erase(
      std::unique(
          resolved.begin(), resolved.end(),
          [](const SnapshotElement& a, const SnapshotElement& b) { return a.element_index == b.element_index; }),
      resolved.end());
  return resolved;
}

std::vector<SnapshotElement> resolveSnapshotPattern(
    const std::vector<SnapshotColumn>& columns, std::string_view pattern) {
  const auto split = splitWildcardPattern(pattern);
  if (!split.has_value()) {
    return {};
  }
  return resolveSnapshotPattern(columns, split->first, split->second);
}

std::vector<std::string> enumerateSnapshotPatterns(const std::vector<SnapshotColumn>& columns) {
  std::vector<std::string> patterns;
  for (const SnapshotColumn& column : columns) {
    const std::string& path = column.field_path;
    for (std::size_t i = 0; i < path.size(); ++i) {
      if (path[i] != '[') {
        continue;
      }
      std::size_t j = i + 1;
      while (j < path.size() && path[j] >= '0' && path[j] <= '9') {
        ++j;
      }
      // A real numeric bracket "[<digits>]" — replace just this one with "[:]".
      if (j > i + 1 && j < path.size() && path[j] == ']') {
        patterns.push_back(path.substr(0, i) + "[:]" + path.substr(j + 1));
      }
    }
  }
  std::sort(patterns.begin(), patterns.end());
  patterns.erase(std::unique(patterns.begin(), patterns.end()), patterns.end());
  return patterns;
}

}  // namespace PJ
