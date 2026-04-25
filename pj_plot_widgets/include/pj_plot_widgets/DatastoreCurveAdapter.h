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

namespace PJ {

class SessionManager;
struct TopicChunk;

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
  struct ChunkSlot {
    const TopicChunk* chunk = nullptr;
    std::size_t row_start = 0;
    std::size_t row_end = 0;
    std::size_t cumulative_begin = 0;
    std::size_t cumulative_end = 0;
  };

  void ensureChunkIndex_() const;
  [[nodiscard]] std::size_t findChunkSlot_(std::size_t global_row) const;
  [[nodiscard]] QPointF readPoint_(const ChunkSlot& slot, std::size_t row) const;
  [[nodiscard]] Timestamp displayOffsetNow_() const;

  SessionManager* session_ = nullptr;
  CurveDescriptor source_;

  Timestamp visible_t_min_raw_ns_ = std::numeric_limits<Timestamp>::min();
  Timestamp visible_t_max_raw_ns_ = std::numeric_limits<Timestamp>::max();

  mutable std::size_t last_slot_ = 0;
  mutable std::vector<ChunkSlot> chunk_index_;
  mutable std::size_t total_visible_rows_ = 0;
  mutable bool chunk_index_dirty_ = true;

  mutable QRectF cached_full_bounding_rect_;
  mutable bool full_bounding_rect_valid_ = false;
};

}  // namespace PJ
