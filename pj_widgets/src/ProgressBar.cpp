// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/ProgressBar.h"

namespace PJ {

ProgressBar::ProgressBar(QWidget* parent) : QProgressBar(parent) {
  // Centre the caption to match the mockup; the colour, border, radius and
  // fill all come from the `PJ--ProgressBar` QSS rule, not from here.
  setAlignment(Qt::AlignCenter);
  setTextVisible(true);
}

}  // namespace PJ
