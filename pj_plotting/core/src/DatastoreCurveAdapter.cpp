// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/DatastoreCurveAdapter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include "pj_base/dataset.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace PJ {
namespace {

[[nodiscard]] QPointF invalidPoint() {
  return {0.0, std::numeric_limits<double>::quiet_NaN()};
}

[[nodiscard]] QRectF invalidRect() {
  return {1.0, 1.0, -2.0, -2.0};
}

// Thin Qwt-boundary adapters over the canonical conversions in Time.h, so the
// (raw - offset)/1e9 arithmetic lives in exactly one place. Caller is responsible
// for finite-checking display_sec — Qwt rects from real viewports are always
// bounded; the upstream call sites guard against NaN/inf.
[[nodiscard]] Timestamp displaySecondsToRawNs(double display_sec, Timestamp display_offset_ns) noexcept {
  return displaySecondsToRaw(fromAxisDouble(display_sec), DisplayOffset{Duration{display_offset_ns}});
}

[[nodiscard]] double rawNsToDisplaySeconds(Timestamp raw_ns, Timestamp display_offset_ns) noexcept {
  return toAxisDouble(rawToDisplaySeconds(raw_ns, DisplayOffset{Duration{display_offset_ns}}));
}

[[nodiscard]] bool isAllRowsWindow(Timestamp t_min, Timestamp t_max) noexcept {
  return t_min == std::numeric_limits<Timestamp>::min() && t_max == std::numeric_limits<Timestamp>::max();
}

}  // namespace

DatastoreCurveAdapter::DatastoreCurveAdapter(SessionManager* session, CurveDescriptor source)
    : session_(session), source_(std::move(source)), cached_full_bounding_rect_(invalidRect()) {}

std::size_t DatastoreCurveAdapter::size() const {
  ensureChunkIndex_();
  return sample_index_.size();
}

QPointF DatastoreCurveAdapter::sample(std::size_t index) const {
  ensureChunkIndex_();
  if (index >= sample_index_.size()) {
    return invalidPoint();
  }

  return readPoint_(sample_index_[index]);
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

  auto series_or = session_->createReader().series(source_.topic_id, source_.column_index);
  if (!series_or.has_value()) {
    return cached_full_bounding_rect_;
  }

  const auto bounds = series_or->bounds();
  if (!bounds.has_value()) {
    return cached_full_bounding_rect_;
  }

  const Timestamp offset = displayOffsetNow_();
  const double x_min = rawNsToDisplaySeconds(bounds->time.min, offset);
  const double x_max = rawNsToDisplaySeconds(bounds->time.max, offset);
  cached_full_bounding_rect_ =
      QRectF(QPointF(x_min, bounds->value.min), QPointF(x_max, bounds->value.max)).normalized();
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
  sample_index_dirty_ = true;
}

std::optional<std::pair<double, double>> DatastoreCurveAdapter::visibleYRange(
    double x_min_sec, double x_max_sec) const {
  if (session_ == nullptr) {
    return std::nullopt;
  }

  auto series_or = session_->createReader().series(source_.topic_id, source_.column_index);
  if (!series_or.has_value()) {
    return std::nullopt;
  }

  std::optional<SeriesBounds> bounds;
  if (!std::isfinite(x_min_sec) || !std::isfinite(x_max_sec)) {
    bounds = series_or->bounds();
  } else {
    const Timestamp offset = displayOffsetNow_();
    const Timestamp raw_a = displaySecondsToRawNs(x_min_sec, offset);
    const Timestamp raw_b = displaySecondsToRawNs(x_max_sec, offset);
    bounds = series_or->bounds(Range<Timestamp>{.min = std::min(raw_a, raw_b), .max = std::max(raw_a, raw_b)});
  }

  if (!bounds.has_value()) {
    return std::nullopt;
  }
  return std::pair<double, double>{bounds->value.min, bounds->value.max};
}

void DatastoreCurveAdapter::onTopicCommitted() {
  sample_index_dirty_ = true;
  full_bounding_rect_valid_ = false;
  cached_display_offset_valid_ = false;
}

void DatastoreCurveAdapter::onDataCleared() {
  sample_index_.clear();
  sample_index_dirty_ = true;
  full_bounding_rect_valid_ = false;
  cached_full_bounding_rect_ = invalidRect();
  cached_display_offset_valid_ = false;
}

std::optional<QPointF> DatastoreCurveAdapter::sampleFromTime(double display_time_sec) const {
  if (session_ == nullptr) {
    return std::nullopt;
  }

  const Timestamp raw_time = displaySecondsToRawNs(display_time_sec, displayOffsetNow_());
  auto series_or = session_->createReader().series(source_.topic_id, source_.column_index);
  if (!series_or.has_value()) {
    return std::nullopt;
  }

  const auto sample = series_or->sampleAtOrBeforeTime(raw_time);
  return sample.has_value() ? std::optional<QPointF>{readPoint_(*sample)} : std::nullopt;
}

void DatastoreCurveAdapter::ensureChunkIndex_() const {
  if (!sample_index_dirty_) {
    return;
  }

  sample_index_.clear();

  if (session_ == nullptr) {
    sample_index_dirty_ = false;
    return;
  }

  auto series_or = session_->createReader().series(source_.topic_id, source_.column_index);
  if (!series_or.has_value()) {
    sample_index_dirty_ = false;
    return;
  }

  const SeriesReader& series = *series_or;
  const auto append_unique = [this](const SeriesSample& sample) {
    if (sample_index_.empty() || sample_index_.back().chunk != sample.chunk ||
        sample_index_.back().row_index != sample.row_index) {
      sample_index_.push_back(sample);
    }
  };

  const bool all_rows = isAllRowsWindow(visible_t_min_raw_ns_, visible_t_max_raw_ns_);
  if (all_rows) {
    auto cursor = series.samples(Range<Timestamp>{.min = visible_t_min_raw_ns_, .max = visible_t_max_raw_ns_});
    cursor.forEach(append_unique);
  } else {
    const auto first_inside = series.indexAtOrAfterTime(visible_t_min_raw_ns_);
    const auto last_inside = series.indexAtOrBeforeTime(visible_t_max_raw_ns_);

    if (const auto left_guard = series.indexAtOrBeforeTime(visible_t_min_raw_ns_);
        left_guard.has_value() && (!first_inside.has_value() || *left_guard < *first_inside)) {
      const auto sample = series.sampleAt(*left_guard);
      if (sample.has_value()) {
        append_unique(*sample);
      }
    }

    auto cursor = series.samples(Range<Timestamp>{.min = visible_t_min_raw_ns_, .max = visible_t_max_raw_ns_});
    cursor.forEach(append_unique);

    if (const auto right_guard = series.indexAtOrAfterTime(visible_t_max_raw_ns_);
        right_guard.has_value() && (!last_inside.has_value() || *right_guard > *last_inside)) {
      const auto sample = series.sampleAt(*right_guard);
      if (sample.has_value()) {
        append_unique(*sample);
      }
    }
  }

  sample_index_dirty_ = false;
}

QPointF DatastoreCurveAdapter::readPoint_(const SeriesSample& sample) const {
  if (sample.chunk == nullptr) {
    return invalidPoint();
  }

  return {rawNsToDisplaySeconds(sample.timestamp, displayOffsetNow_()), sample.value};
}

Timestamp DatastoreCurveAdapter::displayOffsetNow_() const {
  if (cached_display_offset_valid_) {
    return cached_display_offset_ns_;
  }

  // Live lookup — never fall back to source_.display_offset_ns, which is a
  // snapshot taken at catalog-build time. The cache below is invalidated by
  // onTopicCommitted / onDataCleared so it tracks time-domain reconfiguration
  // through the same signals that drive sample re-indexing.
  Timestamp offset = 0;
  if (session_ != nullptr) {
    const DatasetInfo* dataset = session_->dataEngine().getDataset(source_.dataset_id);
    if (dataset != nullptr && dataset->time_domain.id != 0) {
      const TimeDomain* time_domain = session_->dataEngine().getTimeDomain(dataset->time_domain.id);
      if (time_domain != nullptr) {
        offset = time_domain->display_offset;
      }
    }
  }

  cached_display_offset_ns_ = offset;
  cached_display_offset_valid_ = true;
  return offset;
}

}  // namespace PJ
