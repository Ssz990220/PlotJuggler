#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The third PlotWidget series type, after DatastoreCurveAdapter (one column vs
// time) and PointSeriesXY (two columns paired over all rows). SnapshotSeriesData
// renders the message currently under the time tracker: for one leaf field of an
// array (e.g. positions[3] of predicted_trajectory[0..28]) it plots that leaf of
// every array element against either the element index or a second leaf (e.g.
// time_from_start_s) — all read from ONE datastore row so no element mixes a value
// from a different message.
//
// Unlike the other two series types, its content is EVENT-driven, not
// viewport-driven: it does not lazily rebuild on paint. PlotWidget calls refresh()
// when the tracker moves (playback) or when new samples arrive on the topic
// (streaming), and size()/sample() just read the snapshot that produced.

#include <qwt_series_data.h>

#include <QPointF>
#include <QRectF>
#include <cstddef>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_plotting/SnapshotGroupResolver.h"

namespace PJ {

class SessionManager;

class SnapshotSeriesData final : public QwtSeriesData<QPointF> {
 public:
  // X source: the element index (0,1,2,...) or a resolved leaf column per element.
  enum class XMode { kIndex, kColumn };

  // `y_elements` is the resolved (element index -> Y column) list, sorted by
  // element index. For kColumn x-mode, `x_elements` is the resolved (element index
  // -> X column) list and X/Y are inner-joined on element index (an element that
  // resolves for Y but not X is dropped, so pairing stays exact across gaps). For
  // kIndex x-mode `x_elements` is ignored and X is the element index.
  SnapshotSeriesData(
      SessionManager* session, TopicId topic_id, DatasetId dataset_id, XMode x_mode,
      std::vector<SnapshotElement> x_elements, std::vector<SnapshotElement> y_elements);

  // QwtSeriesData<QPointF>
  std::size_t size() const override;
  QPointF sample(std::size_t index) const override;
  QRectF boundingRect() const override;
  void setRectOfInterest(const QRectF&) override {}

  // Rebuild the snapshot to the message at-or-before `display_time_sec` (the shared
  // time axis, converted to raw ns through the topic's dataset display offset).
  // Reads ONE datastore row, so every point shares a vintage; ragged / absent
  // elements and non-finite values are skipped. Returns true if the point set
  // changed, so the caller can gate a replot.
  bool refresh(double display_time_sec);

  // Drop the current snapshot (dataset replaced / cleared).
  void onDataCleared();

  [[nodiscard]] TopicId topicId() const noexcept {
    return topic_id_;
  }
  [[nodiscard]] DatasetId datasetId() const noexcept {
    return dataset_id_;
  }
  [[nodiscard]] XMode xMode() const noexcept {
    return x_mode_;
  }
  // The resolved element lists, for layout persistence (P2).
  [[nodiscard]] const std::vector<SnapshotElement>& xElements() const noexcept {
    return x_elements_;
  }
  [[nodiscard]] const std::vector<SnapshotElement>& yElements() const noexcept {
    return y_elements_;
  }

 private:
  // One plotted element: where its Y (and, for kColumn, X) value lands in the
  // latestRowAt result that is parallel to query_columns_.
  struct PointPlan {
    uint32_t element_index = 0;
    std::size_t y_result_index = 0;
    std::size_t x_result_index = 0;  // kColumn only
  };

  void recomputeBoundingRect() const;

  SessionManager* session_ = nullptr;
  TopicId topic_id_ = 0;
  DatasetId dataset_id_ = 0;
  XMode x_mode_ = XMode::kIndex;
  std::vector<SnapshotElement> x_elements_;
  std::vector<SnapshotElement> y_elements_;

  // Column indices passed to latestRowAt (one read per refresh), and the join plan
  // that maps each plotted element to positions inside that read's result.
  std::vector<std::size_t> query_columns_;
  std::vector<PointPlan> plans_;

  // The current snapshot points (set by refresh(), read by size()/sample()).
  std::vector<QPointF> points_;
  mutable QRectF cached_bounding_rect_;
  mutable bool bounding_rect_valid_ = false;
};

}  // namespace PJ
