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
    bool is_image_topic = false;
  };

  // Hierarchical: split dataset/topic/field on every '/' (after '.' → '/').
  // ShowTopics: dataset and topic stay as literal nodes (topic shown
  // verbatim, e.g. "/camera/image"); the field still splits on '/' so
  // nested struct fields show up as sub-folders below the topic.
  enum class ViewMode { Hierarchical, ShowTopics };

  explicit CurveTreeView(QWidget* parent = nullptr);

  void setViewMode(ViewMode mode);
  [[nodiscard]] ViewMode viewMode() const {
    return view_mode_;
  }

  [[nodiscard]] static QString catalogItemsMimeType();
  [[nodiscard]] static QByteArray encodeCatalogKeys(const QStringList& keys);
  [[nodiscard]] static QStringList decodeCatalogKeys(const QMimeData* mime_data);

  void addCurve(const QString& name);
  void addCurve(const CurvePath& path);
  void addCatalogItem(const CurvePath& path);
  void clearCurves();
  void applyFilter(const QString& filter);
  void refreshIcons(const QString& theme);
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
  void resizeEvent(QResizeEvent* event) override;

 private:
  QTreeWidgetItem* ensureGroupSegments(const QStringList& segments);
  QTreeWidgetItem* ensureGroup(const QString& path);
  QString treePathFromCurvePath(const CurvePath& path) const;
  // Recompute the Name column so it fills whatever viewport width is
  // left over after the Value column. Used by the resize event handler
  // and the value-column show/hide toggle.
  void syncNameColumnWidth();
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
  // Re-entry guard for the header sectionResized handler: programmatic
  // resizes inside the handler re-fire the signal, which would otherwise
  // cause an infinite ping-pong between Name and Value.
  bool adjusting_columns_ = false;
  ViewMode view_mode_ = ViewMode::Hierarchical;
};

}  // namespace PJ
