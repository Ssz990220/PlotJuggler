#pragma once

#include <QWidget>

#include "pj_widgets/ChromeMetrics.h"

namespace Ui {
class LeftPanel;
}

namespace PJ {

// "Sources" section: file load + streaming source. Emits high-level
// user-intent signals — MainWindow wires them to services.
class LeftPanel : public QWidget {
  Q_OBJECT
 public:
  explicit LeftPanel(QWidget* parent = nullptr);
  ~LeftPanel() override;

 signals:
  void loadDataRequested();
  void reloadDataRequested();
  // Emitted when the user picks a path from the recent-files menu in
  // the Input header. MainWindow connects this to FileLoader::loadFile.
  void recentFileSelected(QString path);
  void streamingStartToggled(bool started);
  void streamingSourceChanged(QString source);
  // Buffer length (seconds) for the streaming source. Persisted to
  // QSettings; emitted when the user adjusts the inline scrubber.
  void streamingBufferChanged(int seconds);

 public slots:
  void onStylesheetChanged(QString theme);
  // Rebinds Chrome metrics from MainWindow. Re-runs applyIcons() so
  // the Sources band, page rows, and streaming row absorb new icon
  // metrics, layout padding and the spacing between items.
  void onChromeMetricsChanged(const ChromeMetrics& metrics);
  // Repopulates the streaming combo. Preserves the current selection if the
  // previously-selected name is still present.
  void setStreamingSources(const QStringList& names);
  void setReloadEnabled(bool enabled);
  void setRecentEnabled(bool enabled);

 private:
  void applyIcons(QString theme);

  Ui::LeftPanel* ui_;
  // Chrome metrics from MainWindow::chromeMetricsChanged.
  ChromeMetrics chrome_metrics_;
};

}  // namespace PJ
