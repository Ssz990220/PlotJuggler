#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QStringList>
#include <QWidget>
#include <vector>

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
// Icons are visual affordances only; drops decide which concrete widget to
// create.
class VisualizationPlaceholderWidget : public QWidget {
  Q_OBJECT
 public:
  explicit VisualizationPlaceholderWidget(QWidget* parent = nullptr);

 public slots:
  // Re-tints the Plot / 2D / 3D icons through LoadSvg so they pick up
  // the active theme's ink (light => #3D3D3D, dark => #E0E0E0) the same
  // way every other chrome icon in the app does. DockWidget routes
  // MainWindow's stylesheetChanged signal here.
  void onStylesheetChanged(const QString& theme);

 signals:
  void catalogItemsDropped(QStringList keys);
  void splitHorizontalRequested();
  void splitVerticalRequested();

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
  std::vector<IconButton> icon_buttons_;
};

}  // namespace PJ
