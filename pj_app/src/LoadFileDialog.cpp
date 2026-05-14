#include "LoadFileDialog.h"

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QModelIndex>
#include <QPushButton>
#include <QStringList>
#include <QTreeView>
#include <QVBoxLayout>

namespace PJ {

namespace {

// Convert a Qt-style filter string into the raw glob list
// QFileSystemModel wants. The catalog produces a multi-section filter
// like
//   "All supported files (*.csv *.json);;CSV (*.csv);;JSON (*.json);;"
// We take the FIRST section only (the union of all extensions). Using
// lastIndexOf(')') would span across all sections and produce garbled
// globs, which silently hide every file.
QStringList parseGlobs(const QString& filter) {
  const int paren_open = filter.indexOf('(');
  if (paren_open < 0) {
    return {};
  }
  const int paren_close = filter.indexOf(')', paren_open);
  if (paren_close <= paren_open) {
    return {};
  }
  return filter.mid(paren_open + 1, paren_close - paren_open - 1).split(' ', Qt::SkipEmptyParts);
}

}  // namespace

LoadFileDialog::LoadFileDialog(QWidget* parent, const QString& start_dir, const QString& name_filter, Options options)
    : Dialog(parent) {
  setDialogTitle(tr("Load Data"));
  resize(720, 520);

  auto* body = new QWidget;
  auto* main_layout = new QVBoxLayout(body);
  main_layout->setContentsMargins(12, 12, 12, 12);

  model_ = new QFileSystemModel(this);
  model_->setRootPath(QDir::rootPath());
  const QStringList globs = parseGlobs(name_filter);
  if (!globs.isEmpty()) {
    model_->setNameFilters(globs);
    model_->setNameFilterDisables(false);  // hide non-matching files entirely
  }

  tree_ = new QTreeView(body);
  tree_->setModel(model_);
  tree_->setSelectionMode(QAbstractItemView::SingleSelection);
  tree_->setRootIsDecorated(true);
  tree_->setAnimated(false);
  tree_->setSortingEnabled(true);
  tree_->sortByColumn(0, Qt::AscendingOrder);
  tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  for (int col = 1; col < model_->columnCount(); ++col) {
    tree_->header()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
  }

  const QString home = QDir::homePath();
  const QString resolved_start = start_dir.isEmpty() ? home : start_dir;
  const QModelIndex start_index = model_->index(resolved_start);
  if (start_index.isValid()) {
    tree_->setCurrentIndex(start_index);
    tree_->scrollTo(start_index, QAbstractItemView::PositionAtCenter);
    tree_->expand(start_index);
  }

  main_layout->addWidget(tree_, 1);

  auto* path_layout = new QHBoxLayout;
  path_layout->addWidget(new QLabel(tr("File:"), body));
  path_edit_ = new QLineEdit(body);
  path_edit_->setReadOnly(true);
  path_layout->addWidget(path_edit_);
  main_layout->addLayout(path_layout);

  if (options == Options::ShowDataOptions) {
    auto* options_layout = new QHBoxLayout;
    add_prefix_ = new QCheckBox(tr("Add Prefix"), body);
    merge_metadata_ = new QCheckBox(tr("Merge Data"), body);
    options_layout->addWidget(add_prefix_);
    options_layout->addWidget(merge_metadata_);
    options_layout->addStretch();
    main_layout->addLayout(options_layout);
  }

  auto* button_layout = new QHBoxLayout;
  button_layout->addStretch();
  auto* cancel_button = new QPushButton(tr("Cancel"), body);
  cancel_button->setProperty("destructive", true);
  ok_button_ = new QPushButton(tr("OK"), body);
  ok_button_->setEnabled(false);
  button_layout->addWidget(cancel_button);
  button_layout->addWidget(ok_button_);
  connect(ok_button_, &QPushButton::clicked, this, &QDialog::accept);
  connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
  main_layout->addLayout(button_layout);

  contentLayout()->addWidget(body);

  connect(
      tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
      [this](const QModelIndex& current, const QModelIndex&) {
        (void)current;
        onSelectionChanged();
      });
  connect(tree_, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
    if (!index.isValid()) {
      return;
    }
    const QFileInfo info(model_->filePath(index));
    if (info.isFile()) {
      path_edit_->setText(info.absoluteFilePath());
      accept();
    } else {
      tree_->setExpanded(index, !tree_->isExpanded(index));
    }
  });
  onSelectionChanged();
}

LoadFileDialog::~LoadFileDialog() = default;

QString LoadFileDialog::selectedPath() const {
  return path_edit_->text();
}

bool LoadFileDialog::addPrefix() const {
  return add_prefix_ != nullptr && add_prefix_->isChecked();
}

bool LoadFileDialog::mergeMetadata() const {
  return merge_metadata_ != nullptr && merge_metadata_->isChecked();
}

void LoadFileDialog::onSelectionChanged() {
  const QModelIndex current = tree_->currentIndex();
  if (!current.isValid()) {
    path_edit_->clear();
    ok_button_->setEnabled(false);
    return;
  }
  const QFileInfo info(model_->filePath(current));
  if (info.isFile()) {
    path_edit_->setText(info.absoluteFilePath());
    ok_button_->setEnabled(true);
  } else {
    path_edit_->clear();
    ok_button_->setEnabled(false);
  }
}

}  // namespace PJ
