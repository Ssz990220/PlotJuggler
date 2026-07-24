#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

namespace PJ {

// Global icon / layout dimensions broadcast from MainWindow to every
// chrome-aware widget. Pixel units. Defaults match the first-launch
// baseline used by MainWindow before QSettings is read.
struct ChromeMetrics {
  int icon_size = 24;
  int icon_padding = 4;
  int layout_padding = 2;
  int layout_spacing = 2;

  // The one canonical titlebar-tone band height. EVERY header band in the app
  // is this tall — the left-panel "Sources" header, "Datasets"/"Custom Series",
  // the timeline "Datasets" header, the Curve Width/Style headers, the toolbox
  // panel banner, and every plugin SectionHeaderBand — so they all read as one
  // strip. It is the button extent (icon_size + icon_padding) grown by the
  // layout padding on each side, matching the contentsMargins the panel headers
  // apply. This is the single source for that formula — never re-derive it at a
  // call site (that divergence is exactly how bands drifted out of alignment).
  [[nodiscard]] int bandHeight() const {
    return icon_size + icon_padding + (2 * layout_padding);
  }

  // The app's window-chrome (title-bar) height. The main-window TitleBar and
  // every framed Dialog chrome are exactly this tall, so a plugin/app dialog's
  // title bar matches the main window. It is bandHeight() grown by 1px for the
  // QSS bottom border that draws inside the bar's geometry. This is the single
  // source for that formula — never hardcode a title-bar height at a call site.
  [[nodiscard]] int titleBarHeight() const {
    return bandHeight() + 1;
  }
};

}  // namespace PJ
