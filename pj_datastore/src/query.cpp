// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/query.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

namespace PJ {
namespace {

[[nodiscard]] Range<Timestamp> normalized(Range<Timestamp> range) {
  if (range.max < range.min) {
    std::swap(range.min, range.max);
  }
  return range;
}

[[nodiscard]] bool isBoolColumn(const TopicChunk& chunk, std::size_t column_index) {
  return column_index < chunk.columns.size() && chunk.columns[column_index].descriptor &&
         chunk.columns[column_index].descriptor->logical_type == PrimitiveType::kBool;
}

[[nodiscard]] std::optional<double> readSeriesValue(
    const TopicChunk& chunk, std::size_t column_index, std::size_t row) {
  if (column_index >= chunk.columns.size() || row >= chunk.stats.row_count || chunk.isNull(column_index, row)) {
    return std::nullopt;
  }
  if (isBoolColumn(chunk, column_index)) {
    return chunk.readBool(column_index, row) ? 1.0 : 0.0;
  }
  return chunk.readNumericAsDouble(column_index, row);
}

[[nodiscard]] SeriesSample makeSeriesSample(const TopicChunk& chunk, std::size_t column_index, std::size_t row) {
  const auto value = readSeriesValue(chunk, column_index, row);
  assert(value.has_value());
  return SeriesSample{chunk.readTimestamp(row), *value, &chunk, row};
}

[[nodiscard]] Range<Timestamp> allTime() {
  return Range<Timestamp>{
      .min = std::numeric_limits<Timestamp>::min(),
      .max = std::numeric_limits<Timestamp>::max(),
  };
}

// Min-heap ordering for cursor frontiers: earliest (ts, chunk, row) on top.
[[nodiscard]] bool frontierAfter(const CursorFrontier& a, const CursorFrontier& b) {
  return std::tie(a.ts, a.chunk, a.row) > std::tie(b.ts, b.chunk, b.row);
}

// Replace the heap top with its in-chunk successor (has_next) or drop it.
// O(1) when the successor is still the global minimum — which is every
// advance when chunk ranges don't overlap; the heap only pays under overlap.
void replaceHeapTop(std::vector<CursorFrontier>& heap, const CursorFrontier& next, bool has_next) {
  if (has_next) {
    const std::size_t n = heap.size();
    const bool before_left = n <= 1 || !frontierAfter(next, heap[1]);
    const bool before_right = n <= 2 || !frontierAfter(next, heap[2]);
    if (before_left && before_right) {
      heap.front() = next;
      return;
    }
  }
  std::pop_heap(heap.begin(), heap.end(), frontierAfter);
  if (has_next) {
    heap.back() = next;
    std::push_heap(heap.begin(), heap.end(), frontierAfter);
  } else {
    heap.pop_back();
  }
}

}  // namespace

// ===========================================================================
// RangeCursor
// ===========================================================================

RangeCursor::RangeCursor(const std::deque<TopicChunk>& chunks, Timestamp t_min, Timestamp t_max)
    : chunks_(&chunks), t_min_(t_min), t_max_(t_max) {
  initFrontiers();
}

bool RangeCursor::valid() const noexcept {
  return !frontiers_.empty();
}

SampleRow RangeCursor::current() const {
  assert(valid());
  const CursorFrontier& top = frontiers_.front();
  return SampleRow{top.ts, &(*chunks_)[top.chunk], top.row};
}

void RangeCursor::advance() {
  assert(valid());
  CursorFrontier next = frontiers_.front();
  const TopicChunk& chunk = (*chunks_)[next.chunk];
  ++next.row;
  bool has_next = next.row < chunk.stats.row_count;
  if (has_next) {
    next.ts = chunk.readTimestamp(next.row);
    has_next = next.ts <= t_max_;
  }
  replaceHeapTop(frontiers_, next, has_next);
}

void RangeCursor::forEach(std::function<void(const SampleRow&)> callback) {
  while (valid()) {
    callback(current());
    advance();
  }
}

void RangeCursor::forEachChunk(std::function<void(const ChunkRowRange&)> callback) {
  for (const TopicChunk& chunk : *chunks_) {
    // No early break: chunk ranges may overlap, so a later chunk can still
    // intersect the query range even after one that lies past it.
    if (chunk.stats.row_count == 0 || chunk.stats.t_max < t_min_ || chunk.stats.t_min > t_max_) {
      continue;
    }
    const auto ts_begin = chunk.timestamps.begin();
    const auto ts_end = ts_begin + static_cast<std::ptrdiff_t>(chunk.stats.row_count);
    const auto first_it = std::lower_bound(ts_begin, ts_end, t_min_);
    const auto end_it = std::upper_bound(first_it, ts_end, t_max_);
    if (first_it != end_it) {
      callback(
          ChunkRowRange{
              &chunk, static_cast<std::size_t>(first_it - ts_begin), static_cast<std::size_t>(end_it - ts_begin)});
    }
  }
  // Mark cursor exhausted
  frontiers_.clear();
}

void RangeCursor::initFrontiers() {
  const auto& chunks = *chunks_;
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    const TopicChunk& chunk = chunks[i];
    if (chunk.stats.row_count == 0 || chunk.stats.t_max < t_min_ || chunk.stats.t_min > t_max_) {
      continue;
    }
    // First row with timestamp >= t_min_ (chunks are internally sorted).
    const auto ts_begin = chunk.timestamps.begin();
    const auto ts_end = ts_begin + static_cast<std::ptrdiff_t>(chunk.stats.row_count);
    const auto row_it = std::lower_bound(ts_begin, ts_end, t_min_);
    if (row_it == ts_end || *row_it > t_max_) {
      continue;
    }
    frontiers_.push_back(CursorFrontier{*row_it, i, static_cast<std::size_t>(row_it - ts_begin)});
  }
  std::make_heap(frontiers_.begin(), frontiers_.end(), frontierAfter);
}

// ===========================================================================
// latest_at
// ===========================================================================

std::optional<SampleRow> latestAt(const std::deque<TopicChunk>& chunks, Timestamp t) {
  // Chunk ranges may overlap (out-of-order ingest), so candidate chunks are
  // scanned rather than binary-searched; each candidate contributes its last
  // row with timestamp <= t (per-chunk binary search — chunks are internally
  // sorted). Later-committed chunks win timestamp ties, matching the
  // pre-overlap behaviour at shared chunk boundaries.
  std::optional<SampleRow> best;
  for (const TopicChunk& chunk : chunks) {
    if (chunk.stats.row_count == 0 || chunk.stats.t_min > t) {
      continue;
    }
    const auto ts_begin = chunk.timestamps.begin();
    const auto ts_end = ts_begin + static_cast<std::ptrdiff_t>(chunk.stats.row_count);
    const auto row_after = std::upper_bound(ts_begin, ts_end, t);
    if (row_after == ts_begin) {
      continue;  // unreachable for committed chunks (row 0 ts == t_min <= t)
    }
    const std::size_t row = static_cast<std::size_t>((row_after - 1) - ts_begin);
    const Timestamp ts = chunk.readTimestamp(row);
    if (!best.has_value() || ts >= best->timestamp) {
      best = SampleRow{ts, &chunk, row};
    }
  }
  return best;
}

// ===========================================================================
// range_query
// ===========================================================================

RangeCursor rangeQuery(const std::deque<TopicChunk>& chunks, Timestamp t_min, Timestamp t_max) {
  return RangeCursor(chunks, t_min, t_max);
}

// ===========================================================================
// SeriesCursor
// ===========================================================================

SeriesCursor::SeriesCursor(const std::deque<TopicChunk>& chunks, std::size_t column_index, Range<Timestamp> time_range)
    : chunks_(&chunks), column_index_(column_index), time_range_(normalized(time_range)) {
  initFrontiers();
}

bool SeriesCursor::valid() const noexcept {
  return !frontiers_.empty();
}

SeriesSample SeriesCursor::current() const {
  assert(valid());
  const CursorFrontier& top = frontiers_.front();
  return makeSeriesSample((*chunks_)[top.chunk], column_index_, top.row);
}

void SeriesCursor::advance() {
  assert(valid());
  CursorFrontier next = frontiers_.front();
  ++next.row;
  replaceHeapTop(frontiers_, next, nextSample(next));
}

void SeriesCursor::forEach(std::function<void(const SeriesSample&)> callback) {
  while (valid()) {
    callback(current());
    advance();
  }
}

bool SeriesCursor::nextSample(CursorFrontier& frontier) const {
  const TopicChunk& chunk = (*chunks_)[frontier.chunk];
  while (frontier.row < chunk.stats.row_count) {
    const Timestamp ts = chunk.readTimestamp(frontier.row);
    if (ts > time_range_.max) {
      return false;
    }
    if (ts >= time_range_.min && readSeriesValue(chunk, column_index_, frontier.row).has_value()) {
      frontier.ts = ts;
      return true;
    }
    ++frontier.row;
  }
  return false;
}

void SeriesCursor::initFrontiers() {
  const auto& chunks = *chunks_;
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    const TopicChunk& chunk = chunks[i];
    // column_index_ bound check: earlier chunks may predate a mid-stream
    // column addition. No early break — chunk ranges may overlap.
    if (chunk.stats.row_count == 0 || chunk.stats.t_max < time_range_.min || chunk.stats.t_min > time_range_.max ||
        column_index_ >= chunk.columns.size()) {
      continue;
    }
    const auto ts_begin = chunk.timestamps.begin();
    const auto ts_end = ts_begin + static_cast<std::ptrdiff_t>(chunk.stats.row_count);
    const auto row_it = std::lower_bound(ts_begin, ts_end, time_range_.min);
    CursorFrontier frontier{0, i, static_cast<std::size_t>(row_it - ts_begin)};
    if (nextSample(frontier)) {
      frontiers_.push_back(frontier);
    }
  }
  std::make_heap(frontiers_.begin(), frontiers_.end(), frontierAfter);
}

// ===========================================================================
// SeriesReader
// ===========================================================================

SeriesReader::SeriesReader(const std::deque<TopicChunk>& chunks, std::size_t column_index)
    : chunks_(&chunks), column_index_(column_index) {}

std::size_t SeriesReader::size() const {
  std::size_t count = 0;
  for (const TopicChunk& chunk : *chunks_) {
    if (column_index_ >= chunk.columns.size()) {
      continue;
    }
    for (std::size_t row = 0; row < chunk.stats.row_count; ++row) {
      if (readSeriesValue(chunk, column_index_, row).has_value()) {
        ++count;
      }
    }
  }
  return count;
}

bool SeriesReader::empty() const {
  return size() == 0;
}

std::optional<SeriesSample> SeriesReader::sampleAt(std::size_t index) const {
  std::size_t series_index = 0;
  for (const TopicChunk& chunk : *chunks_) {
    if (column_index_ >= chunk.columns.size()) {
      continue;
    }
    for (std::size_t row = 0; row < chunk.stats.row_count; ++row) {
      if (!readSeriesValue(chunk, column_index_, row).has_value()) {
        continue;
      }
      if (series_index == index) {
        return makeSeriesSample(chunk, column_index_, row);
      }
      ++series_index;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> SeriesReader::indexAtOrBeforeTime(Timestamp t) const {
  std::optional<std::size_t> latest;
  std::size_t series_index = 0;
  for (const TopicChunk& chunk : *chunks_) {
    if (chunk.stats.row_count == 0 || column_index_ >= chunk.columns.size()) {
      continue;
    }
    if (chunk.stats.t_min > t) {
      break;
    }
    for (std::size_t row = 0; row < chunk.stats.row_count; ++row) {
      const Timestamp ts = chunk.readTimestamp(row);
      if (ts > t) {
        return latest;
      }
      if (readSeriesValue(chunk, column_index_, row).has_value()) {
        latest = series_index;
        ++series_index;
      }
    }
  }
  return latest;
}

std::optional<std::size_t> SeriesReader::indexAtOrAfterTime(Timestamp t) const {
  std::size_t series_index = 0;
  for (const TopicChunk& chunk : *chunks_) {
    if (chunk.stats.row_count == 0 || column_index_ >= chunk.columns.size()) {
      continue;
    }
    if (chunk.stats.t_max < t) {
      for (std::size_t row = 0; row < chunk.stats.row_count; ++row) {
        if (readSeriesValue(chunk, column_index_, row).has_value()) {
          ++series_index;
        }
      }
      continue;
    }
    for (std::size_t row = 0; row < chunk.stats.row_count; ++row) {
      if (!readSeriesValue(chunk, column_index_, row).has_value()) {
        continue;
      }
      if (chunk.readTimestamp(row) >= t) {
        return series_index;
      }
      ++series_index;
    }
  }
  return std::nullopt;
}

std::optional<SeriesSample> SeriesReader::sampleAtOrBeforeTime(Timestamp t) const {
  const auto index = indexAtOrBeforeTime(t);
  return index.has_value() ? sampleAt(*index) : std::nullopt;
}

std::optional<SeriesSample> SeriesReader::sampleAtOrAfterTime(Timestamp t) const {
  const auto index = indexAtOrAfterTime(t);
  return index.has_value() ? sampleAt(*index) : std::nullopt;
}

SeriesCursor SeriesReader::samples(Range<Timestamp> time_range) const {
  return SeriesCursor(*chunks_, column_index_, time_range);
}

std::optional<SeriesBounds> SeriesReader::bounds() const {
  return bounds(allTime());
}

std::optional<SeriesBounds> SeriesReader::bounds(Range<Timestamp> time_range) const {
  SeriesBounds result;
  bool found_time = false;
  bool found_value = false;
  auto cursor = samples(time_range);
  cursor.forEach([&](const SeriesSample& sample) {
    if (!found_time) {
      result.time.min = sample.timestamp;
      result.time.max = sample.timestamp;
      found_time = true;
    } else {
      result.time.min = std::min(result.time.min, sample.timestamp);
      result.time.max = std::max(result.time.max, sample.timestamp);
    }

    if (std::isfinite(sample.value)) {
      if (!found_value) {
        result.value.min = sample.value;
        result.value.max = sample.value;
        found_value = true;
      } else {
        result.value.min = std::min(result.value.min, sample.value);
        result.value.max = std::max(result.value.max, sample.value);
      }
    }
    ++result.sample_count;
  });

  if (!found_time || !found_value) {
    return std::nullopt;
  }
  return result;
}

}  // namespace PJ
