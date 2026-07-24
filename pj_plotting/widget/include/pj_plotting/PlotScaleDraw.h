#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_scale_draw.h>
#include <qwt_text.h>

namespace PJ {

/// Scale draw installed on every PJ4 plot axis: tick labels use fixed
/// notation (6 decimals, default locale) with trailing zeros stripped,
/// instead of Qwt's default shortest-form QLocale::toString(). Keep in sync
/// with the twin in pj_dialog_host/src/chart_preview_widget.cpp, which cannot
/// link this module.
class PlotScaleDraw : public QwtScaleDraw {
 public:
  [[nodiscard]] QwtText label(double value) const override;
};

}  // namespace PJ
