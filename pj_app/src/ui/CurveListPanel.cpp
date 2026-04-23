#include "ui/CurveListPanel.h"

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>

#include "pj_app_core/CatalogModel.h"
#include "pj_app_core/SvgUtil.h"
#include "ui/CurveTreeView.h"
#include "ui/LineEdit.h"
#include "ui_CurveListPanel.h"

namespace PJ {

CurveListPanel::CurveListPanel(QWidget* parent)
    : QWidget(parent), ui_(new Ui::CurveListPanel) {
  ui_->setupUi(this);

  tree_view_ = ui_->treeView;
  custom_view_ = ui_->customView;

  // Splitter ratio per PJ3 (top timeseries gets more room than custom).
  ui_->verticalSplitter->setStretchFactor(0, 5);
  ui_->verticalSplitter->setStretchFactor(1, 1);

  QSettings settings;
  const QString theme = settings.value("StyleSheet::theme", "light").toString();
  ui_->pushButtonTrash->setIcon(LoadSvg(":/resources/svg/trash.svg", theme));
  ui_->buttonEditCustom->setIcon(LoadSvg(":/resources/svg/pencil-edit.svg", theme));
  ui_->buttonDeleteCustom->setIcon(LoadSvg(":/resources/svg/delete_forever.svg", theme));

  connect(ui_->lineEditFilter, &QLineEdit::textChanged, this, &CurveListPanel::onFilterChanged);
  connect(ui_->lineEditCustomFilter, &QLineEdit::textChanged, this,
          &CurveListPanel::onCustomFilterChanged);
  connect(ui_->checkBoxShowValues, &QCheckBox::toggled, this,
          &CurveListPanel::onShowValuesToggled);
  connect(ui_->pushButtonTrash, &QPushButton::clicked, this, &CurveListPanel::onTrashClicked);

  connect(ui_->buttonAddCustom, &QPushButton::clicked, this,
          &CurveListPanel::createCustomSeriesRequested);
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

  // Default: values column hidden until the user asks for it.
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
  // No-op for the prototype. Once CatalogModel can serve values the per-row
  // second column is populated here.
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
  emit clearAllCurvesRequested();
}

void CurveListPanel::onCurveAdded(const QString& name) {
  tree_view_->addCurve(name);
}

void CurveListPanel::onCurveRemoved(const QString& /*name*/) {
  // Linear rebuild is fine for the prototype; CurveTreeView has no
  // efficient remove-by-name yet.
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

}  // namespace PJ
