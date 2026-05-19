#pragma once

#include <QFileDialog>
#include <QObject>
#include <QString>
#include <QStringList>

#include "pj_widgets/ChromeMetrics.h"
#include "pj_widgets/Dialog.h"

namespace PJ {

// Frameless wrapper around QFileDialog: embeds a non-native
// QFileDialog inside PJ::Dialog's chrome so file pickers match the
// rest of the app (custom title bar, drag-to-move, edge resize) instead
// of the native GTK frame. Most call sites should use the static
// getOpenFileName / getSaveFileName helpers below.
class FileDialog : public Dialog {
  Q_OBJECT
 public:
  explicit FileDialog(QWidget* parent = nullptr);
  ~FileDialog() override;

  // Forwarders to the embedded QFileDialog. Add more as callers need.
  void setAcceptMode(QFileDialog::AcceptMode mode);
  void setFileMode(QFileDialog::FileMode mode);
  void setNameFilter(const QString& filter);
  void setDirectory(const QString& directory);
  void setDefaultSuffix(const QString& suffix);
  void selectFile(const QString& filename);

  [[nodiscard]] QStringList selectedFiles() const;
  // First entry of selectedFiles(), or an empty string when nothing selected.
  [[nodiscard]] QString selectedFile() const;

  // Drop-in replacements for QFileDialog::getOpenFileName / getSaveFileName.
  static QString getOpenFileName(QWidget* parent, const QString& caption, const QString& dir, const QString& filter);
  static QString getSaveFileName(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter,
      const QString& default_suffix = QString());

  // Templated overloads that wire chrome metrics from a signal source.
  // The source must expose `chromeMetrics()` returning a (const?)
  // ChromeMetrics(&) and a Qt signal `chromeMetricsChanged(const
  // ChromeMetrics&)`. In practice that's MainWindow; the template lets
  // pj_widgets stay decoupled from pj_app types.
  template <typename SignalSourceT>
  static QString getOpenFileName(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter,
      SignalSourceT* metrics_source);
  template <typename SignalSourceT>
  static QString getSaveFileName(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter, const QString& default_suffix,
      SignalSourceT* metrics_source);

 public slots:
  // Apply chrome metrics (icon size) to the embedded QFileDialog. Connect
  // MainWindow::chromeMetricsChanged to this to keep the toolbar icons
  // in step with the rest of the app — the templated static helpers do
  // both the prime + connect for you.
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 private:
  QFileDialog* inner_;
  ChromeMetrics chrome_metrics_;
};

template <typename SignalSourceT>
QString FileDialog::getOpenFileName(
    QWidget* parent, const QString& caption, const QString& dir, const QString& filter, SignalSourceT* metrics_source) {
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
  if (metrics_source != nullptr) {
    dlg.onChromeMetricsChanged(metrics_source->chromeMetrics());
    QObject::connect(metrics_source, &SignalSourceT::chromeMetricsChanged, &dlg, &FileDialog::onChromeMetricsChanged);
  }
  if (dlg.exec() != QDialog::Accepted) {
    return {};
  }
  return dlg.selectedFile();
}

template <typename SignalSourceT>
QString FileDialog::getSaveFileName(
    QWidget* parent, const QString& caption, const QString& dir, const QString& filter, const QString& default_suffix,
    SignalSourceT* metrics_source) {
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
  if (metrics_source != nullptr) {
    dlg.onChromeMetricsChanged(metrics_source->chromeMetrics());
    QObject::connect(metrics_source, &SignalSourceT::chromeMetricsChanged, &dlg, &FileDialog::onChromeMetricsChanged);
  }
  if (dlg.exec() != QDialog::Accepted) {
    return {};
  }
  return dlg.selectedFile();
}

}  // namespace PJ
