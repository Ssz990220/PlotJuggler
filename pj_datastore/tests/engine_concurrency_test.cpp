// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Reproduces the worker-write vs GUI-read data race that progressive file
// loading exposed: a worker thread commits chunks (sealed_chunks_.push_back)
// while a reader thread iterates the same deque via getMetadata()/rangeQuery().
// Without engine synchronization this is a std::deque data race (UB) and
// segfaults under load. With the engine's reader-writer lock it must run to
// completion with consistent results.
//
// This is the GREEN GATE for the DataEngine thread-safety work: before the lock
// lands it SEGFAULTS (exit 139) under the concurrent commit/read; after it must
// run to completion with a consistent final row count.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <latch>
#include <limits>
#include <thread>

#include "pj_base/dataset.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {
namespace {

TEST(EngineConcurrencyTest, ConcurrentCommitAndReadDoesNotRace) {
  DataEngine engine;
  auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "race", .time_domain_id = 0});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  DataWriter setup = engine.createWriter();
  auto handle = setup.registerScalarSeries(*dataset, "v", NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  const TopicId topic = handle->topic_id;

  // Bound BOTH the fill and the reader passes so runtime is deterministic, not
  // fairness-dependent: each full-range read serializes against the writer, so an
  // unbounded reader is O(passes * N) where `passes` depends on lock fairness —
  // which once blew a CORRECT build past the ctest TIMEOUT (~100s, a false
  // "deadlock"). The race (writer push_back vs a reader iterating the deque) fires
  // regardless of size, so the cap now only ever catches a true hang.
  constexpr int kBatches = 50;  // 3.2k samples
  constexpr int kSamplesPerBatch = 64;
  constexpr int kMaxReaderPasses = 1000;  // bounds the O(passes*N) reader walk
  std::atomic<bool> writer_done{false};
  std::atomic<int> completed_reads{0};
  // The writer blocks on this after its FIRST commit until the reader has
  // completed one pass, making the commit/read overlap deterministic instead of
  // scheduler-dependent: when both threads share a core (CI under `ctest -j`, a
  // single-vCPU runner, or `taskset -c 0`) the scheduler will otherwise run the
  // tiny writer to completion before the reader's first turn, leaving
  // completed_reads == 0 and false-failing the EXPECT_GT below on correct code.
  std::latch first_read_done{1};

  // Writer: commit `kBatches` chunks back-to-back (each commit push_backs a chunk).
  // After the first commit it waits for one reader pass (see `first_read_done`),
  // so batches 1..N-1 are guaranteed to overlap a live reader. The writer holds
  // NO engine lock while waiting, so a correct reader always makes progress; a
  // real lock-order deadlock instead hangs both threads and trips the 90s ctest
  // TIMEOUT — the loud-failure contract this test is registered with.
  std::thread writer([&]() {
    DataWriter w = engine.createWriter();
    const ScalarSeriesHandle wh{topic, 0};
    Timestamp t = 0;
    for (int batch = 0; batch < kBatches; ++batch) {
      for (int i = 0; i < kSamplesPerBatch; ++i) {
        w.appendScalar(wh, t++, 1.0);
      }
      engine.commitChunks(w.flushAll());
      if (batch == 0) {
        first_read_done.wait();
      }
      // Hand the core to the reader so the remaining commits interleave with
      // reads rather than draining in a single scheduling quantum.
      std::this_thread::yield();
    }
    writer_done.store(true, std::memory_order_release);
  });

  // Reader: read metadata + iterate the full range for up to kMaxReaderPasses
  // passes while the writer fills (heavy overlap either way), mirroring the GUI's
  // catalog rebuild + plot reads during a load. Each read holds the engine lock
  // for its whole forEach, so it can never observe a deque torn mid-push_back. The
  // cap bounds runtime; if the reader caps out first, the writer finishes the
  // remaining batches uncontended.
  std::thread reader([&]() {
    DataReader r = engine.createReader();
    for (int i = 0; i < kMaxReaderPasses && !writer_done.load(std::memory_order_acquire); ++i) {
      (void)r.getMetadata(topic);
      auto cursor = r.rangeQuery(
          QueryRange{
              .topic_id = topic,
              .t_min = std::numeric_limits<Timestamp>::min(),
              .t_max = std::numeric_limits<Timestamp>::max()});
      if (cursor.has_value()) {
        std::size_t rows = 0;
        cursor->forEach([&rows](const SampleRow&) { ++rows; });
      }
      // Release the writer's first-commit wait on the very first pass, so the
      // remaining writes overlap a confirmed-live reader (fetch_add returns the
      // prior value, so == 0 means "this is the first pass").
      if (completed_reads.fetch_add(1, std::memory_order_relaxed) == 0) {
        first_read_done.count_down();
      }
      // Yield so the writer (blocked on the same exclusive lock) gets a turn:
      // without this the reader re-acquires the lock before the writer's wakeup
      // wins, starving the writer's commit loop into an effective livelock.
      std::this_thread::yield();
    }
  });

  reader.join();
  writer.join();

  // The reader overlapped the writer (no crash / no timeout = reads serialized safely)...
  EXPECT_GT(completed_reads.load(), 0);
  // ...and after the run every committed row is visible and consistent.
  DataReader verify = engine.createReader();
  auto final_cursor = verify.rangeQuery(
      QueryRange{
          .topic_id = topic,
          .t_min = std::numeric_limits<Timestamp>::min(),
          .t_max = std::numeric_limits<Timestamp>::max()});
  ASSERT_TRUE(final_cursor.has_value());
  std::size_t total = 0;
  final_cursor->forEach([&total](const SampleRow&) { ++total; });
  EXPECT_EQ(total, static_cast<std::size_t>(kBatches) * kSamplesPerBatch);
}

}  // namespace
}  // namespace PJ
