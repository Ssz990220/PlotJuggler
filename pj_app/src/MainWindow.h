#pragma once

#include <QByteArray>
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
class QMenu;
class QPushButton;

namespace Ui {
class MainWindow;
}

namespace PJ {

class AppSession;
class CurveEditor;
class DockWidget;
class FileLoader;
class PlotDocker;
class PlotWidget;
class QtDiagnosticBridge;
class RecentFilesMenu;
class Theme;

// Three-state cycle of the Legend toggle. Click cycles RIGHT -> LEFT ->
// HIDDEN -> RIGHT, mirroring PJ3 buttonLegend behaviour. Stored as int in
// QSettings ("MainWindow.legendStatus").
enum class LegendStatus { kLeft = 0, kRight = 1, kHidden = 2 };

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
  void onOpenMarketplace();
  void onLoadDataRequested();
  void onReloadDataRequested();
  void onRecentDataRequested(QPoint global_pos);
  void onFileLoaded(const QString& path);
  void onTrashRequested(const QStringList& names, bool covers_all);
  void onCatalogCurveRemoved(const QString& name);
  void onCatalogCleared();
  void onShowDiagnosticsDialog();
  void onShowPreferencesDialog();
  void onThemeChanged(const QString& theme);
  void onPlotTabAdded(PlotDocker* docker);
  void onPlotAdded(PlotWidget* plot);
  // Mirrors X zoom to linked plots.
  void onPlotZoomChanged(PlotWidget* modified, QRectF rect);
  void onTrackerMovedFromWidget(QPointF point);
  void onUndoableChange();
  void onUndo();
  void onRedo();
  void onSaveLayout();
  void onLoadLayout();
  void onRecentLayoutRequested(QPoint global_pos);
  // Cycles legend_status_ to the next state and re-applies it to every plot.
  void onLegendButtonClicked();

 private:
  // Pushes the current toolbar toggle state (show_points / legend_status /
  // activate_grid / dots) into one plot, so newly added plots match.
  void applyGlobalToggles(PlotWidget* plot);
  void applyLegendStatus(PlotWidget* plot);

  void onDiagnosticReported(int level, QString source, QString id, QString message);
  void updateDiagnosticsButton();
  void refreshStreamingCombo();

  // Returns false and shows a QMessageBox warning on I/O or parse failure.
  bool saveLayoutToFile(const QString& path);
  bool loadLayoutFromFile(const QString& path);

  void wireExistingPlots();
  void forEachDocker(const std::function<void(PlotDocker*)>& operation);
  void forEachDock(const std::function<void(DockWidget*)>& operation);
  void forEachPlot(const std::function<void(PlotWidget*)>& operation);

  // Icons not owned by a subwidget with its own onStylesheetChanged.
  void applyIcons(QString theme);

  [[nodiscard]] QDomDocument xmlSaveState() const;
  bool xmlLoadState(const QDomDocument& state_document);

  void pushInitialUndoState();
  void pushUndoState(bool force_new_state = false);
  void updateUndoRedoActions();

  void showCurveEditor();
  void hideCurveEditor();
  // Re-binds the editor to the first dock of the current tab.
  void bindEditorToActivePlot();

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
  std::deque<QByteArray> undo_states_;
  std::deque<QByteArray> redo_states_;
  QElapsedTimer undo_timer_;
  bool applying_state_ = false;
  CurveEditor* curve_editor_ = nullptr;
  LegendStatus legend_status_ = LegendStatus::kLeft;

  // Paths from the last successful load batch. Session-only (not persisted),
  // matching PJ3 _loaded_datafiles_previous semantics.
  QStringList last_loaded_data_files_;
  RecentFilesMenu* recent_data_menu_ = nullptr;
  RecentFilesMenu* recent_layout_menu_ = nullptr;
};

}  // namespace PJ
