#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QCheckBox>
#include <QDomDocument>
#include <QDomElement>
#include <QList>
#include <QPoint>
#include <QStringList>
#include <QWidget>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_widgets/ChromeMetrics.h"

class QAction;
class QPushButton;
class QTimer;

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
  // The user chose "Remove dataset(s)" on a dataset selection. The panel resolved
  // the selected dataset nodes to ids; MainWindow confirms (one combined dialog)
  // and performs the removal.
  void removeDatasetsRequested(const QList<DatasetId>& dataset_ids);
  // The user chose "Merge" on a multi-dataset selection (≥2). MainWindow shows the
  // shared destructive-merge confirmation and performs the merge.
  void mergeDatasetsRequested(const QList<DatasetId>& dataset_ids);

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
  // Right-click on a dataset node → Merge (≥2 selected) + Remove dataset(s) menu,
  // operating on the selected top-level dataset nodes. Emits intents; MainWindow
  // confirms + performs.
  void onTreeContextMenu(const QPoint& pos);

 private:
  void onCatalogItemsAdded(const std::vector<CatalogItem>& items);
  void onCatalogItemsRemoved(const QStringList& keys);
  void onCatalogCleared();
  void applyIcons(QString theme);
  std::vector<QString> selectedCurveNamesForDrag() const;

  // True when there is a catalog and at least one tree's Value column is shown —
  // i.e. there is anything to fill.
  [[nodiscard]] bool valuesColumnActive() const;
  // Reads each visible scalar leaf at last_tracker_time_ and writes the formatted
  // value. The unthrottled body behind refreshValues().
  void fillValuesNow();

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

  // Set to true around restoreListState so the toggle slots suppress their
  // QSettings writes. Layout-driven changes mutate the UI but must not mutate
  // the global per-user defaults.
  bool applying_state_ = false;
  // Last tracker time pushed via refreshValues(), in display-axis seconds. Reused
  // when a non-tracker event (Show Values toggle) needs to re-fill the column at
  // the current cursor instead of waiting for the next playback tick.
  double last_tracker_time_ = 0.0;
  // Caps the value-column refresh to ~10 Hz: playback emits tracker updates up to
  // ~60 Hz, but the column only needs to track the eye. Leading + trailing edge
  // (immediate first fill, then at most one per window, plus a final catch-up so
  // the value where playback stops is shown). value_refresh_pending_ marks that a
  // tracker update arrived mid-window and a trailing fill is owed.
  QTimer* value_throttle_timer_ = nullptr;
  bool value_refresh_pending_ = false;
  // Chrome metrics from MainWindow::chromeMetricsChanged.
  ChromeMetrics chrome_metrics_;
};

}  // namespace PJ
