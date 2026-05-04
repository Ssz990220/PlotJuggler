#pragma once

#include <qwt_series_data.h>

#include <QPointF>
#include <QRectF>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "pj_app_core/CurveDescriptor.h"
#include "pj_base/types.hpp"
#include "pj_datastore/query.hpp"

namespace PJ {

class SessionManager;

class DatastoreCurveAdapter final : public QwtSeriesData<QPointF> {
 public:
  DatastoreCurveAdapter(SessionManager* session, CurveDescriptor source);

  std::size_t size() const override;
  QPointF sample(std::size_t index) const override;
  QRectF boundingRect() const override;
  void setRectOfInterest(const QRectF& rect) override;

  [[nodiscard]] std::optional<std::pair<double, double>> visibleYRange(double x_min_sec, double x_max_sec) const;

  void onTopicCommitted();
  void onDataCleared();

  [[nodiscard]] const CurveDescriptor& source() const noexcept {
    return source_;
  }
  [[nodiscard]] std::optional<QPointF> sampleFromTime(double display_time_sec) const;

 private:
  void ensureChunkIndex_() const;
  [[nodiscard]] QPointF readPoint_(const SeriesSample& sample) const;
  [[nodiscard]] Timestamp displayOffsetNow_() const;

  SessionManager* session_ = nullptr;
  CurveDescriptor source_;

  Timestamp visible_t_min_raw_ns_ = std::numeric_limits<Timestamp>::min();
  Timestamp visible_t_max_raw_ns_ = std::numeric_limits<Timestamp>::max();

  mutable std::vector<SeriesSample> sample_index_;
  mutable bool sample_index_dirty_ = true;

  mutable QRectF cached_full_bounding_rect_;
  mutable bool full_bounding_rect_valid_ = false;
};

}  // namespace PJ
