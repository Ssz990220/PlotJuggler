#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QCheckBox>
#include <QDomDocument>
#include <QDomElement>
#include <QPoint>
#include <QStringList>
#include <QWidget>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_widgets/ChromeMetrics.h"

class QAction;
class QPushButton;

namespace Ui {
class CurveListPanel;
}

namespace PJ {

class CatalogModel;
struct CatalogItem;
class CurveTreeView;

// Timeseries list + Custom Series section. Top tree mirrors CatalogModel;
// bottom tree is the user's custom/derived series.
class CurveListPanel : public QWidget {
  Q_OBJECT
 public:
  explicit CurveListPanel(QWidget* parent = nullptr);
  ~CurveListPanel() override;

  void setCatalog(CatalogModel* catalog);

  void refreshValues(double tracker_time);

  // Builds <curve_list_state show_topics="..." show_values="..."
  // datasets_filter="..." custom_filter="..."/> — filter text plus
  // display-mode toggles.
  [[nodiscard]] QDomElement saveListState(QDomDocument& doc) const;

  // Applies <curve_list_state> attributes individually; missing or
  // mismatched values are silently ignored. Sets applying_state_
  // around the toggle calls so QSettings stays untouched.
  void restoreListState(const QDomElement& element);

 signals:
  void createCustomSeriesRequested();
  void deleteCustomSeriesRequested(QString name);
  // covers_all is true when the panel determined the selection (or its
  // empty-implies-all interpretation) targets every known curve. MainWindow
  // decides whether to prompt the user.
  void trashRequested(QStringList names, bool covers_all);
  // Emitted when the user picks "Clear All" from the datasets menu.
  void clearAllCurvesRequested();
  // Emitted after the user confirms "Remove dataset"; the panel has already
  // resolved the dataset node to a DatasetId.
  void removeDatasetRequested(DatasetId dataset_id);

 public slots:
  void onStylesheetChanged(QString theme);
  // Rebinds Chrome metrics broadcast from MainWindow. Resizes header
  // bands to (icon_size + icon_padding) + 2 * layout_padding tall,
  // sizes the chrome buttons to (icon_size + icon_padding) square,
  // pushes layout_padding as contentsMargins on the header band
  // layouts, and uses layout_spacing for both the in-band spacing and
  // the per-row vertical padding of the Datasets / Custom Series tree
  // views (via per-instance QSS).
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 protected:
  // Hides the sibling label / action buttons in each header band when
  // its filter QLineEdit gets focus, so the input expands to fill the
  // row. Restores them on focus loss.
  bool eventFilter(QObject* watched, QEvent* event) override;

 private slots:
  void onFilterChanged(const QString& text);
  void onCustomFilterChanged(const QString& text);
  void onShowValuesToggled(bool show);
  void onPreserveTopicNameToggled(bool checked);
  void onTrashClicked();
  // Right-click on a dataset node → "Remove dataset" + confirmation dialog.
  void onTreeContextMenu(const QPoint& pos);

 private:
  void onCatalogItemsAdded(const std::vector<CatalogItem>& items);
  void onCatalogItemsRemoved(const QStringList& keys);
  void onCatalogCleared();
  void applyIcons(QString theme);
  std::vector<QString> selectedCurveNamesForDrag() const;

  Ui::CurveListPanel* ui_;
  CatalogModel* catalog_ = nullptr;
  CurveTreeView* tree_view_ = nullptr;
  CurveTreeView* custom_view_ = nullptr;
  // QPushButtons hosted inside QWidgetAction items in the section
  // dropdown menus. Kept as members so applyIcons() can retint their
  // leading icons on theme switch.
  QPushButton* clear_all_button_ = nullptr;
  QPushButton* delete_custom_button_ = nullptr;
  // Show Values and Preserve Topic Name checkboxes live inside the
  // datasets popup menu. Held as members so restoreListState can flip
  // them without rummaging through the menu's children.
  QCheckBox* show_values_check_ = nullptr;
  QCheckBox* preserve_topic_name_check_ = nullptr;

  // Set to true around restoreListState so the Preserve-Topic-Name slot
  // suppresses its QSettings write. Layout-driven changes mutate the UI
  // but must not mutate the global per-user default.
  bool applying_state_ = false;
  // Chrome metrics from MainWindow::chromeMetricsChanged.
  ChromeMetrics chrome_metrics_;
};

}  // namespace PJ
