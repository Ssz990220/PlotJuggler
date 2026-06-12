#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <compare>
#include <cstdint>

namespace PJ {

/// Stable entry identity. Value 0 is reserved as the invalid/default sentinel;
/// real entries start at 1 and are monotonically increasing. Unlike a deque
/// index, a SequentialUID is not reused when retention evicts old entries.
///
/// Allocation is process-global across all topics, so consecutive entries of
/// one topic are NOT consecutive integers — never iterate a topic by
/// incrementing values; step with ObjectStore::nextUIDAfter() instead.
struct SequentialUID {
  static constexpr uint64_t kInvalidValue = 0;
  static constexpr uint64_t kFirstValidValue = 1;

  uint64_t value = kInvalidValue;

  [[nodiscard]] bool valid() const {
    return value != kInvalidValue;
  }

  /// Thread-safe process-wide UID creation.
  [[nodiscard]] static SequentialUID getNext() noexcept;

  friend auto operator<=>(SequentialUID, SequentialUID) = default;
};

}  // namespace PJ
