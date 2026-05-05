#pragma once

#include <QTreeWidget>
#include <vector>

namespace PJ {

// Hierarchical tree of curves with two columns (name, value-at-tracker).
// Drag source emits "curveslist/add_curve" (left drag) or
// "curveslist/new_XY_axis" (right drag of exactly two curves).
class CurveTreeView : public QTreeWidget {
  Q_OBJECT
 public:
  explicit CurveTreeView(QWidget* parent = nullptr);

  void addCurve(const QString& name);
  void clearCurves();
  void applyFilter(const QString& filter);
  std::vector<QString> selectedCurveNames() const;

  void setValuesColumnHidden(bool hidden);
  bool valuesColumnHidden() const {
    return isColumnHidden(1);
  }

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;

 private:
  QTreeWidgetItem* ensureGroup(const QString& path);

  QPoint drag_start_pos_;
  Qt::MouseButton drag_button_ = Qt::NoButton;
  QString last_filter_;
};

}  // namespace PJ
