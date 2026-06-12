// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/sequential_uid.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <thread>
#include <vector>

namespace PJ {
namespace {

TEST(SequentialUIDTest, GeneratedValuesAreValidAndIncreasing) {
  const SequentialUID first = SequentialUID::getNext();
  const SequentialUID second = SequentialUID::getNext();

  EXPECT_TRUE(first.valid());
  EXPECT_TRUE(second.valid());
  EXPECT_LT(first, second);
}

TEST(SequentialUIDTest, ConcurrentCreationIsUniqueAndNeverInvalid) {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 1000;
  constexpr int kTotal = kThreads * kPerThread;

  std::vector<uint64_t> values(kTotal);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    threads.emplace_back([thread_index, &values]() {
      const int offset = thread_index * kPerThread;
      for (int i = 0; i < kPerThread; ++i) {
        values[static_cast<std::size_t>(offset + i)] = SequentialUID::getNext().value;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  std::sort(values.begin(), values.end());
  for (std::size_t i = 0; i < values.size(); ++i) {
    EXPECT_NE(values[i], SequentialUID::kInvalidValue);
    if (i > 0) {
      EXPECT_LT(values[i - 1], values[i]);
    }
  }
}

}  // namespace
}  // namespace PJ
