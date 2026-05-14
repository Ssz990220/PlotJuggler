#include "pj_widgets/SaveFileDialog.h"

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

// Same first-section parser used by LoadFileDialog. Multi-section
// filters like "PJ4 Layout (*.pjl4);;..." are accepted; only the first
// section's globs are honoured.
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

SaveFileDialog::SaveFileDialog(
    QWidget* parent, const QString& start_dir, const QString& name_filter, const QString& default_extension)
    : Dialog(parent), default_extension_(default_extension) {
  setDialogTitle(tr("Save File"));
  resize(720, 520);

  auto* body = new QWidget;
  auto* main_layout = new QVBoxLayout(body);
  main_layout->setContentsMargins(12, 12, 12, 12);

  model_ = new QFileSystemModel(this);
  model_->setRootPath(QDir::rootPath());
  const QStringList globs = parseGlobs(name_filter);
  if (!globs.isEmpty()) {
    model_->setNameFilters(globs);
    // For save we want users to see existing files of the same type so
    // they can pick one to overwrite. Setting filter-disables to true
    // greys non-matching files instead of hiding them — matches the
    // load picker's hide behaviour while keeping directories navigable.
    model_->setNameFilterDisables(false);
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

  auto* name_layout = new QHBoxLayout;
  name_layout->addWidget(new QLabel(tr("File name:"), body));
  filename_edit_ = new QLineEdit(body);
  filename_edit_->setPlaceholderText(tr("untitled%1").arg(default_extension_));
  name_layout->addWidget(filename_edit_);
  main_layout->addLayout(name_layout);

  auto* button_layout = new QHBoxLayout;
  button_layout->addStretch();
  auto* cancel_button = new QPushButton(tr("Cancel"), body);
  cancel_button->setProperty("destructive", true);
  save_button_ = new QPushButton(tr("Save"), body);
  save_button_->setEnabled(false);
  button_layout->addWidget(cancel_button);
  button_layout->addWidget(save_button_);
  connect(save_button_, &QPushButton::clicked, this, &SaveFileDialog::onAccept);
  connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
  main_layout->addLayout(button_layout);

  contentLayout()->addWidget(body);

  connect(
      tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
      [this](const QModelIndex& current, const QModelIndex&) {
        (void)current;
        onSelectionChanged();
      });
  // Single-click on a file pre-fills the name edit (overwrite affordance).
  connect(tree_, &QTreeView::clicked, this, [this](const QModelIndex& index) {
    if (!index.isValid()) {
      return;
    }
    const QFileInfo info(model_->filePath(index));
    if (info.isFile()) {
      filename_edit_->setText(info.fileName());
    }
  });
  connect(filename_edit_, &QLineEdit::textChanged, this, &SaveFileDialog::onFilenameChanged);
  onSelectionChanged();
}

SaveFileDialog::~SaveFileDialog() = default;

QString SaveFileDialog::selectedPath() const {
  return resolved_path_;
}

void SaveFileDialog::onSelectionChanged() {
  // Save dialog only needs the *directory* from the tree; the filename
  // comes from the line edit. Enable Save when both are present.
  onFilenameChanged();
}

void SaveFileDialog::onFilenameChanged() {
  const QModelIndex current = tree_->currentIndex();
  const bool has_dir = current.isValid();
  const bool has_name = !filename_edit_->text().trimmed().isEmpty();
  save_button_->setEnabled(has_dir && has_name);
}

void SaveFileDialog::onAccept() {
  const QModelIndex current = tree_->currentIndex();
  if (!current.isValid()) {
    return;
  }
  const QFileInfo selected(model_->filePath(current));
  // If the selected tree item is a file, save into its parent directory.
  // Otherwise treat it as a directory.
  const QString dir = selected.isFile() ? selected.absolutePath() : selected.absoluteFilePath();

  QString name = filename_edit_->text().trimmed();
  if (name.isEmpty()) {
    return;
  }
  if (!default_extension_.isEmpty() && !name.endsWith(default_extension_, Qt::CaseInsensitive)) {
    name.append(default_extension_);
  }
  resolved_path_ = QDir(dir).filePath(name);
  accept();
}

}  // namespace PJ
