#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <DockWidget.h>

#include <QWidget>

QT_BEGIN_NAMESPACE
class QEnterEvent;
class QLabel;
class QMouseEvent;
class QPushButton;
QT_END_NAMESPACE

namespace Ui {
class DockToolbar;
}

namespace PJ {

// Per-dock toolbar on top of each plot area: rename-on-double-click label,
// split-horizontal / split-vertical / fullscreen / close buttons.
class DockToolbar : public QWidget {
  Q_OBJECT
 public:
  explicit DockToolbar(ads::CDockWidget* parent);
  ~DockToolbar() override;

  QLabel* label();
  QPushButton* buttonFullscreen();
  QPushButton* buttonClose();
  QPushButton* buttonSplitHorizontal();
  QPushButton* buttonSplitVertical();

  void toggleFullscreen();
  bool isFullscreen() const {
    return fullscreen_mode_;
  }

  bool eventFilter(QObject* object, QEvent* event) override;

 public slots:
  void onStylesheetChanged(QString theme);

 signals:
  void titleChanged(QString title);

 private:
  void mousePressEvent(QMouseEvent* ev) override;
  void mouseReleaseEvent(QMouseEvent* ev) override;
  void mouseMoveEvent(QMouseEvent* ev) override;
  void enterEvent(QEnterEvent* ev) override;
  void leaveEvent(QEvent* ev) override;

  // Inline rename: double-clicking the label swaps it for an in-place
  // QLineEdit (matching the tab-rename UX). Enter commits, Escape or focus
  // loss reverts to the previous name.
  void enterRenameMode();
  void commitRename();
  void cancelRename();
  // Sizes the rename editor to its text: a 300 px default that grows to fit a
  // longer name (capped to the toolbar's available width).
  void updateRenameEditWidth();

  ads::CDockWidget* parent_dock_;
  Ui::DockToolbar* ui_;
  bool fullscreen_mode_ = false;
  bool rename_active_ = false;

  QIcon expand_icon_;
  QIcon collapse_icon_;
};

}  // namespace PJ
