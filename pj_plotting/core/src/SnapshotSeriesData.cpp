// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/SnapshotSeriesData.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

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

}  // namespace

SnapshotSeriesData::SnapshotSeriesData(
    SessionManager* session, TopicId topic_id, DatasetId dataset_id, XMode x_mode,
    std::vector<SnapshotElement> x_elements, std::vector<SnapshotElement> y_elements, SnapshotBinding binding)
    : session_(session),
      topic_id_(topic_id),
      dataset_id_(dataset_id),
      x_mode_(x_mode),
      x_elements_(std::move(x_elements)),
      y_elements_(std::move(y_elements)),
      binding_(std::move(binding)),
      cached_bounding_rect_(invalidRect()) {
  // Build the read plan once: the set of columns to read per refresh (query_columns_)
  // and, per plotted element, where its X/Y values land in that read's result. For
  // kColumn we inner-join Y onto X by element index so a Y-only element is dropped
  // and X/Y never get misaligned across gaps.
  plans_.reserve(y_elements_.size());
  if (x_mode_ == XMode::kIndex) {
    for (const SnapshotElement& y : y_elements_) {
      PointPlan plan;
      plan.element_index = y.element_index;
      plan.y_result_index = query_columns_.size();
      query_columns_.push_back(y.column_index);
      plans_.push_back(plan);
    }
  } else {
    std::unordered_map<uint32_t, std::size_t> x_column_by_element;
    x_column_by_element.reserve(x_elements_.size());
    for (const SnapshotElement& x : x_elements_) {
      x_column_by_element.emplace(x.element_index, x.column_index);
    }
    for (const SnapshotElement& y : y_elements_) {
      const auto it = x_column_by_element.find(y.element_index);
      if (it == x_column_by_element.end()) {
        continue;  // no X column for this element — drop it, keeping X/Y paired
      }
      PointPlan plan;
      plan.element_index = y.element_index;
      plan.y_result_index = query_columns_.size();
      query_columns_.push_back(y.column_index);
      plan.x_result_index = query_columns_.size();
      query_columns_.push_back(it->second);
      plans_.push_back(plan);
    }
  }
}

std::size_t SnapshotSeriesData::size() const {
  return points_.size();
}

QPointF SnapshotSeriesData::sample(std::size_t index) const {
  if (index >= points_.size()) {
    return invalidPoint();
  }
  return points_[index];
}

QRectF SnapshotSeriesData::boundingRect() const {
  if (!bounding_rect_valid_) {
    recomputeBoundingRect();
  }
  return cached_bounding_rect_;
}

void SnapshotSeriesData::recomputeBoundingRect() const {
  double x_min = 0.0;
  double x_max = 0.0;
  double y_min = 0.0;
  double y_max = 0.0;
  bool found = false;
  for (const QPointF& point : points_) {
    if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
      continue;
    }
    if (!found) {
      x_min = x_max = point.x();
      y_min = y_max = point.y();
      found = true;
    } else {
      x_min = std::min(x_min, point.x());
      x_max = std::max(x_max, point.x());
      y_min = std::min(y_min, point.y());
      y_max = std::max(y_max, point.y());
    }
  }
  cached_bounding_rect_ =
      found ? QRectF(QPointF(x_min, y_min), QPointF(x_max, y_max)).normalized() : invalidRect();
  bounding_rect_valid_ = true;
}

bool SnapshotSeriesData::refresh(double display_time_sec) {
  std::vector<QPointF> next;

  if (session_ != nullptr && !plans_.empty()) {
    // The tracker time is on the shared display-seconds axis; convert to raw ns via
    // the topic's dataset display offset to find the message at-or-before it. The X
    // and Y VALUES themselves are raw data (a duration field or an index), so no
    // display-offset conversion applies to them.
    const DisplayOffset offset = session_->displayOffset(dataset_id_);
    const Timestamp raw_ns = displaySecondsToRaw(fromAxisDouble(display_time_sec), offset);

    const auto snapshot_or =
        session_->createReader().latestRowAt(QueryPoint{.topic_id = topic_id_, .t = raw_ns}, query_columns_);
    if (snapshot_or.has_value() && snapshot_or->has_value()) {
      const RowSnapshot& snapshot = **snapshot_or;
      next.reserve(plans_.size());
      for (const PointPlan& plan : plans_) {
        const std::optional<double>& y = snapshot.values[plan.y_result_index];
        if (!y.has_value() || !std::isfinite(*y)) {
          continue;  // ragged / absent element, or a non-finite value
        }
        double x = 0.0;
        if (x_mode_ == XMode::kIndex) {
          x = static_cast<double>(plan.element_index);
        } else {
          const std::optional<double>& x_value = snapshot.values[plan.x_result_index];
          if (!x_value.has_value() || !std::isfinite(*x_value)) {
            continue;  // element has Y but no X in this message — skip to stay paired
          }
          x = *x_value;
        }
        next.emplace_back(x, *y);
      }
    }
  }

  const bool changed = next != points_;

  if (std::getenv("PJ_SNAP_TRACE") != nullptr) {
    const DisplayOffset offset = session_ != nullptr ? session_->displayOffset(dataset_id_) : DisplayOffset{};
    const Timestamp raw_ns = displaySecondsToRaw(fromAxisDouble(display_time_sec), offset);
    std::fprintf(
        stderr,
        "[snap] refresh topic=%llu ds=%llu disp=%.6f offset_ns=%lld raw_ns=%lld plans=%zu pts=%zu->%zu first=%s "
        "changed=%d y='%s'\n",
        static_cast<unsigned long long>(topic_id_), static_cast<unsigned long long>(dataset_id_), display_time_sec,
        static_cast<long long>(offset.value.count()), static_cast<long long>(raw_ns), plans_.size(), points_.size(),
        next.size(), next.empty() ? "(none)" : std::to_string(next.front().y()).c_str(), changed ? 1 : 0,
        binding_.y_pattern.c_str());
  }

  points_ = std::move(next);
  bounding_rect_valid_ = false;
  return changed;
}

void SnapshotSeriesData::onDataCleared() {
  points_.clear();
  bounding_rect_valid_ = false;
  cached_bounding_rect_ = invalidRect();
}

}  // namespace PJ
