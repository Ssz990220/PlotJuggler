#include "pj_widgets/FileDialog.h"

#include <QDialog>
#include <QIcon>
#include <QLayout>
#include <QList>
#include <QSize>
#include <QString>
#include <QToolButton>

#include "pj_widgets/SvgUtil.h"

namespace PJ {

namespace {

// objectNames of the QFileDialog toolbar buttons we re-skin and re-size.
constexpr const char* kToolbarButtons[] = {
    "backButton", "forwardButton", "toParentButton", "newFolderButton", "listModeButton", "detailModeButton",
};

// Re-skin a QFileDialog toolbar button identified by objectName with one
// of the app's themed SVG icons. Silent no-op when the button is missing
// (defensive against Qt internal renames between versions).
void ApplySvgIcon(QFileDialog* dialog, const char* object_name, const QString& svg_resource) {
  if (auto* button = dialog->findChild<QToolButton*>(QString::fromLatin1(object_name))) {
    button->setIcon(QIcon(LoadSvg(svg_resource, currentTheme())));
  }
}

}  // namespace

FileDialog::FileDialog(QWidget* parent) : Dialog(parent) {
  setDialogTitle(tr("Open File"));
  resize(820, 560);

  // Build the inner picker as a plain child widget — Qt::Widget strips
  // the default Qt::Dialog window flags so it doesn't try to be its own
  // top-level window. DontUseNativeDialog forces the Qt-drawn widget
  // tree, which is what our stylesheet targets.
  inner_ = new QFileDialog(contentWidget());
  inner_->setOption(QFileDialog::DontUseNativeDialog);
  inner_->setWindowFlags(Qt::Widget);
  inner_->setSizeGripEnabled(false);

  contentLayout()->addWidget(inner_);

  // Swap Qt's stock SP_FileDialog* icons (native theme glyphs that clash
  // with our flat chrome) for our keyboard_arrow_* chevrons. LoadSvg
  // re-tints for the active theme automatically.
  ApplySvgIcon(inner_, "backButton", ":/resources/svg/keyboard_arrow_left_dark.svg");
  ApplySvgIcon(inner_, "forwardButton", ":/resources/svg/keyboard_arrow_right_dark.svg");
  ApplySvgIcon(inner_, "toParentButton", ":/resources/svg/keyboard_arrow_up_dark.svg");
  ApplySvgIcon(inner_, "newFolderButton", ":/resources/svg/create_new_folder.svg");
  // QFileDialog's "list mode" is the columns-of-icons display, "detail
  // mode" is the table with name/size/type/date columns — match Material's
  // visual convention: Grid View for the icon-grid mode, View List for
  // the row-with-metadata mode.
  ApplySvgIcon(inner_, "listModeButton", ":/resources/svg/grid_view.svg");
  ApplySvgIcon(inner_, "detailModeButton", ":/resources/svg/view_list.svg");

  // Prime icon size from ChromeMetrics defaults. Callers (or the
  // templated static helpers) override with the live MainWindow values
  // and may wire chromeMetricsChanged to stay in step.
  onChromeMetricsChanged(chrome_metrics_);

  // The inner QDialogButtonBox accepts/rejects route the outer Dialog,
  // so exec() on the outer returns the expected QDialog::DialogCode.
  connect(inner_, &QFileDialog::accepted, this, &QDialog::accept);
  connect(inner_, &QFileDialog::rejected, this, &QDialog::reject);
}

void FileDialog::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  chrome_metrics_ = metrics;
  // Mirror CurveListPanel: the icon gets icon_size, the button's outer
  // extent gets icon_size + icon_padding. Setting only setIconSize leaves
  // the button at its default ~16px size and the larger icon clips.
  const QSize icon_sz(metrics.icon_size, metrics.icon_size);
  const int button_extent = metrics.icon_size + metrics.icon_padding;
  for (const char* name : kToolbarButtons) {
    if (auto* button = inner_->findChild<QToolButton*>(QString::fromLatin1(name))) {
      button->setIconSize(icon_sz);
      button->setMinimumSize(button_extent, button_extent);
      button->setMaximumSize(button_extent, button_extent);
    }
  }
}

FileDialog::~FileDialog() = default;

void FileDialog::setAcceptMode(QFileDialog::AcceptMode mode) {
  inner_->setAcceptMode(mode);
}

void FileDialog::setFileMode(QFileDialog::FileMode mode) {
  inner_->setFileMode(mode);
}

void FileDialog::setNameFilter(const QString& filter) {
  inner_->setNameFilter(filter);
}

void FileDialog::setDirectory(const QString& directory) {
  inner_->setDirectory(directory);
}

void FileDialog::setDefaultSuffix(const QString& suffix) {
  inner_->setDefaultSuffix(suffix);
}

void FileDialog::selectFile(const QString& filename) {
  inner_->selectFile(filename);
}

QStringList FileDialog::selectedFiles() const {
  return inner_->selectedFiles();
}

QString FileDialog::selectedFile() const {
  const QStringList files = inner_->selectedFiles();
  return files.isEmpty() ? QString{} : files.front();
}

QString FileDialog::getOpenFileName(
    QWidget* parent, const QString& caption, const QString& dir, const QString& filter) {
  FileDialog dlg(parent);
  if (!caption.isEmpty()) {
    dlg.setDialogTitle(caption);
  }
  dlg.setAcceptMode(QFileDialog::AcceptOpen);
  dlg.setFileMode(QFileDialog::ExistingFile);
  if (!dir.isEmpty()) {
    dlg.setDirectory(dir);
  }
  if (!filter.isEmpty()) {
    dlg.setNameFilter(filter);
  }
  if (dlg.exec() != QDialog::Accepted) {
    return {};
  }
  return dlg.selectedFile();
}

QString FileDialog::getSaveFileName(
    QWidget* parent, const QString& caption, const QString& dir, const QString& filter, const QString& default_suffix) {
  FileDialog dlg(parent);
  if (!caption.isEmpty()) {
    dlg.setDialogTitle(caption);
  }
  dlg.setAcceptMode(QFileDialog::AcceptSave);
  dlg.setFileMode(QFileDialog::AnyFile);
  if (!dir.isEmpty()) {
    dlg.setDirectory(dir);
  }
  if (!filter.isEmpty()) {
    dlg.setNameFilter(filter);
  }
  if (!default_suffix.isEmpty()) {
    dlg.setDefaultSuffix(default_suffix);
  }
  if (dlg.exec() != QDialog::Accepted) {
    return {};
  }
  return dlg.selectedFile();
}

}  // namespace PJ
