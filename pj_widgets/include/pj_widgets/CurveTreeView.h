#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

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
    bool is_3d_object_topic = false;
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
  void addCurves(const std::vector<QString>& names);
  void addCurve(const CurvePath& path);
  void addCatalogItem(const CurvePath& path);
  void addCatalogItems(const std::vector<CurvePath>& paths);
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

  // Builds the MIME payload for a drag of the current selection: the
  // "curveslist/add_curve" / "curveslist/new_XY_axis" curve-name format and
  // the catalog-key format, each covering EVERY selected row (not just the row
  // under the cursor). Returns nullptr — and transfers ownership otherwise —
  // when the selection has nothing draggable for `button`. Exposed for tests
  // because the live drag path ends in a blocking QDrag::exec() that cannot be
  // driven from a unit test.
  [[nodiscard]] QMimeData* createDragMimeData(Qt::MouseButton button) const;

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
  enum class SortMode { Immediate, Deferred };

  void addCurve(const QString& name, SortMode sort_mode);
  void addCatalogItem(const CurvePath& path, SortMode sort_mode);
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
