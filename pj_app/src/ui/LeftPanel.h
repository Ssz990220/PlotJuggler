#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
class QFrame;
class QPushButton;
QT_END_NAMESPACE

namespace Ui {
class LeftPanel;
}

namespace PJ {

// Stacked collapsible sections for File / Streaming / Publishers. Emits
// high-level user-intent signals — MainWindow wires them to services.
// Collapse state is persisted to QSettings under the PJ3-compatible keys
// ("MainWindow.hiddenFileFrame" etc.).
class LeftPanel : public QWidget {
  Q_OBJECT
 public:
  explicit LeftPanel(QWidget* parent = nullptr);
  ~LeftPanel() override;

 signals:
  void loadDataRequested();
  void reloadDataRequested();
  // Carries the popup anchor (button's bottom-left in global coords).
  void recentDataRequested(QPoint global_pos);
  void loadLayoutRequested();
  void saveLayoutRequested();
  // Carries the popup anchor (button's bottom-left in global coords).
  void recentLayoutRequested(QPoint global_pos);
  void addPrefixToggled(bool enabled);
  void mergeDataToggled(bool enabled);
  void streamingStartToggled(bool started);
  void streamingPauseToggled(bool paused);
  void streamingBufferChanged(int seconds);
  void streamingSourceChanged(QString source);
  void streamingOptionsRequested();

 public slots:
  void onStylesheetChanged(QString theme);
  // Repopulates the streaming combo. Preserves the current selection if the
  // previously-selected name is still present.
  void setStreamingSources(const QStringList& names);
  void setReloadEnabled(bool enabled);
  void setRecentEnabled(bool enabled);
  void setRecentLayoutEnabled(bool enabled);

 private:
  void toggleSection(QFrame* frame, QPushButton* button, const char* settings_key);
  void loadCollapseStateFromSettings();
  void applyIcons(QString theme);

  Ui::LeftPanel* ui_;
};

}  // namespace PJ
