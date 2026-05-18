#pragma once

#include <QStringList>
#include <QWidget>

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QObject;

namespace PJ {

// Neutral dock content shown before the user chooses a visualization family.
// Icons are visual affordances only; drops decide which concrete widget to
// create.
class VisualizationPlaceholderWidget : public QWidget {
  Q_OBJECT
 public:
  explicit VisualizationPlaceholderWidget(QWidget* parent = nullptr);

 signals:
  void catalogItemsDropped(QStringList keys);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dragMoveEvent(QDragMoveEvent* event) override;
  void dropEvent(QDropEvent* event) override;
};

}  // namespace PJ
