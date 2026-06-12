// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/sequential_uid.hpp"

#include <atomic>

namespace PJ {
namespace {

std::atomic<uint64_t> g_next_sequential_uid{SequentialUID::kFirstValidValue};

}  // namespace

SequentialUID SequentialUID::getNext() noexcept {
  uint64_t value = g_next_sequential_uid.fetch_add(1, std::memory_order_relaxed);
  if (value == kInvalidValue) {
    value = g_next_sequential_uid.fetch_add(1, std::memory_order_relaxed);
  }
  return SequentialUID{value};
}

}  // namespace PJ
