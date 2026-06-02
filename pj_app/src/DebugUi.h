#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>

class QMainWindow;
class QWidget;

namespace PJ {

class Theme;

// Self-contained dev-time UI inspector. Two toggles, two shortcuts:
//
//   Ctrl+Shift+D — paints a translucent dashed outline around every
//                  visible descendant widget of the host (mouse-
//                  transparent, layout-neutral).
//
//   Ctrl+Shift+Q — appends a debug QSS layer to the active theme that
//                  draws 1px dashed borders on every common Qt widget
//                  class. Shifts layout by 1px — intentional, accepted.
//
// Lifecycle is tied to the host: installInto(window, theme) parents a
// DebugUi to the window; closing the window destroys everything,
// including the overlay widget, shortcuts, and the QSS hook. To remove
// the feature entirely, delete this file pair + its sole call site in
// MainWindow.
class DebugUi : public QObject {
  Q_OBJECT
 public:
  // Wires both toggles onto host (overlay paints over its subtree) and
  // hooks Theme::qssChanged so the debug layer survives theme switches.
  static void installInto(QMainWindow* host, Theme* theme);

  explicit DebugUi(QMainWindow* host, Theme* theme);

 private:
  void toggleOverlay();
  void toggleQssLayer();
  void applyQss();

  QMainWindow* host_;
  Theme* theme_;
  QWidget* overlay_ = nullptr;
  bool qss_enabled_ = false;
};

}  // namespace PJ
