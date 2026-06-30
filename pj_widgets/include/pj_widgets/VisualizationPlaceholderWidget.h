#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QStringList>
#include <QWidget>
#include <vector>

#include "pj_widgets/VisualizationKind.h"

class QAction;
class QContextMenuEvent;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QObject;
class QPoint;
class QToolButton;

namespace PJ {

// Neutral dock content shown before the user chooses a visualization family.
// The Plot / 2D / 3D icons are clickable: a click emits visualizationRequested
// so the host can convert the dock into an *empty* widget of that family. A
// catalog drop still creates-and-populates the matching widget directly.
class VisualizationPlaceholderWidget : public QWidget {
  Q_OBJECT
 public:
  explicit VisualizationPlaceholderWidget(QWidget* parent = nullptr);
  // Enables Paste when the host sees compatible widget XML in the clipboard.
  void setPasteActionEnabled(bool enabled);

 public slots:
  // Re-tints the Plot / 2D / 3D icons through LoadSvg so they pick up
  // the active theme's ink (light => #3D3D3D, dark => #E0E0E0) the same
  // way every other chrome icon in the app does. DockWidget routes
  // MainWindow's stylesheetChanged signal here.
  void onStylesheetChanged(const QString& theme);

 signals:
  void catalogItemsDropped(QStringList keys);
  // Emitted instead of catalogItemsDropped when the drop is the "create XY plot"
  // gesture (a right-drag of exactly two curves — the new_XY_axis mime). The host
  // turns this into an XY plot rather than two time-series curves.
  void catalogItemsXyRequested(QStringList keys);
  void splitHorizontalRequested();
  void splitVerticalRequested();
  // Emitted when the placeholder Paste menu action is triggered.
  void pasteRequested();
  // Emitted just before the context menu is built so Paste state can refresh.
  void contextMenuAboutToShow();
  // Emitted when the user clicks one of the family icons. The host converts the
  // placeholder into an empty widget of that family (no data bound yet).
  void visualizationRequested(VisualizationKind kind);

 protected:
  void contextMenuEvent(QContextMenuEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dragMoveEvent(QDragMoveEvent* event) override;
  void dropEvent(QDropEvent* event) override;

 private:
  void showSplitContextMenu(const QPoint& global_pos);
  void updateSplitActionIcons(const QString& theme);

  struct IconButton {
    QToolButton* button;
    QString icon_path;
  };
  QAction* action_split_horizontal_ = nullptr;
  QAction* action_split_vertical_ = nullptr;
  QAction* action_paste_ = nullptr;
  std::vector<IconButton> icon_buttons_;
};

}  // namespace PJ
