#pragma once

#include <QTreeWidget>

#include <vector>

namespace PJ {

// Hierarchical tree of curves with two columns (name, value-at-tracker).
// Hosts the drag-source logic that emits the PJ3-compatible mime types
// ("curveslist/add_curve", "curveslist/new_XY_axis").
//
// Trimmed port of PJ3 plotjuggler_app/curvetree_view.{h,cpp}: the tree
// structure and drag contract are preserved; the TreeCompleter / regex
// filter / values column refresh are simplified pending CatalogModel wiring.
class CurveTreeView : public QTreeWidget {
  Q_OBJECT
 public:
  explicit CurveTreeView(QWidget* parent = nullptr);

  void addCurve(const QString& name);
  void clearCurves();
  void applyFilter(const QString& filter);
  std::vector<QString> selectedCurveNames() const;

  void setValuesColumnHidden(bool hidden);
  bool valuesColumnHidden() const { return isColumnHidden(1); }

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;

 private:
  QTreeWidgetItem* ensureGroup(const QString& path);

  QPoint drag_start_pos_;
  Qt::MouseButton drag_button_ = Qt::NoButton;
};

}  // namespace PJ
