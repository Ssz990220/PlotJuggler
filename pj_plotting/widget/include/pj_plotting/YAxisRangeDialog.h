#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <optional>

#include "pj_widgets/Dialog.h"

class QCheckBox;
class QDoubleSpinBox;

namespace PJ {

// Modal dialog to assign a plot's MANUAL y-axis range. Each bound is independent:
// its "Auto" checkbox leaves that bound auto-fitting (a half-open pin — e.g. a fixed
// max with an auto min). Backs the plot context menu's "Y Axis Range..." action; the
// result feeds PlotWidgetBase::setFixedYRange. OK is disabled while the range is
// degenerate (both bounds pinned with min >= max).
class YAxisRangeDialog : public Dialog {
  Q_OBJECT
 public:
  YAxisRangeDialog(std::optional<double> current_min, std::optional<double> current_max, QWidget* parent = nullptr);
  ~YAxisRangeDialog() override = default;

  // The chosen bounds; nullopt for a bound left on "Auto".
  [[nodiscard]] std::optional<double> yMin() const;
  [[nodiscard]] std::optional<double> yMax() const;

 private:
  void refreshEnabledState();

  QCheckBox* auto_min_ = nullptr;
  QCheckBox* auto_max_ = nullptr;
  QDoubleSpinBox* min_spin_ = nullptr;
  QDoubleSpinBox* max_spin_ = nullptr;
  QWidget* ok_button_ = nullptr;
};

}  // namespace PJ
