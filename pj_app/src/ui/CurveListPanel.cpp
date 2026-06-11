// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/CurveListPanel.h"

#include <QAction>
#include <QCheckBox>
#include <QDomDocument>
#include <QDomElement>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMargins>
#include <QMenu>
#include <QPoint>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSettings>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidgetItem>
#include <QWidgetAction>
#include <algorithm>
#include <array>

#include "pj_runtime/CatalogModel.h"
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/MessageBox.h"
#include "pj_widgets/SvgUtil.h"
#include "scene_object_classification.h"
#include "ui_CurveListPanel.h"

namespace PJ {

namespace {

constexpr auto kPreserveTopicNameKey = "CurveListPanel/show_topics";

// The image icon marks "2D-viewable" in general: stills (raw or compressed),
// video streams, depth images, and image annotations all carry it.
bool is2dMediaObjectType(sdk::BuiltinObjectType type) {
  return type == sdk::BuiltinObjectType::kImage || type == sdk::BuiltinObjectType::kVideoFrame ||
         type == sdk::BuiltinObjectType::kDepthImage || type == sdk::BuiltinObjectType::kImageAnnotations;
}

CurveTreeView::CurvePath treePathFromCatalogItem(const CatalogItem& item) {
  const auto* scalar = asScalarField(item);
  const auto* object_topic = asObjectTopic(item);
  const auto object_type = object_topic != nullptr ? object_topic->object_type : sdk::BuiltinObjectType::kNone;
  return CurveTreeView::CurvePath{
      .key = item.key,
      .dataset = item.dataset_name,
      .topic = item.topic_name,
      .field = scalar != nullptr ? scalar->field_name : QString{},
      .selectable = scalar != nullptr,
      .is_image_topic = is2dMediaObjectType(object_type),
      .is_3d_object_topic = is3dSceneObjectType(object_type),
  };
}

void addCatalogItems(CurveTreeView* tree_view, const std::vector<CatalogItem>& items) {
  if (tree_view == nullptr || items.empty()) {
    return;
  }
  std::vector<CurveTreeView::CurvePath> paths;
  paths.reserve(items.size());
  for (const CatalogItem& item : items) {
    paths.push_back(treePathFromCatalogItem(item));
  }
  tree_view->addCatalogItems(paths);
}

void rebuildTree(CurveTreeView* tree_view, CatalogModel* catalog) {
  tree_view->clearCurves();
  if (catalog == nullptr) {
    return;
  }
  addCatalogItems(tree_view, catalog->items());
}

}  // namespace

CurveListPanel::CurveListPanel(QWidget* parent) : QWidget(parent), ui_(new Ui::CurveListPanel) {
  ui_->setupUi(this);

  tree_view_ = ui_->treeView;
  custom_view_ = ui_->customView;

  // Top tree takes more room than the custom series panel.
  ui_->verticalSplitter->setStretchFactor(0, 5);
  ui_->verticalSplitter->setStretchFactor(1, 1);

  // Leading search icons attached before applyIcons() so the icon
  // refresh sees them. Zero text margins so the leading action sits
  // flush against the line edit's left edge instead of getting style-
  // default inset.
  // No leading-action search icons — QLineEdit's internal
  // QLineEditIconButton hardcodes its rendered icon to 16 px for any
  // line edit shorter than 34 px (see QLineEditPrivate::
  // sideWidgetParameters in Qt source), so setIconSize is ignored and
  // the magnifying glass paints with visible padding inside the
  // 20-px chrome button. Instead, the .ui keeps the search button as
  // a sibling QToolButton next to the filter line edit, sized at the
  // standard 20×20 with no extra chrome.
  ui_->lineEditFilter->setTextMargins(0, 0, 0, 0);
  ui_->lineEditCustomFilter->setTextMargins(0, 0, 0, 0);

  // Datasets header overflow menu — view toggles + Clear All
  // (destructive, so styled red).
  auto* datasets_menu = new QMenu(this);
  datasets_menu->setObjectName(QStringLiteral("PJMenu"));

  show_values_check_ = new QCheckBox(tr("Show Values"), datasets_menu);
  auto* show_values_action = new QWidgetAction(datasets_menu);
  show_values_action->setDefaultWidget(show_values_check_);
  datasets_menu->addAction(show_values_action);
  connect(show_values_check_, &QCheckBox::toggled, this, &CurveListPanel::onShowValuesToggled);

  preserve_topic_name_check_ = new QCheckBox(tr("Preserve Topic Name"), datasets_menu);
  // Seed the checkbox AND the tree view mode from QSettings before wiring
  // the signal — that way the first rebuildTree() driven by setCatalog()
  // already lays out under the saved mode (no rebuild thrash on startup).
  QSettings settings;
  const bool preserve_topic_name = settings.value(QLatin1String(kPreserveTopicNameKey), true).toBool();
  preserve_topic_name_check_->setChecked(preserve_topic_name);
  tree_view_->setViewMode(
      preserve_topic_name ? CurveTreeView::ViewMode::ShowTopics : CurveTreeView::ViewMode::Hierarchical);
  auto* preserve_topic_name_action = new QWidgetAction(datasets_menu);
  preserve_topic_name_action->setDefaultWidget(preserve_topic_name_check_);
  datasets_menu->addAction(preserve_topic_name_action);
  connect(preserve_topic_name_check_, &QCheckBox::toggled, this, &CurveListPanel::onPreserveTopicNameToggled);

  datasets_menu->addSeparator();

  // QWidgetAction wraps a flat QPushButton so we can colour the text
  // red — QMenu's default item painter doesn't expose a per-action
  // colour the way QSS would for a regular QPushButton.
  clear_all_button_ = new QPushButton(tr("Remove all Datasets"), datasets_menu);
  clear_all_button_->setFlat(true);
  clear_all_button_->setProperty("destructive", true);
  // Padding, text-align, AND the destructive ${purple} colour
  // are handled centrally in stylesheet_*.qss under
  // `QMenu#PJMenu QPushButton[destructive="true"]` — no per-button
  // stylesheet needed here.
  auto* clear_all_action = new QWidgetAction(datasets_menu);
  clear_all_action->setDefaultWidget(clear_all_button_);
  datasets_menu->addAction(clear_all_action);
  connect(clear_all_button_, &QPushButton::clicked, this, [this, datasets_menu]() {
    datasets_menu->hide();
    emit clearAllCurvesRequested();
  });

  connect(ui_->buttonDatasetsMenu, &QToolButton::clicked, this, [this, datasets_menu]() {
    const QPoint anchor = ui_->buttonDatasetsMenu->mapToGlobal(QPoint(0, ui_->buttonDatasetsMenu->height()));
    datasets_menu->popup(anchor);
  });

  applyIcons(currentTheme());

  // Both filter line edits are inline in their respective header bands.
  connect(ui_->lineEditFilter, &QLineEdit::textChanged, this, &CurveListPanel::onFilterChanged);
  connect(ui_->lineEditCustomFilter, &QLineEdit::textChanged, this, &CurveListPanel::onCustomFilterChanged);

  // Enter while typing drops focus back to the panel — restores the
  // sibling label + action buttons (via the focus-out branch of
  // eventFilter) without forcing the user to click elsewhere.
  connect(ui_->lineEditFilter, &QLineEdit::returnPressed, ui_->lineEditFilter, &QLineEdit::clearFocus);
  connect(ui_->lineEditCustomFilter, &QLineEdit::returnPressed, ui_->lineEditCustomFilter, &QLineEdit::clearFocus);

  // While the filter has focus, hide its sibling label + buttons so the
  // input takes the full header width. Restored on focus loss.
  ui_->lineEditFilter->installEventFilter(this);
  ui_->lineEditCustomFilter->installEventFilter(this);

  // Lock each header band to its natural height so hiding the siblings
  // can't shrink the row and shift the line edit's vertical centre.
  // QHBoxLayout vertically centres items, so even a 1-2px drop in the
  // row's preferred height (when the tallest sibling hides) was enough
  // to nudge the line edit upwards on focus.
  ui_->widgetLabelTimeseries->layout()->activate();
  ui_->widgetLabelCustom->layout()->activate();
  ui_->widgetLabelTimeseries->setFixedHeight(ui_->widgetLabelTimeseries->layout()->sizeHint().height());
  ui_->widgetLabelCustom->setFixedHeight(ui_->widgetLabelCustom->layout()->sizeHint().height());

  connect(ui_->buttonAddCustom, &QToolButton::clicked, this, &CurveListPanel::createCustomSeriesRequested);

  // Custom-series header overflow menu — mirrors the Datasets menu.
  // Delete is destructive so it gets the same red treatment.
  auto* custom_menu = new QMenu(this);
  custom_menu->setObjectName(QStringLiteral("PJMenu"));
  delete_custom_button_ = new QPushButton(tr("Delete"), custom_menu);
  delete_custom_button_->setFlat(true);
  delete_custom_button_->setProperty("destructive", true);
  auto* delete_custom_action = new QWidgetAction(custom_menu);
  delete_custom_action->setDefaultWidget(delete_custom_button_);
  custom_menu->addAction(delete_custom_action);
  connect(delete_custom_button_, &QPushButton::clicked, this, [this, custom_menu]() {
    custom_menu->hide();
    auto names = custom_view_->selectedCurveNames();
    if (!names.empty()) {
      emit deleteCustomSeriesRequested(names.front());
    }
  });
  connect(ui_->buttonCustomMenu, &QToolButton::clicked, this, [this, custom_menu]() {
    const QPoint anchor = ui_->buttonCustomMenu->mapToGlobal(QPoint(0, ui_->buttonCustomMenu->height()));
    custom_menu->popup(anchor);
  });

  tree_view_->setValuesColumnHidden(true);
  custom_view_->setValuesColumnHidden(true);

  auto drag_selection_provider = [this]() { return selectedCurveNamesForDrag(); };
  tree_view_->setDragSelectionProvider(drag_selection_provider);
  custom_view_->setDragSelectionProvider(drag_selection_provider);

  tree_view_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(tree_view_, &QWidget::customContextMenuRequested, this, &CurveListPanel::onTreeContextMenu);
}

CurveListPanel::~CurveListPanel() {
  delete ui_;
}

void CurveListPanel::setCatalog(CatalogModel* catalog) {
  if (catalog_ == catalog) {
    return;
  }
  if (catalog_) {
    disconnect(catalog_, nullptr, this, nullptr);
  }
  catalog_ = catalog;
  rebuildTree(tree_view_, catalog_);
  if (!catalog_) {
    return;
  }
  connect(catalog_, &CatalogModel::itemsAdded, this, &CurveListPanel::onCatalogItemsAdded);
  connect(catalog_, &CatalogModel::itemsRemoved, this, &CurveListPanel::onCatalogItemsRemoved);
  connect(catalog_, &CatalogModel::cleared, this, &CurveListPanel::onCatalogCleared);
}

void CurveListPanel::refreshValues(double /*tracker_time*/) {
  // TODO: populate the second column from CatalogModel once it serves values.
}

QDomElement CurveListPanel::saveListState(QDomDocument& doc) const {
  QDomElement element = doc.createElement(QStringLiteral("curve_list_state"));

  if (preserve_topic_name_check_ != nullptr) {
    element.setAttribute(
        QStringLiteral("show_topics"),
        preserve_topic_name_check_->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
  }
  if (show_values_check_ != nullptr) {
    element.setAttribute(
        QStringLiteral("show_values"),
        show_values_check_->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
  }
  if (ui_->lineEditFilter != nullptr) {
    element.setAttribute(QStringLiteral("datasets_filter"), ui_->lineEditFilter->text());
  }
  if (ui_->lineEditCustomFilter != nullptr) {
    element.setAttribute(QStringLiteral("custom_filter"), ui_->lineEditCustomFilter->text());
  }
  return element;
}

void CurveListPanel::restoreListState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != QStringLiteral("curve_list_state")) {
    return;
  }

  // Bracket the toggles with applying_state_ so onPreserveTopicNameToggled
  // doesn't write its QSettings key. setViewMode + rebuildTree still run.
  // QScopedValueRollback gives exception-safety: a throw inside any slot
  // restores the flag instead of leaving it stuck-true (which would
  // silently disable QSettings writes from later user-initiated toggles).
  // Matches MainWindow.cpp's pattern at the xmlLoadState path.
  {
    QScopedValueRollback guard(applying_state_, true);

    if (element.hasAttribute(QStringLiteral("show_topics")) && preserve_topic_name_check_ != nullptr) {
      const bool wanted = element.attribute(QStringLiteral("show_topics")) == QStringLiteral("true");
      if (preserve_topic_name_check_->isChecked() != wanted) {
        preserve_topic_name_check_->setChecked(wanted);  // emits toggled -> slot runs (rebuilds tree)
      }
    }

    if (element.hasAttribute(QStringLiteral("show_values")) && show_values_check_ != nullptr) {
      const bool wanted = element.attribute(QStringLiteral("show_values")) == QStringLiteral("true");
      if (show_values_check_->isChecked() != wanted) {
        show_values_check_->setChecked(wanted);  // emits toggled -> slot runs (no QSettings write)
      }
    }
  }

  // Filter texts: setText emits textChanged, which the connected slots
  // forward to tree_view_->applyFilter — that's exactly what we want.
  // Do NOT block signals here.
  if (element.hasAttribute(QStringLiteral("datasets_filter")) && ui_->lineEditFilter != nullptr) {
    ui_->lineEditFilter->setText(element.attribute(QStringLiteral("datasets_filter")));
  }
  if (element.hasAttribute(QStringLiteral("custom_filter")) && ui_->lineEditCustomFilter != nullptr) {
    ui_->lineEditCustomFilter->setText(element.attribute(QStringLiteral("custom_filter")));
  }
}

void CurveListPanel::onFilterChanged(const QString& text) {
  tree_view_->applyFilter(text);
}

void CurveListPanel::onCustomFilterChanged(const QString& text) {
  custom_view_->applyFilter(text);
}

void CurveListPanel::onShowValuesToggled(bool show) {
  tree_view_->setValuesColumnHidden(!show);
  custom_view_->setValuesColumnHidden(!show);
}

void CurveListPanel::onPreserveTopicNameToggled(bool checked) {
  if (!applying_state_) {
    QSettings settings;
    settings.setValue(QLatin1String(kPreserveTopicNameKey), checked);
  }
  tree_view_->setViewMode(checked ? CurveTreeView::ViewMode::ShowTopics : CurveTreeView::ViewMode::Hierarchical);
  rebuildTree(tree_view_, catalog_);
  tree_view_->applyFilter(ui_->lineEditFilter->text());
}

void CurveListPanel::onTrashClicked() {
  const auto selected = tree_view_->selectedCatalogKeysRecursive();
  const std::size_t total = catalog_ != nullptr ? catalog_->items().size() : 0;
  const bool covers_all = selected.empty() || (total > 0 && selected.size() >= total);
  emit trashRequested(QStringList(selected.begin(), selected.end()), covers_all);
}

void CurveListPanel::onTreeContextMenu(const QPoint& pos) {
  // Dataset is the only top-level group (label has no '/', has >=1 topic child).
  // Topic/curve nodes get no menu.
  QTreeWidgetItem* item = tree_view_->itemAt(pos);
  if (item == nullptr || item->parent() != nullptr || item->childCount() == 0 || catalog_ == nullptr) {
    return;
  }
  const QString dataset_name = item->text(0);
  DatasetId dataset_id = 0;
  bool found = false;
  for (const auto& [id, name] : catalog_->datasets()) {
    if (name == dataset_name) {
      dataset_id = id;
      found = true;
      break;
    }
  }
  if (!found) {
    return;
  }

  QMenu menu(this);
  menu.setObjectName(QStringLiteral("PJMenu"));
  QAction* remove_action = menu.addAction(tr("Remove dataset"));
  if (menu.exec(tree_view_->viewport()->mapToGlobal(pos)) != remove_action) {
    return;
  }

  const int choice = MessageBox::question(
      this, tr("Remove dataset"), tr("Are you sure you want to remove '%1' and its data?").arg(dataset_name),
      {{tr("Remove"), MessageBox::DestructiveRole}, {tr("Cancel"), MessageBox::CancelRole}});
  if (choice == 0) {
    emit removeDatasetRequested(dataset_id);
  }
}

void CurveListPanel::onCatalogItemsAdded(const std::vector<CatalogItem>& items) {
  addCatalogItems(tree_view_, items);
}

void CurveListPanel::onCatalogItemsRemoved(const QStringList& /*keys*/) {
  // One rebuild per batch (a dataset / multi-key trash is a single itemsRemoved).
  // TODO: incremental CurveTreeView::removeCurve(name); linear rebuild wipes
  // scroll/expansion/selection.
  rebuildTree(tree_view_, catalog_);
}

void CurveListPanel::onCatalogCleared() {
  tree_view_->clearCurves();
}

void CurveListPanel::onStylesheetChanged(QString theme) {
  applyIcons(theme);
}

void CurveListPanel::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  chrome_metrics_ = metrics;
  applyIcons(currentTheme());
}

bool CurveListPanel::eventFilter(QObject* watched, QEvent* event) {
  const QEvent::Type type = event->type();
  if (type == QEvent::FocusIn || type == QEvent::FocusOut) {
    const bool focused = (type == QEvent::FocusIn);
    if (watched == ui_->lineEditFilter) {
      ui_->labelTimeseries->setVisible(!focused);
      ui_->buttonDatasetsMenu->setVisible(!focused);
    } else if (watched == ui_->lineEditCustomFilter) {
      ui_->labelCustom->setVisible(!focused);
      ui_->buttonAddCustom->setVisible(!focused);
      ui_->buttonCustomMenu->setVisible(!focused);
    }
  }
  return QWidget::eventFilter(watched, event);
}

void CurveListPanel::applyIcons(QString theme) {
  if (tree_view_ != nullptr) {
    tree_view_->refreshIcons(theme);
  }
  ui_->buttonDatasetsMenu->setIcon(LoadSvg(":/resources/svg/more_vert.svg", theme));
  ui_->buttonCustomMenu->setIcon(LoadSvg(":/resources/svg/more_vert.svg", theme));
  ui_->buttonAddCustom->setIcon(LoadSvg(":/resources/svg/add_tab.svg", theme));
  if (clear_all_button_ != nullptr) {
    clear_all_button_->setIcon(LoadSvg(":/resources/svg/trash.svg", theme));
  }
  if (delete_custom_button_ != nullptr) {
    delete_custom_button_->setIcon(LoadSvg(":/resources/svg/delete_forever.svg", theme));
  }
  const QIcon search_icon(LoadSvg(":/resources/svg/search_light.svg", theme));
  ui_->buttonSearchTimeseries->setIcon(search_icon);
  ui_->buttonSearchCustom->setIcon(search_icon);

  // Resize chrome buttons in lock-step with the global icon metrics.
  // clear_all_button_ and delete_custom_button_ are inline-action menu
  // items (full-width inside a popup), not square chrome — skip them.
  const QSize icon_sz(chrome_metrics_.icon_size, chrome_metrics_.icon_size);
  const int button_extent = chrome_metrics_.icon_size + chrome_metrics_.icon_padding;
  const int band_extent = button_extent + (2 * chrome_metrics_.layout_padding);
  const std::array<QToolButton*, 5> chrome_buttons{
      ui_->buttonDatasetsMenu, ui_->buttonCustomMenu, ui_->buttonAddCustom, ui_->buttonSearchTimeseries,
      ui_->buttonSearchCustom};
  for (QToolButton* btn : chrome_buttons) {
    btn->setMinimumSize(button_extent, button_extent);
    btn->setMaximumSize(button_extent, button_extent);
    btn->setIconSize(icon_sz);
  }
  ui_->lineEditFilter->setMinimumHeight(button_extent);
  ui_->lineEditFilter->setMaximumHeight(button_extent);
  ui_->lineEditCustomFilter->setMinimumHeight(button_extent);
  ui_->lineEditCustomFilter->setMaximumHeight(button_extent);
  // Bands grow to band_extent so the contentsMargins applied to their
  // inner layouts (below) are absorbed by the band instead of squeezing
  // the chrome inside.
  ui_->widgetLabelTimeseries->setFixedHeight(band_extent);
  ui_->widgetLabelCustom->setFixedHeight(band_extent);
  const QMargins margins(
      chrome_metrics_.layout_padding, chrome_metrics_.layout_padding, chrome_metrics_.layout_padding,
      chrome_metrics_.layout_padding);
  if (auto* layout = ui_->timeseriesHeaderLayout) {
    layout->setContentsMargins(margins);
    layout->setSpacing(chrome_metrics_.layout_spacing);
  }
  if (auto* layout = ui_->customHeaderLayout) {
    layout->setContentsMargins(margins);
    layout->setSpacing(chrome_metrics_.layout_spacing);
  }
  // Per-row padding on the Datasets / Custom Series trees. QTreeView
  // has no setSpacing() the way QListWidget does — instead, push a
  // per-instance stylesheet that pads ::item by layout_spacing on top
  // and bottom. Setting an empty stylesheet at zero spacing clears the
  // rule (otherwise the previous value would linger).
  const QString row_padding = chrome_metrics_.layout_spacing > 0
                                  ? QStringLiteral("QTreeView::item { padding-top: %1px; padding-bottom: %1px; }")
                                        .arg(chrome_metrics_.layout_spacing)
                                  : QString();
  if (tree_view_ != nullptr) {
    tree_view_->setStyleSheet(row_padding);
  }
  if (custom_view_ != nullptr) {
    custom_view_->setStyleSheet(row_padding);
  }
}

std::vector<QString> CurveListPanel::selectedCurveNamesForDrag() const {
  std::vector<QString> names = tree_view_->selectedCurveNamesRecursive();
  std::vector<QString> custom_names = custom_view_->selectedCurveNamesRecursive();
  names.insert(names.end(), custom_names.begin(), custom_names.end());
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

}  // namespace PJ
