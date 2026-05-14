#include "ui/CurveListPanel.h"

#include <QAction>
#include <QCheckBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPoint>
#include <QPushButton>
#include <QSplitter>
#include <QToolButton>
#include <QWidgetAction>
#include <algorithm>

#include "pj_runtime/CatalogModel.h"
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_CurveListPanel.h"

namespace PJ {

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

  // Datasets header overflow menu — Show Values toggle + Clear All
  // (destructive, so styled red).
  auto* datasets_menu = new QMenu(this);
  datasets_menu->setObjectName(QStringLiteral("PJMenu"));

  auto* show_values_check = new QCheckBox(tr("Show Values"), datasets_menu);
  auto* show_values_action = new QWidgetAction(datasets_menu);
  show_values_action->setDefaultWidget(show_values_check);
  datasets_menu->addAction(show_values_action);
  connect(show_values_check, &QCheckBox::toggled, this, &CurveListPanel::onShowValuesToggled);

  datasets_menu->addSeparator();

  // QWidgetAction wraps a flat QPushButton so we can colour the text
  // red — QMenu's default item painter doesn't expose a per-action
  // colour the way QSS would for a regular QPushButton.
  clear_all_button_ = new QPushButton(tr("Clear all curves"), datasets_menu);
  clear_all_button_->setFlat(true);
  clear_all_button_->setProperty("destructive", true);
  // Padding, text-align, AND the destructive ${PJPurple} colour
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
  tree_view_->clearCurves();
  if (!catalog_) {
    return;
  }
  connect(catalog_, &CatalogModel::curveAdded, this, &CurveListPanel::onCurveAdded);
  connect(catalog_, &CatalogModel::curveRemoved, this, &CurveListPanel::onCurveRemoved);
  connect(catalog_, &CatalogModel::cleared, this, &CurveListPanel::onCatalogCleared);
  for (const QString& name : catalog_->curveNames()) {
    tree_view_->addCurve(name);
  }
}

void CurveListPanel::refreshValues(double /*tracker_time*/) {
  // TODO: populate the second column from CatalogModel once it serves values.
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

void CurveListPanel::onTrashClicked() {
  const auto selected = tree_view_->selectedCurveNamesRecursive();
  const std::size_t total = catalog_ != nullptr ? catalog_->curveNames().size() : 0;
  const bool covers_all = selected.empty() || (total > 0 && selected.size() >= total);
  emit trashRequested(QStringList(selected.begin(), selected.end()), covers_all);
}

void CurveListPanel::onCurveAdded(const QString& name) {
  tree_view_->addCurve(name);
}

void CurveListPanel::onCurveRemoved(const QString& /*name*/) {
  // TODO: add CurveTreeView::removeCurve(name) — linear rebuild is a
  // prototype stand-in and wipes scroll / expansion / selection state.
  tree_view_->clearCurves();
  if (catalog_) {
    for (const QString& n : catalog_->curveNames()) {
      tree_view_->addCurve(n);
    }
  }
}

void CurveListPanel::onCatalogCleared() {
  tree_view_->clearCurves();
}

void CurveListPanel::onStylesheetChanged(QString theme) {
  applyIcons(theme);
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
