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

#include <string>

#include "pj_base/types.hpp"
#include "pj_plotting/SnapshotGroupResolver.h"

namespace PJ {

class SessionManager;

// The stable, human-facing identity of a snapshot curve, independent of the
// per-load topic id / column indices. This is what layout persistence stores and
// re-resolves on load, and what forms the curve's stable source key.
struct SnapshotBinding {
  std::string topic_name;  // stable topic name, e.g. "/wholebody/left_arm/debug/spline_info"
  std::string x_pattern;   // "<array>[:]<leaf>" X wildcard; empty => X is the element index
  std::string y_pattern;   // "<array>[:]<leaf>" Y wildcard, one per curve
};

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
      std::vector<SnapshotElement> x_elements, std::vector<SnapshotElement> y_elements, SnapshotBinding binding = {});

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

  // Re-resolve the X/Y patterns held in binding() against a fresh column set (the
  // topic's columns after an in-place dataset reload swapped its column descriptors
  // wholesale) and rebuild the read plan, so the cached numeric column indices can
  // never silently point at different fields. Clears the current points; the next
  // refresh() repopulates. Returns true if the Y pattern (and, in kColumn x-mode, at
  // least one element-paired X) still resolves — false leaves the plan empty so the
  // curve gracefully renders nothing.
  bool rebuildPlan(const std::vector<SnapshotColumn>& columns);

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
  // Stable identity (topic name + X/Y patterns) for layout save/re-resolve.
  [[nodiscard]] const SnapshotBinding& binding() const noexcept {
    return binding_;
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
  // Build query_columns_ + plans_ from the current x_elements_/y_elements_ (the
  // shared core of construction and rebuildPlan). Clears both first.
  void buildPlan();

  SessionManager* session_ = nullptr;
  TopicId topic_id_ = 0;
  DatasetId dataset_id_ = 0;
  XMode x_mode_ = XMode::kIndex;
  std::vector<SnapshotElement> x_elements_;
  std::vector<SnapshotElement> y_elements_;
  SnapshotBinding binding_;

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
