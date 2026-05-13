#pragma once

#include <QTreeWidget>
#include <functional>
#include <vector>

namespace PJ {

// Hierarchical tree of curves with two columns (name, value-at-tracker).
// Drag source emits "curveslist/add_curve" (left drag) or
// "curveslist/new_XY_axis" (right drag of exactly two curves).
class CurveTreeView : public QTreeWidget {
  Q_OBJECT
 public:
  using DragSelectionProvider = std::function<std::vector<QString>()>;

  explicit CurveTreeView(QWidget* parent = nullptr);

  void addCurve(const QString& name);
  void clearCurves();
  void applyFilter(const QString& filter);
  std::vector<QString> selectedCurveNames() const;
  // selectedCurveNames() returns only directly-selected leaves; this variant
  // expands selected group nodes to all their leaf descendants. Result is
  // sorted and deduplicated.
  std::vector<QString> selectedCurveNamesRecursive() const;

  void setValuesColumnHidden(bool hidden);
  bool valuesColumnHidden() const {
    return isColumnHidden(1);
  }
  void setDragSelectionProvider(DragSelectionProvider provider);

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;

 private:
  QTreeWidgetItem* ensureGroup(const QString& path);
  void sortTree();
  void setDescendantsExpanded(QTreeWidgetItem* item, bool expanded);
  std::vector<QString> selectedCurveNamesForDrag() const;

  QPoint drag_start_pos_;
  Qt::MouseButton drag_button_ = Qt::NoButton;
  std::vector<QString> drag_curve_names_;
  bool suppress_next_release_ = false;
  QString last_filter_;
  DragSelectionProvider drag_selection_provider_;
};

}  // namespace PJ
