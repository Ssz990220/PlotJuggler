#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

namespace PJ {

// Global icon / layout dimensions broadcast from MainWindow to every
// chrome-aware widget. Pixel units. Defaults match the first-launch
// baseline used by MainWindow before QSettings is read.
struct ChromeMetrics {
  int icon_size = 20;
  int icon_padding = 4;
  int layout_padding = 0;
  int layout_spacing = 0;
};

}  // namespace PJ
