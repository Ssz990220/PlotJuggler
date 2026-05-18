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
#include "pj_widgets/ChromeMetrics.h"

class QAction;
class QButtonGroup;
class QCloseEvent;
class QMenu;
class QPushButton;
class QStackedWidget;
class QToolButton;

namespace Ui {
class MainWindow;
}

namespace PJ {

class AppSession;
class CurveEditor;
class DiagnosticHistory;
class DockWidget;
class FileLoader;
class PlotDocker;
class PlotWidget;
class QtDiagnosticBridge;
class Theme;
class TitleBar;

// Legend corner placement. Four corner buttons in the right toolbar act
// as an exclusive group: click sets the position, click the active one
// again hides the legend. Stored as int in QSettings ("MainWindow.legendStatus").
enum class LegendStatus {
  kBottomRight = 0,
  kBottomLeft = 1,
  kTopRight = 2,
  kTopLeft = 3,
  kHidden = 4,
};

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

  [[nodiscard]] TitleBar* titleBar() const {
    return title_bar_;
  }

  // Diagnostic sink that fans out to the title-bar bell + popup via the
  // DiagnosticHistory service. Use this from tools / CLI / dev feeds to
  // emit through the same pipeline plugins use.
  [[nodiscard]] DiagnosticSink diagnosticSink() const;

  // Global Chrome metrics for toolbar/panel buttons. Persisted to
  // QSettings (ui/icon_size, ui/icon_padding, ui/layout_padding,
  // ui/layout_spacing) and broadcast via chromeMetricsChanged so each
  // icon-bearing widget can re-render.
  [[nodiscard]] const ChromeMetrics& chromeMetrics() const {
    return chrome_metrics_;
  }

 public slots:
  void setIconSize(int size);
  void setIconPadding(int padding);
  void setLayoutPadding(int padding);
  void setLayoutSpacing(int spacing);

 signals:
  // Fires after qApp's stylesheet is applied; subwidgets refresh
  // palette-tinted icons via their onStylesheetChanged slots.
  void stylesheetChanged(QString theme);

  // Fires when any chrome metric changes. Bundled so consumers always
  // recompute layout from a consistent snapshot:
  //   band height       = (icon_size + icon_padding) + 2 * layout_padding
  //   chrome margins    = layout_padding
  //   chrome / list gap = layout_spacing
  // layout_padding feeds QLayout::setContentsMargins (band grows to
  // absorb it), layout_spacing feeds QLayout::setSpacing and the
  // CurveEditor list-row gap.
  void chromeMetricsChanged(const ChromeMetrics& metrics);

 private slots:
  // Layout file flow: open / save / replay-recent. Persists the chosen
  // file path into the recent-layouts list (cap 5) regardless of whether
  // the on-disk format is meaningful yet.
  void onLoadLayout();
  void onSaveLayout();
  void onLoadRecentLayout(const QString& path);
  void onRebuildRecentLayoutsMenu();

  // Opens the extension marketplace dialog.
  void onOpenMarketplace();

  // Opens the file-load workflow.
  void onLoadDataRequested();

  // Updates playback bounds after a data file has populated datastore and
  // object-store topics.
  void onFileLoaded(const QString& path);

  // Removes selected catalog entries from the curve/object tree.
  void onCatalogTrashRequested(QStringList keys, bool covers_all);

  void onShowPreferencesDialog();

  // Rebuilds the title-bar Extensions popup from the current
  // ExtensionCatalogService snapshot. Lists installed plugins (data
  // sources, message parsers, toolboxes) followed by a separator and a
  // "PlotJuggler Marketplace" entry that opens the marketplace dialog.
  void onRebuildExtensionsMenu();

  void onThemeChanged(const QString& theme);

  // Wires callbacks for a newly created plot tab.
  void onPlotTabAdded(PlotDocker* docker);

  // Routes the focused DockWidget to the right config page and updates
  // the curve-editor binding. Plot-only state changes still go through
  // bindEditorToPlot.
  void onDockFocused(DockWidget* dock);

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

 private:
  // Sets legend position (or hides if `position` already matches the
  // current state — clicking the active corner toggles the legend off).
  // Updates QSettings, refreshes button checked states, and re-applies
  // to every plot.
  void setLegendStatus(LegendStatus position);

  // Pushes the current toolbar toggle state (show_points / legend_status /
  // activate_grid / dots) into one plot, so newly added plots match.
  void applyGlobalToggles(PlotWidget* plot);
  void applyLegendStatus(PlotWidget* plot);
  // Toggles dots overlay on Lines/LinesAndDots curves only; curves in
  // Dots/Sticks/Steps keep their style.
  void applyDots(PlotWidget* plot);

  // Convenience: emit a diagnostic into the session's sink. Source/id
  // are stable string literals; message is a translated QString. The
  // sink fans out to QtDiagnosticBridge → DiagnosticHistory and from
  // there to the bell label + popup.
  void emitDiagnostic(DiagnosticLevel level, const char* source, const char* id, const QString& message);

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

  // Builds the global toolbar column — vertical stack of Chart + Legend
  // icons in a fixed 24-px wide strip that sits left of the local panel.
  // No headers, no flow-layout, always visible regardless of the local
  // panel's toggle state.
  void buildGlobalToolbar();

  // Builds the local panel — Curve Width + Curve Style header bands and
  // their flow-layout icon strips above the CurveEditor. Snap/compact
  // behaviour still applies (hidden by the "Toggle Right Panel" button,
  // headers fold below ~72 px wide).
  void buildLocalToolbar();

  // Apply to every curve of the editor's bound plot. No-op when unbound.
  void applyActivePlotWidth(double width);
  void applyActivePlotStyle(int style);

  // Layout helpers.
  void loadLayoutFromPath(const QString& path);
  void saveLayoutToPath(const QString& path);
  void recordRecentLayout(const QString& path);
  [[nodiscard]] QStringList recentLayouts() const;

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

  // Binds the CurveEditor to `plot` and enables/disables the width and
  // style toolbar buttons accordingly (they no-op without an active plot).
  void bindEditorToPlot(PlotWidget* plot);
  [[nodiscard]] PlotWidget* firstPlotOfActiveTab() const;

 protected:
  // Persists main-window settings before close.
  void closeEvent(QCloseEvent* event) override;

  // Frameless-window edge resize: catches mouse events on ourselves or
  // any descendant widget, updates the cursor near edges, and starts a
  // system-resize on press.
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  Ui::MainWindow* ui_;
  QtDiagnosticBridge* diagnostic_bridge_ = nullptr;
  DiagnosticHistory* diagnostic_history_ = nullptr;
  QAction* undo_action_ = nullptr;
  QAction* redo_action_ = nullptr;
  std::unique_ptr<AppSession> session_;
  std::unique_ptr<FileLoader> file_loader_;
  std::unique_ptr<Theme> theme_;
  TitleBar* title_bar_ = nullptr;
  QMenu* recent_layouts_menu_ = nullptr;
  // Local-panel header bands (grey "Curve Width" / "Curve Style" labels).
  // Kept as members so build_section's findChild lookups for the
  // exclusive radio buttons have a stable parent to query.
  QWidget* curve_width_header_ = nullptr;
  QWidget* curve_style_header_ = nullptr;
  // Curve-style + Curve-width buttons each form an exclusive radio-style
  // group (one checked at a time; defaults: "Lines" / 1.0 px). The group
  // owns no widgets — it just enforces the mutual-exclusion semantics on
  // the existing toolbar buttons.
  QButtonGroup* style_button_group_ = nullptr;
  QButtonGroup* width_button_group_ = nullptr;

  // Stored so applyIcons() can re-tint them on theme change.
  QAction* action_load_layout_ = nullptr;
  QAction* action_save_layout_ = nullptr;
  // The Exit menu item is a QWidgetAction-wrapped QPushButton so it
  // can carry the `special` dynamic property used by the menu QSS
  // to give it a gradient hover. Stored here so applyIcons() can
  // re-tint its icon on theme change.
  QPushButton* exit_menu_button_ = nullptr;
  std::deque<QByteArray> undo_states_;
  std::deque<QByteArray> redo_states_;
  QElapsedTimer undo_timer_;
  bool applying_state_ = false;
  // Lives inside localToolbarWidget; visibility piggybacks on the
  // right-panel toggle in the tab strip.
  CurveEditor* curve_editor_ = nullptr;

  // Right-sidepanel content swap: the stack hosts a plot-config page
  // (Curve Width / Style strips + CurveEditor) and per-family pages
  // for 2D and 3D scenes. onDockFocused() picks the active page from
  // the focused DockWidget's content type.
  QStackedWidget* right_panel_stack_ = nullptr;
  QWidget* plot_config_page_ = nullptr;
  QWidget* scene2d_config_page_ = nullptr;
  QWidget* scene3d_config_page_ = nullptr;
  // Shown when the focused dock holds the 3-icon
  // VisualizationPlaceholderWidget — nothing to configure yet.
  QWidget* empty_dock_page_ = nullptr;

  // Global-column "Chart" icons — built in buildGlobalToolbar(), so
  // stored as member pointers (no ui_-> accessor).
  QToolButton* button_link_ = nullptr;
  QToolButton* button_show_point_ = nullptr;
  QToolButton* button_grid_ = nullptr;
  QToolButton* button_dots_ = nullptr;
  // Global-column "Legend" button — single icon that combines a corner
  // picker (left-click) with a show/hide toggle (right-click). Checked
  // while the legend is shown at one of the four corners; unchecked
  // when hidden. The icon always reflects the "current position": the
  // active corner while checked, or the saved corner that will be
  // restored on the next show while unchecked.
  QToolButton* button_legend_ = nullptr;
  // Saved corner used while the legend is hidden. Updated on every
  // visit to a corner so right-click → show restores the user's last
  // position rather than always jumping back to a fixed default.
  LegendStatus previous_legend_corner_ = LegendStatus::kTopRight;

  // Global-column view toggles. Persisted to QSettings; XML
  // round-tripped; applied to every plot via forEachPlot.
  LegendStatus legend_status_ = LegendStatus::kTopRight;
  bool show_points_ = false;
  bool activate_grid_ = false;
  bool dots_ = false;

  // Loaded from QSettings before any child widget is built so the first
  // applyIcons() of each widget already uses the saved metrics.
  ChromeMetrics chrome_metrics_;
};

}  // namespace PJ
