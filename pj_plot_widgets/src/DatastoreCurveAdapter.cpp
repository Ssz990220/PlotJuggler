#include "pj_plot_widgets/DatastoreCurveAdapter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include "pj_app_core/SessionManager.h"
#include "pj_base/dataset.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/topic_storage.hpp"

namespace PJ {
namespace {

constexpr double kNanosecondsPerSecond = 1e9;

[[nodiscard]] QPointF invalidPoint() {
  return {0.0, std::numeric_limits<double>::quiet_NaN()};
}

[[nodiscard]] QRectF invalidRect() {
  return {1.0, 1.0, -2.0, -2.0};
}

// Caller is responsible for finite-checking display_sec — Qwt rects from real
// viewports are always bounded; the upstream call sites guard against NaN/inf.
[[nodiscard]] Timestamp displaySecondsToRawNs(double display_sec, Timestamp display_offset_ns) noexcept {
  return static_cast<Timestamp>(std::llround(display_sec * kNanosecondsPerSecond)) + display_offset_ns;
}

[[nodiscard]] double rawNsToDisplaySeconds(Timestamp raw_ns, Timestamp display_offset_ns) noexcept {
  return static_cast<double>(raw_ns - display_offset_ns) / kNanosecondsPerSecond;
}

[[nodiscard]] bool isAllRowsWindow(Timestamp t_min, Timestamp t_max) noexcept {
  return t_min == std::numeric_limits<Timestamp>::min() && t_max == std::numeric_limits<Timestamp>::max();
}

[[nodiscard]] std::size_t firstRowGreaterEqual(const TopicChunk& chunk, Timestamp t) {
  const auto begin = chunk.timestamps.begin();
  const auto end = begin + static_cast<std::ptrdiff_t>(chunk.stats.row_count);
  return static_cast<std::size_t>(std::lower_bound(begin, end, t) - begin);
}

[[nodiscard]] std::size_t firstRowGreater(const TopicChunk& chunk, Timestamp t) {
  const auto begin = chunk.timestamps.begin();
  const auto end = begin + static_cast<std::ptrdiff_t>(chunk.stats.row_count);
  return static_cast<std::size_t>(std::upper_bound(begin, end, t) - begin);
}

[[nodiscard]] bool isBoolColumn(const TopicChunk& chunk, std::size_t column_index) {
  return column_index < chunk.columns.size() && chunk.columns[column_index].descriptor &&
         chunk.columns[column_index].descriptor->logical_type == PrimitiveType::kBool;
}

[[nodiscard]] double readY(const TopicChunk& chunk, std::size_t column_index, std::size_t row) {
  if (column_index >= chunk.columns.size() || chunk.isNull(column_index, row)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (isBoolColumn(chunk, column_index)) {
    return chunk.readBool(column_index, row) ? 1.0 : 0.0;
  }
  return chunk.readNumericAsDouble(column_index, row);
}

void updateFiniteRange(double value, double& min_value, double& max_value, bool& found_value) {
  if (!std::isfinite(value)) {
    return;
  }
  min_value = found_value ? std::min(min_value, value) : value;
  max_value = found_value ? std::max(max_value, value) : value;
  found_value = true;
}

}  // namespace

DatastoreCurveAdapter::DatastoreCurveAdapter(SessionManager* session, CurveDescriptor source)
    : session_(session), source_(std::move(source)), cached_full_bounding_rect_(invalidRect()) {}

std::size_t DatastoreCurveAdapter::size() const {
  ensureChunkIndex_();
  return total_visible_rows_;
}

QPointF DatastoreCurveAdapter::sample(std::size_t index) const {
  ensureChunkIndex_();
  if (index >= total_visible_rows_ || chunk_index_.empty()) {
    return invalidPoint();
  }

  const std::size_t slot_index = findChunkSlot_(index);
  const ChunkSlot& slot = chunk_index_[slot_index];
  const std::size_t row = slot.row_start + (index - slot.cumulative_begin);
  return readPoint_(slot, row);
}

QRectF DatastoreCurveAdapter::boundingRect() const {
  if (full_bounding_rect_valid_) {
    return cached_full_bounding_rect_;
  }

  cached_full_bounding_rect_ = invalidRect();
  full_bounding_rect_valid_ = true;

  if (session_ == nullptr) {
    return cached_full_bounding_rect_;
  }

  const TopicStorage* storage = session_->dataEngine().getTopicStorage(source_.topic_id);
  if (storage == nullptr || storage->empty()) {
    return cached_full_bounding_rect_;
  }

  const TopicMetadata metadata = storage->metadata();
  double y_min = 0.0;
  double y_max = 0.0;
  bool found_y = false;
  for (const TopicChunk& chunk : storage->sealedChunks()) {
    if (source_.column_index >= chunk.stats.column_stats.size()) {
      continue;
    }
    const ColumnStats& stats = chunk.stats.column_stats[source_.column_index];
    if (stats.min_value.has_value()) {
      updateFiniteRange(*stats.min_value, y_min, y_max, found_y);
    }
    if (stats.max_value.has_value()) {
      updateFiniteRange(*stats.max_value, y_min, y_max, found_y);
    }
  }

  if (!found_y) {
    return cached_full_bounding_rect_;
  }

  const Timestamp offset = displayOffsetNow_();
  const double x_min = rawNsToDisplaySeconds(metadata.time_range_min, offset);
  const double x_max = rawNsToDisplaySeconds(metadata.time_range_max, offset);
  cached_full_bounding_rect_ = QRectF(QPointF(x_min, y_min), QPointF(x_max, y_max)).normalized();
  return cached_full_bounding_rect_;
}

void DatastoreCurveAdapter::setRectOfInterest(const QRectF& rect) {
  Timestamp next_min = 0;
  Timestamp next_max = 0;
  if (!std::isfinite(rect.left()) || !std::isfinite(rect.right())) {
    // Non-finite rect bounds (NaN / inf) — fall back to the all-rows window so
    // size()/sample() stay valid. Qwt occasionally hands us sentinel rects on
    // first paint or while axes are being reconfigured.
    next_min = std::numeric_limits<Timestamp>::min();
    next_max = std::numeric_limits<Timestamp>::max();
  } else {
    const Timestamp offset = displayOffsetNow_();
    const Timestamp left = displaySecondsToRawNs(rect.left(), offset);
    const Timestamp right = displaySecondsToRawNs(rect.right(), offset);
    next_min = std::min(left, right);
    next_max = std::max(left, right);
  }

  if (visible_t_min_raw_ns_ == next_min && visible_t_max_raw_ns_ == next_max) {
    return;
  }

  visible_t_min_raw_ns_ = next_min;
  visible_t_max_raw_ns_ = next_max;
  chunk_index_dirty_ = true;
  last_slot_ = 0;
}

std::optional<std::pair<double, double>> DatastoreCurveAdapter::visibleYRange(
    double x_min_sec, double x_max_sec) const {
  if (session_ == nullptr) {
    return std::nullopt;
  }

  const TopicStorage* storage = session_->dataEngine().getTopicStorage(source_.topic_id);
  if (storage == nullptr) {
    return std::nullopt;
  }

  const Timestamp offset = displayOffsetNow_();
  const Timestamp raw_a = displaySecondsToRawNs(x_min_sec, offset);
  const Timestamp raw_b = displaySecondsToRawNs(x_max_sec, offset);
  const Timestamp raw_min = std::min(raw_a, raw_b);
  const Timestamp raw_max = std::max(raw_a, raw_b);

  double y_min = 0.0;
  double y_max = 0.0;
  bool found_y = false;

  for (const TopicChunk& chunk : storage->sealedChunks()) {
    if (chunk.stats.row_count == 0 || chunk.stats.t_max < raw_min || chunk.stats.t_min > raw_max) {
      continue;
    }

    const bool full_chunk = raw_min <= chunk.stats.t_min && chunk.stats.t_max <= raw_max;
    if (full_chunk && source_.column_index < chunk.stats.column_stats.size()) {
      const ColumnStats& stats = chunk.stats.column_stats[source_.column_index];
      if (stats.min_value.has_value()) {
        updateFiniteRange(*stats.min_value, y_min, y_max, found_y);
      }
      if (stats.max_value.has_value()) {
        updateFiniteRange(*stats.max_value, y_min, y_max, found_y);
      }
      continue;
    }

    const std::size_t row_start = firstRowGreaterEqual(chunk, raw_min);
    const std::size_t row_end = firstRowGreater(chunk, raw_max);
    for (std::size_t row = row_start; row < row_end; ++row) {
      updateFiniteRange(readY(chunk, source_.column_index, row), y_min, y_max, found_y);
    }
  }

  if (!found_y) {
    return std::nullopt;
  }
  return std::pair<double, double>{y_min, y_max};
}

void DatastoreCurveAdapter::onTopicCommitted() {
  chunk_index_dirty_ = true;
  full_bounding_rect_valid_ = false;
  last_slot_ = 0;
}

void DatastoreCurveAdapter::onDataCleared() {
  chunk_index_.clear();
  total_visible_rows_ = 0;
  chunk_index_dirty_ = true;
  full_bounding_rect_valid_ = false;
  cached_full_bounding_rect_ = invalidRect();
  last_slot_ = 0;
}

std::optional<QPointF> DatastoreCurveAdapter::sampleFromTime(double display_time_sec) const {
  if (session_ == nullptr) {
    return std::nullopt;
  }

  const Timestamp raw_time = displaySecondsToRawNs(display_time_sec, displayOffsetNow_());
  auto latest = session_->createReader().latestAt(QueryPoint{.topic_id = source_.topic_id, .t = raw_time});
  if (!latest.has_value() || !latest->has_value()) {
    return std::nullopt;
  }

  const SampleRow& row = **latest;
  ChunkSlot slot{
      .chunk = row.chunk,
      .row_start = row.row_index,
      .row_end = row.row_index + 1,
      .cumulative_begin = 0,
      .cumulative_end = 1,
  };
  return readPoint_(slot, row.row_index);
}

void DatastoreCurveAdapter::ensureChunkIndex_() const {
  if (!chunk_index_dirty_) {
    return;
  }

  chunk_index_.clear();
  total_visible_rows_ = 0;
  last_slot_ = 0;

  if (session_ == nullptr) {
    chunk_index_dirty_ = false;
    return;
  }

  const TopicStorage* storage = session_->dataEngine().getTopicStorage(source_.topic_id);
  if (storage == nullptr) {
    chunk_index_dirty_ = false;
    return;
  }

  // Plan-then-materialize so cross-chunk boundary guards (one row from the chunk
  // immediately before the window, one from the chunk immediately after) can be
  // emitted in time order before cumulative offsets are assigned.
  struct PreSlot {
    const TopicChunk* chunk;
    std::size_t row_start;
    std::size_t row_end;
  };
  std::vector<PreSlot> pre_slots;

  const bool all_rows = isAllRowsWindow(visible_t_min_raw_ns_, visible_t_max_raw_ns_);
  if (all_rows) {
    for (const TopicChunk& chunk : storage->sealedChunks()) {
      const std::size_t row_count = chunk.stats.row_count;
      if (row_count == 0) {
        continue;
      }
      pre_slots.push_back(PreSlot{&chunk, 0, row_count});
    }
  } else {
    const TopicChunk* prev_left_guard = nullptr;  // last chunk fully before window
    bool emitted_overlap = false;
    // True when the most recent overlap chunk had no in-chunk forward-step
    // available (its natural row_end was at row_count). In that case the
    // segment crossing the upper boundary needs a cross-chunk right guard
    // from the next chunk.
    bool last_overlap_needs_right_guard = false;

    for (const TopicChunk& chunk : storage->sealedChunks()) {
      const std::size_t row_count = chunk.stats.row_count;
      if (row_count == 0) {
        continue;
      }

      if (chunk.stats.t_max < visible_t_min_raw_ns_) {
        prev_left_guard = &chunk;
        continue;
      }

      if (chunk.stats.t_min > visible_t_max_raw_ns_) {
        // First chunk after the window — chunks are time-ordered, so no later
        // chunk can overlap. Emit cross-chunk right guards when needed.
        if (!emitted_overlap) {
          // Window falls in a gap between this chunk and prev_left_guard:
          // emit both guards so the segment renders across the gap.
          if (prev_left_guard != nullptr && prev_left_guard->stats.row_count > 0) {
            pre_slots.push_back(
                PreSlot{prev_left_guard, prev_left_guard->stats.row_count - 1, prev_left_guard->stats.row_count});
            pre_slots.push_back(PreSlot{&chunk, 0, 1});
          }
        } else if (last_overlap_needs_right_guard) {
          // Last overlap chunk's forward-step couldn't be applied (it ended at
          // row_count); supply the right guard from this next chunk.
          pre_slots.push_back(PreSlot{&chunk, 0, 1});
        }
        break;
      }

      const std::size_t natural_row_start = firstRowGreaterEqual(chunk, visible_t_min_raw_ns_);
      const std::size_t natural_row_end = firstRowGreater(chunk, visible_t_max_raw_ns_);
      std::size_t row_start = natural_row_start;
      std::size_t row_end = natural_row_end;
      if (row_start > 0) {
        --row_start;
      }
      if (row_end < row_count) {
        ++row_end;
      }
      if (row_start >= row_end) {
        continue;
      }

      // Cross-chunk left guard: only when the natural (pre-back-step) row_start
      // was already 0 — i.e. no in-chunk guard is available because the window's
      // lower bound is at or before this chunk's first sample. Pull the previous
      // chunk's last row so the segment crossing the lower boundary survives.
      if (!emitted_overlap && natural_row_start == 0 && prev_left_guard != nullptr &&
          prev_left_guard->stats.row_count > 0) {
        pre_slots.push_back(
            PreSlot{prev_left_guard, prev_left_guard->stats.row_count - 1, prev_left_guard->stats.row_count});
      }

      pre_slots.push_back(PreSlot{&chunk, row_start, row_end});
      emitted_overlap = true;
      last_overlap_needs_right_guard = (natural_row_end == row_count);
    }
  }

  chunk_index_.reserve(pre_slots.size());
  for (const PreSlot& pre : pre_slots) {
    const std::size_t cumulative_begin = total_visible_rows_;
    total_visible_rows_ += pre.row_end - pre.row_start;
    chunk_index_.push_back(
        ChunkSlot{
            .chunk = pre.chunk,
            .row_start = pre.row_start,
            .row_end = pre.row_end,
            .cumulative_begin = cumulative_begin,
            .cumulative_end = total_visible_rows_,
        });
  }

  chunk_index_dirty_ = false;
}

std::size_t DatastoreCurveAdapter::findChunkSlot_(std::size_t global_row) const {
  if (last_slot_ < chunk_index_.size()) {
    const ChunkSlot& slot = chunk_index_[last_slot_];
    if (slot.cumulative_begin <= global_row && global_row < slot.cumulative_end) {
      return last_slot_;
    }
  }

  const std::size_t next_slot = last_slot_ + 1;
  if (next_slot < chunk_index_.size()) {
    const ChunkSlot& slot = chunk_index_[next_slot];
    if (slot.cumulative_begin <= global_row && global_row < slot.cumulative_end) {
      last_slot_ = next_slot;
      return last_slot_;
    }
  }

  const auto it = std::upper_bound(
      chunk_index_.begin(), chunk_index_.end(), global_row,
      [](std::size_t row, const ChunkSlot& slot) { return row < slot.cumulative_end; });
  last_slot_ = static_cast<std::size_t>(it - chunk_index_.begin());
  return last_slot_;
}

QPointF DatastoreCurveAdapter::readPoint_(const ChunkSlot& slot, std::size_t row) const {
  if (slot.chunk == nullptr || row >= slot.chunk->stats.row_count) {
    return invalidPoint();
  }

  const Timestamp timestamp = slot.chunk->readTimestamp(row);
  const double x = rawNsToDisplaySeconds(timestamp, displayOffsetNow_());
  const double y = readY(*slot.chunk, source_.column_index, row);
  return {x, y};
}

Timestamp DatastoreCurveAdapter::displayOffsetNow_() const {
  // Live lookup only — never fall back to source_.display_offset_ns, which is
  // a snapshot taken at catalog-build time and would silently go stale if the
  // time domain were reconfigured.
  if (session_ == nullptr) {
    return 0;
  }
  const DatasetInfo* dataset = session_->dataEngine().getDataset(source_.dataset_id);
  if (dataset == nullptr || dataset->time_domain.id == 0) {
    return 0;
  }
  const TimeDomain* time_domain = session_->dataEngine().getTimeDomain(dataset->time_domain.id);
  return time_domain != nullptr ? time_domain->display_offset : 0;
}

}  // namespace PJ
