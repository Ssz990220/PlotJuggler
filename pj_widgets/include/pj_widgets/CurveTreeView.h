#pragma once

#include <QByteArray>
#include <QMimeData>
#include <QStringList>
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

  struct CurvePath {
    QString key;
    QString dataset;
    QString topic;
    QString field;
    bool selectable = true;
  };

  explicit CurveTreeView(QWidget* parent = nullptr);

  [[nodiscard]] static QString catalogItemsMimeType();
  [[nodiscard]] static QByteArray encodeCatalogKeys(const QStringList& keys);
  [[nodiscard]] static QStringList decodeCatalogKeys(const QMimeData* mime_data);

  void addCurve(const QString& name);
  void addCurve(const CurvePath& path);
  void addCatalogItem(const CurvePath& path);
  void clearCurves();
  void applyFilter(const QString& filter);
  std::vector<QString> selectedCurveNames() const;
  // selectedCurveNames() returns only directly-selected leaves; this variant
  // expands selected group nodes to all their leaf descendants. Result is
  // sorted and deduplicated.
  std::vector<QString> selectedCurveNamesRecursive() const;
  // Returns catalog item keys for selected nodes, including object-topic
  // branch nodes. Scalar-only curve selection remains available through
  // selectedCurveNamesRecursive().
  std::vector<QString> selectedCatalogKeysRecursive() const;

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
  QString treePathFromCurvePath(const CurvePath& path) const;
  void sortTree();
  void setDescendantsExpanded(QTreeWidgetItem* item, bool expanded);
  std::vector<QString> selectedCurveNamesForDrag() const;

  QPoint drag_start_pos_;
  Qt::MouseButton drag_button_ = Qt::NoButton;
  std::vector<QString> drag_curve_names_;
  QStringList drag_catalog_keys_;
  bool suppress_next_release_ = false;
  QString last_filter_;
  DragSelectionProvider drag_selection_provider_;
};

}  // namespace PJ
