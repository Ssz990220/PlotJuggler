#pragma once

#include <QDateTime>
#include <QDomDocument>
#include <QElapsedTimer>
#include <QList>
#include <QMainWindow>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <deque>
#include <functional>
#include <memory>

#include "pj_base/diagnostic_sink.hpp"

class QAction;
class QCloseEvent;
class QPushButton;

namespace Ui {
class MainWindow;
}

namespace PJ {

class AppSession;
class DockWidget;
class FileLoader;
class PlotDocker;
class PlotWidget;
class QtDiagnosticBridge;
class Theme;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  // Creates the main window using the default extension directory.
  explicit MainWindow(QWidget* parent = nullptr);

  // Creates the main window using an explicit extension directory.
  explicit MainWindow(QString extensions_dir, QWidget* parent = nullptr);

  // Releases UI resources and the application session.
  ~MainWindow() override;

  // Populates the session with generated data for smoke testing.
  [[nodiscard]] bool populateTestData();

 signals:
  // Fires after qApp's stylesheet is applied; subwidgets refresh
  // palette-tinted icons via their onStylesheetChanged slots.
  void stylesheetChanged(QString theme);

 private slots:
  // Opens the extension marketplace dialog.
  void onOpenMarketplace();

  // Opens the file-load workflow.
  void onLoadDataRequested();

  // Opens the recent diagnostics dialog.
  void onShowDiagnosticsDialog();

  void onShowPreferencesDialog();

  void onThemeChanged(const QString& theme);

  // Wires callbacks for a newly created plot tab.
  void onPlotTabAdded(PlotDocker* docker);

  // Wires callbacks for a newly created plot widget.
  void onPlotAdded(PlotWidget* plot);

  // Mirrors X zoom to linked plots.
  void onPlotZoomChanged(PlotWidget* modified, QRectF rect);

  // Updates playback time from a plot tracker move.
  void onTrackerMovedFromWidget(QPointF point);

  // Records a user-visible plot layout change.
  void onUndoableChange();

  // Restores the previous layout snapshot.
  void onUndo();

  // Restores the next layout snapshot after undo.
  void onRedo();

  // Saves the current layout XML to disk.
  void onSaveLayout();

  // Loads layout XML from disk.
  void onLoadLayout();

 private:
  // Records and displays one diagnostic from the shared bridge.
  void onDiagnosticReported(int level, QString source, QString id, QString message);

  // Updates visibility and text for diagnostics UI affordances.
  void updateDiagnosticsButton();

  // Refreshes the streaming source selector from loaded plugins.
  void refreshStreamingCombo();

  // Wires callbacks for plots already present after UI setup.
  void wireExistingPlots();

  // Applies operation to each plot docker.
  void forEachDocker(const std::function<void(PlotDocker*)>& operation);

  // Applies operation to each dock widget.
  void forEachDock(const std::function<void(DockWidget*)>& operation);

  // Applies operation to each plot widget.
  void forEachPlot(const std::function<void(PlotWidget*)>& operation);

  // Icons not owned by a subwidget with its own onStylesheetChanged.
  void applyIcons(QString theme);

  // Serializes the current app layout state.
  [[nodiscard]] QDomDocument xmlSaveState() const;

  // Loads a previously serialized app layout state.
  bool xmlLoadState(const QDomDocument& state_document);

  // Initializes the undo stack with the post-construction state.
  void pushInitialUndoState();

  // Adds or replaces the newest undo snapshot.
  void pushUndoState(bool force_new_state = false);

  // Updates enabled state for undo / redo actions.
  void updateUndoRedoActions();

 protected:
  // Persists main-window settings before close.
  void closeEvent(QCloseEvent* event) override;

 private:
  struct UiDiagnostic {
    DiagnosticLevel level;
    QString source;
    QString id;
    QString message;
    QDateTime timestamp;
  };

  Ui::MainWindow* ui_;
  QtDiagnosticBridge* diagnostic_bridge_ = nullptr;
  QList<UiDiagnostic> diagnostics_;
  QAction* diagnostics_action_ = nullptr;
  QAction* undo_action_ = nullptr;
  QAction* redo_action_ = nullptr;
  QAction* save_layout_action_ = nullptr;
  QAction* load_layout_action_ = nullptr;
  QPushButton* diagnostics_button_ = nullptr;
  std::unique_ptr<AppSession> session_;
  std::unique_ptr<FileLoader> file_loader_;
  std::unique_ptr<Theme> theme_;
  std::deque<QDomDocument> undo_states_;
  std::deque<QDomDocument> redo_states_;
  QElapsedTimer undo_timer_;
  bool applying_state_ = false;
};

}  // namespace PJ
