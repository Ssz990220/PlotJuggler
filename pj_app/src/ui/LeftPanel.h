#pragma once

#include <QWidget>

namespace Ui {
class LeftPanel;
}

namespace PJ {

// BLUE region: stacked collapsible sections for File / Streaming / Publishers.
// Ported from the `leftMainWindowFrame` subtree of PJ3 mainwindow.ui (lines
// 62-1062) with Custom Series moved to CurveListPanel and the Publishers
// grid kept empty until the plugin-host wiring lands.
//
// Emits high-level user-intent signals — wiring to services is MainWindow's
// responsibility. Collapse state is persisted to QSettings under the same
// keys PJ3 used, so an existing user's preferences carry over.
class LeftPanel : public QWidget {
  Q_OBJECT
 public:
  explicit LeftPanel(QWidget* parent = nullptr);
  ~LeftPanel() override;

 signals:
  void loadDataRequested();
  void reloadDataRequested();
  void loadLayoutRequested();
  void saveLayoutRequested();
  void addPrefixToggled(bool enabled);
  void mergeDataToggled(bool enabled);
  void streamingStartToggled(bool started);
  void streamingPauseToggled(bool paused);
  void streamingBufferChanged(int seconds);
  void streamingSourceChanged(QString source);
  void streamingOptionsRequested();

 public slots:
  void onStylesheetChanged(QString theme);

 private slots:
  void onHideFileFrameClicked();
  void onHideStreamingFrameClicked();
  void onHidePublishersFrameClicked();

 private:
  void loadCollapseStateFromSettings();
  void applyIcons(QString theme);

  Ui::LeftPanel* ui_;
};

}  // namespace PJ
