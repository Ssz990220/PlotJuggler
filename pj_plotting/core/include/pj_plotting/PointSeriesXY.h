#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_series_data.h>

#include <QPointF>
#include <QRectF>
#include <cstddef>
#include <limits>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_runtime/CurveDescriptor.h"

namespace PJ {

class SessionManager;
struct TopicChunk;

class PointSeriesXY final : public QwtSeriesData<QPointF> {
 public:
  PointSeriesXY(SessionManager* session, CurveDescriptor x_source, CurveDescriptor y_source);

  std::size_t size() const override;
  QPointF sample(std::size_t index) const override;
  QRectF boundingRect() const override;
  void setRectOfInterest(const QRectF& rect) override;

  void onTopicCommitted();
  void onDataCleared();

  [[nodiscard]] const CurveDescriptor& xSource() const noexcept {
    return x_source_;
  }
  [[nodiscard]] const CurveDescriptor& ySource() const noexcept {
    return y_source_;
  }

 private:
  struct PairSlot {
    const TopicChunk* x_chunk = nullptr;
    std::size_t x_row = 0;
    const TopicChunk* y_chunk = nullptr;
    std::size_t y_row = 0;
  };

  struct RowRef {
    const TopicChunk* chunk = nullptr;
    std::size_t row = 0;
  };

  void ensureAlignmentIndex_() const;
  void buildSameTopicIndex_() const;
  void buildDifferentTopicIndex_() const;
  [[nodiscard]] std::vector<RowRef> rowsFor_(TopicId topic_id) const;
  [[nodiscard]] QPointF readPoint_(const PairSlot& slot) const;

  SessionManager* session_ = nullptr;
  CurveDescriptor x_source_;
  CurveDescriptor y_source_;

  mutable std::vector<PairSlot> pairs_;
  mutable QRectF cached_bounding_rect_;
  mutable bool alignment_dirty_ = true;
  mutable bool bounding_rect_valid_ = false;
};

}  // namespace PJ
