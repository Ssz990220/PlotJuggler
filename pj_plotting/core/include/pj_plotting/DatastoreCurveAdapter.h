#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_series_data.h>

#include <QPointF>
#include <QRectF>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_datastore/query.hpp"
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/Time.h"

namespace PJ {

class SessionManager;

// Not `final`: FilteredCurveAdapter derives from it so a filter-output preview
// rides the SAME samplesIngested -> onTopicCommitted() refresh path as the raw
// ghost (see FilteredCurveAdapter.h). The data accessors are already virtual via
// QwtSeriesData; the commit/clear hooks are made virtual for that subclass.
class DatastoreCurveAdapter : public QwtSeriesData<QPointF> {
 public:
  DatastoreCurveAdapter(SessionManager* session, CurveDescriptor source);
  ~DatastoreCurveAdapter() override = default;

  std::size_t size() const override;
  QPointF sample(std::size_t index) const override;
  QRectF boundingRect() const override;
  void setRectOfInterest(const QRectF& rect) override;

  // Y extent over the given display-x window — the fast path PlotWidget uses to
  // auto-fit the Y axis (it queries the store's per-range bounds rather than the
  // visible sample buffer). virtual so a derived curve (FilteredCurveAdapter)
  // reports its OWN output range instead of this raw input range.
  [[nodiscard]] virtual std::optional<Range<double>> visibleYRange(Range<double> x_range_sec) const;

  // Invalidate the cached sample index / bounds so the next read re-queries the
  // store. PlotWidget calls these from its samplesIngested / dataset-replace
  // handlers; a subclass overrides them to refresh its own derived cache too.
  virtual void onTopicCommitted();
  virtual void onDataCleared();

  [[nodiscard]] const CurveDescriptor& source() const noexcept {
    return source_;
  }
  [[nodiscard]] std::optional<QPointF> sampleFromTime(double display_time_sec) const;

 protected:
  // The session a subclass needs to read the input column / resolve the display
  // offset. May be null (constructed without a session); callers null-check.
  [[nodiscard]] SessionManager* session() const noexcept {
    return session_;
  }

 private:
  void ensureChunkIndex() const;
  [[nodiscard]] QPointF readPoint(const SeriesSample& sample) const;
  [[nodiscard]] DisplayOffset displayOffsetNow() const;

  SessionManager* session_ = nullptr;
  CurveDescriptor source_;

  Timestamp visible_t_min_raw_ns_ = std::numeric_limits<Timestamp>::min();
  Timestamp visible_t_max_raw_ns_ = std::numeric_limits<Timestamp>::max();

  mutable std::vector<SeriesSample> sample_index_;
  mutable bool sample_index_dirty_ = true;

  mutable QRectF cached_full_bounding_rect_;
  mutable bool full_bounding_rect_valid_ = false;

  // Cached display-time offset. Resolved live on first use after an
  // invalidation; invalidated in onTopicCommitted/onDataCleared so it tracks
  // time-domain reconfiguration through the same signals that drive sample
  // re-indexing. Removing this cache makes readPoint_() pay 2 DataEngine
  // lookups per sample, which dominates per-curve paint cost.
  mutable DisplayOffset cached_display_offset_;
  mutable bool cached_display_offset_valid_ = false;
};

}  // namespace PJ
