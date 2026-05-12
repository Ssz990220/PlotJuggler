#include "ui/CurveListPanel.h"

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>

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

  applyIcons(currentTheme());

  connect(ui_->lineEditFilter, &QLineEdit::textChanged, this, &CurveListPanel::onFilterChanged);
  connect(ui_->lineEditCustomFilter, &QLineEdit::textChanged, this, &CurveListPanel::onCustomFilterChanged);
  connect(ui_->checkBoxShowValues, &QCheckBox::toggled, this, &CurveListPanel::onShowValuesToggled);
  connect(ui_->pushButtonTrash, &QPushButton::clicked, this, &CurveListPanel::onTrashClicked);

  connect(ui_->buttonAddCustom, &QPushButton::clicked, this, &CurveListPanel::createCustomSeriesRequested);
  connect(ui_->buttonEditCustom, &QPushButton::clicked, this, [this]() {
    auto names = custom_view_->selectedCurveNames();
    if (!names.empty()) {
      emit editCustomSeriesRequested(names.front());
    }
  });
  connect(ui_->buttonDeleteCustom, &QPushButton::clicked, this, [this]() {
    auto names = custom_view_->selectedCurveNames();
    if (!names.empty()) {
      emit deleteCustomSeriesRequested(names.front());
    }
  });

  tree_view_->setValuesColumnHidden(true);
  custom_view_->setValuesColumnHidden(true);
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

void CurveListPanel::applyIcons(QString theme) {
  ui_->pushButtonTrash->setIcon(LoadSvg(":/resources/svg/trash.svg", theme));
  ui_->buttonEditCustom->setIcon(LoadSvg(":/resources/svg/pencil-edit.svg", theme));
  ui_->buttonDeleteCustom->setIcon(LoadSvg(":/resources/svg/delete_forever.svg", theme));
}

}  // namespace PJ
