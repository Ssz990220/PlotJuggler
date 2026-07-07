// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/SnapshotGroupDialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <set>
#include <string>

#include "pj_plotting/SnapshotGroupResolver.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveDescriptor.h"

namespace PJ {
namespace {

QDoubleSpinBox* makeSpin(double value) {
  auto* spin = new QDoubleSpinBox;
  spin->setRange(-1.0e12, 1.0e12);
  spin->setDecimals(4);
  spin->setValue(value);
  return spin;
}

}  // namespace

SnapshotGroupDialog::SnapshotGroupDialog(CatalogModel* catalog, QWidget* parent)
    : Dialog(parent), catalog_(catalog) {
  setDialogTitle(tr("Add Snapshot Group"));

  auto* grid = new QGridLayout;
  int row = 0;

  grid->addWidget(new QLabel(tr("Topic:"), contentWidget()), row, 0);
  topic_combo_ = new QComboBox(contentWidget());
  grid->addWidget(topic_combo_, row++, 1, 1, 2);

  // Distinct topics (that have at least one plottable column), ordered by name.
  QSet<QString> seen;
  std::vector<CurveDescriptor> curves = catalog_ != nullptr ? catalog_->curves() : std::vector<CurveDescriptor>{};
  for (const CurveDescriptor& curve : curves) {
    const QString key = QStringLiteral("%1/%2").arg(curve.dataset_id).arg(curve.topic_id);
    if (seen.contains(key)) {
      continue;
    }
    seen.insert(key);
    topics_.push_back(TopicEntry{.dataset_id = curve.dataset_id, .topic_id = curve.topic_id});
    topic_combo_->addItem(curve.topic_name);
  }

  grid->addWidget(new QLabel(tr("X source:"), contentWidget()), row, 0);
  x_index_ = new QRadioButton(tr("Element index"), contentWidget());
  x_leaf_ = new QRadioButton(tr("Field:"), contentWidget());
  x_index_->setChecked(true);
  auto* x_group = new QButtonGroup(this);
  x_group->addButton(x_index_);
  x_group->addButton(x_leaf_);
  grid->addWidget(x_index_, row++, 1, 1, 2);
  x_combo_ = new QComboBox(contentWidget());
  x_combo_->setEnabled(false);
  grid->addWidget(x_leaf_, row, 0);
  grid->addWidget(x_combo_, row++, 1, 1, 2);

  grid->addWidget(new QLabel(tr("Y fields:"), contentWidget()), row, 0, Qt::AlignTop);
  y_list_ = new QListWidget(contentWidget());
  y_list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  grid->addWidget(y_list_, row++, 1, 1, 2);

  grid->addWidget(new QLabel(tr("Alias prefix:"), contentWidget()), row, 0);
  alias_edit_ = new QLineEdit(contentWidget());
  grid->addWidget(alias_edit_, row++, 1, 1, 2);

  // Optional manual y-range (same semantics as the Y Axis Range dialog).
  grid->addWidget(new QLabel(tr("Y max:"), contentWidget()), row, 0);
  max_spin_ = makeSpin(1.0);
  auto_max_ = new QCheckBox(tr("Auto"), contentWidget());
  auto_max_->setChecked(true);
  grid->addWidget(max_spin_, row, 1);
  grid->addWidget(auto_max_, row++, 2);
  grid->addWidget(new QLabel(tr("Y min:"), contentWidget()), row, 0);
  min_spin_ = makeSpin(-1.0);
  auto_min_ = new QCheckBox(tr("Auto"), contentWidget());
  auto_min_->setChecked(true);
  grid->addWidget(min_spin_, row, 1);
  grid->addWidget(auto_min_, row++, 2);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, contentWidget());
  ok_button_ = buttons->button(QDialogButtonBox::Ok);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  contentLayout()->addItem(grid);
  contentLayout()->addWidget(buttons);

  connect(topic_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { onTopicChanged(); });
  connect(x_leaf_, &QRadioButton::toggled, this, [this](bool on) {
    x_combo_->setEnabled(on);
    refreshOkState();
  });
  connect(y_list_, &QListWidget::itemSelectionChanged, this, [this]() { refreshOkState(); });
  const auto on_pin = [this]() {
    min_spin_->setEnabled(!auto_min_->isChecked());
    max_spin_->setEnabled(!auto_max_->isChecked());
    refreshOkState();
  };
  connect(auto_min_, &QCheckBox::toggled, this, on_pin);
  connect(auto_max_, &QCheckBox::toggled, this, on_pin);
  min_spin_->setEnabled(false);
  max_spin_->setEnabled(false);

  onTopicChanged();
}

void SnapshotGroupDialog::onTopicChanged() {
  x_combo_->clear();
  y_list_->clear();
  const int idx = topic_combo_->currentIndex();
  if (idx < 0 || idx >= static_cast<int>(topics_.size()) || catalog_ == nullptr) {
    dataset_id_ = 0;
    topic_id_ = 0;
    refreshOkState();
    return;
  }
  dataset_id_ = topics_[idx].dataset_id;
  topic_id_ = topics_[idx].topic_id;

  std::vector<SnapshotColumn> columns;
  for (const CurveDescriptor& curve : catalog_->curves()) {
    if (curve.topic_id == topic_id_) {
      columns.push_back(
          SnapshotColumn{.column_index = curve.column_index, .field_path = curve.field_path.toStdString()});
    }
  }
  for (const std::string& pattern : enumerateSnapshotPatterns(columns)) {
    const QString qpattern = QString::fromStdString(pattern);
    x_combo_->addItem(qpattern);
    y_list_->addItem(qpattern);
  }
  refreshOkState();
}

QString SnapshotGroupDialog::xPattern() const {
  if (x_index_->isChecked()) {
    return {};
  }
  return x_combo_->currentText();
}

QStringList SnapshotGroupDialog::yPatterns() const {
  QStringList out;
  for (const QListWidgetItem* item : y_list_->selectedItems()) {
    out.push_back(item->text());
  }
  return out;
}

QString SnapshotGroupDialog::aliasPrefix() const {
  return alias_edit_->text().trimmed();
}

std::optional<double> SnapshotGroupDialog::yMin() const {
  return auto_min_->isChecked() ? std::nullopt : std::optional<double>{min_spin_->value()};
}

std::optional<double> SnapshotGroupDialog::yMax() const {
  return auto_max_->isChecked() ? std::nullopt : std::optional<double>{max_spin_->value()};
}

void SnapshotGroupDialog::refreshOkState() {
  bool ok = topic_id_ != 0 && !yPatterns().isEmpty();
  if (x_leaf_->isChecked() && x_combo_->currentText().isEmpty()) {
    ok = false;
  }
  if (const auto lo = yMin(), hi = yMax(); lo.has_value() && hi.has_value() && *lo >= *hi) {
    ok = false;
  }
  if (ok_button_ != nullptr) {
    ok_button_->setEnabled(ok);
  }
}

}  // namespace PJ
