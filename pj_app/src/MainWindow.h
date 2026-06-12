#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QDateTime>
#include <QDir>
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
#include <optional>
#include <utility>
#include <vector>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/types.hpp"
#include "pj_plotting/CurveTracker.h"
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

namespace pj::scene3d {
class TransformService;
}  // namespace pj::scene3d

namespace PJ {

class AppSession;
class CurveEditor;
class DiagnosticHistory;
class DockWidget;
class FileLoader;
class IDataWidget;
class PlotDocker;
class PlotWidget;
class QtDiagnosticBridge;
class StreamingSourceManager;
class RecentFilesMenu;
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

  // Presents the embedded external-process view in the central area (via
  // presentPanel) and restores the chart when the session ends. Idempotent:
  // a no-op if a panel is already presented.
  void openEmbeddedConsole();

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

  // Reloads the remembered data source with its recorded plugin config.
  void onReloadDataRequested();

  // Updates playback bounds after a data file has populated datastore and
  // object-store topics.
  void onFileLoaded(
      const QString& path, const QString& prefix, const QString& plugin_id, const QString& plugin_config_json);

  // Removes selected catalog entries from the curve/object tree.
  void onCatalogTrashRequested(QStringList keys, bool covers_all);

  // Removes one whole dataset (right-click "Remove dataset", post-confirmation):
  // tombstones its scalars and evicts its media. Widget sync is signal-driven.
  void onRemoveDatasetRequested(DatasetId dataset_id);

  void onShowPreferencesDialog();

  // Opens the modal About box (Help ▸ About PlotJuggler…).
  void onShowAboutDialog();

  // Rebuilds the Help ▸ Installed Extensions submenu from the current
  // ExtensionCatalogService snapshot. Informational only (disabled
  // entries): data sources, message parsers, toolboxes. Managing
  // extensions happens in the Marketplace (File menu).
  void onRebuildExtensionsMenu();

  // Rebuilds the title-bar Toolbox menu from the current toolbox
  // catalog. Lists only the launchable, *non-cloud* toolboxes (cloud
  // toolboxes live in the Sources panel instead); each entry launches
  // its toolbox via launchToolbox(). Built lazily on aboutToShow so it
  // tracks whatever the catalog currently has loaded.
  void onRebuildToolboxMenu();

  // Launches a toolbox by id: builds a ToolboxRuntimeHost, binds the
  // toolbox, hosts its dialog in a PanelEngine, and presents it in the
  // chart area. Close tears it all down. Shared by the Toolbox menu and
  // LeftPanel::cloudToolboxRequested ("cloud" is just a manifest tag).
  void launchToolbox(const QString& plugin_id);

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

  // Seconds-domain range spanning all data in the active streaming dataset, or
  // nullopt when no streaming session is active or it holds no data yet.
  // Shared by the live-ingest range update and the drop-to-view seeding.
  std::optional<Range<double>> computeActiveStreamingRangeSec() const;

  // Seeds the streaming playback slider over the active streamed window, playhead
  // at the live edge, and marks the session seeded so live ingests keep the range
  // fresh. No-op when no streaming dataset is active. Called from BOTH the 2D and
  // 3D drop-to-view branches: either family alone must establish the timeline, or
  // a 3D-only stream never seeds and the slider stays stuck at the startup default.
  void seedStreamingPlaybackFromDrop();

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
  void applyShowPointsToDock(DockWidget* dock);
  void applyShowPointsTo2DWidgets();
  void applyLegendStatus(PlotWidget* plot);
  // Slot: left-click cycles or restores the legend position.
  void onLegendButtonClicked();
  // Toggles dots overlay on Lines/LinesAndDots curves only; curves in
  // Dots/Sticks/Steps keep their style.
  void applyDots(PlotWidget* plot);

  // Refreshes button_time_tracker_'s icon to match tracker_info_.
  void updateTimeTrackerIcon();
  // Slot: cycles tracker_info_ to the next state and applies to every plot.
  void onTimeTrackerButtonClicked();

  // Per-tab union of X-ranges across non-XY plots when buttonLink is checked;
  // independent zoom-out otherwise. XY plots always zoom out individually
  // (their X axis is a curve value, not time, so the union is meaningless).
  void linkedZoomOut();

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

  // Re-syncs every data widget to the catalog after a removal: each prunes its
  // own dead pieces (plots drop dead curves; object viewers drop dead layers,
  // resetting the dock to the placeholder when empty). Connected to the
  // catalog's cleared()/itemsRemoved() signals.
  void syncWidgetsToCatalog();

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
  void saveLayoutToPath(const QString& path, bool include_data_source);
  void recordRecentLayout(const QString& path);
  [[nodiscard]] QStringList recentLayouts() const;

  // Picks the dataset a layout's curves bind to. Returns the sole dataset
  // when only one is loaded; otherwise prompts the user (defaulting to the
  // most-recently-loaded). nullopt means the user cancelled the chooser.
  [[nodiscard]] std::optional<DatasetId> chooseActiveDataset(
      const std::vector<std::pair<DatasetId, QString>>& datasets);

  // Rewrites every curve's stable topic+field path to a concrete catalog key
  // in the currently-loaded data (first dataset that has the path). Used by
  // undo/redo restore, whose snapshots carry stable paths, not the per-load
  // keys — so a snapshot survives an intervening data reload.
  void rebindToCurrentSession(QDomDocument& doc);

  // kPlaceholders was removed: the SessionManager API for registering
  // empty placeholder series doesn't exist yet, so the "Create empty
  // placeholders" button was indistinguishable from "Remove from plots"
  // (both just dropped the curves). Re-add the enumerator + the button
  // once the underlying API lands.
  enum class MissingCurveChoice { kRemove, kCancel };

  // Modal prompt mirroring PJ3's missing-curve dialog. `names` is shown to
  // the user (truncated past ~10 entries). Returns the user's pick.
  [[nodiscard]] MissingCurveChoice promptMissingCurves(const QStringList& names);

  // Builds <previouslyLoaded_Datafiles> from SessionManager's record, using
  // a path relative to `layout_dir` when the source lives at or beneath it,
  // absolute otherwise. Returns a null element when no source is recorded.
  [[nodiscard]] QDomElement appendDataSourceElement(QDomDocument& doc, const QDir& layout_dir) const;

  // Builds <right_panel_state visible="…" width="…" style="…"
  // splitter_sizes="…"/> from the four right-panel state sources. Always
  // emits an element (none of the attributes are gated). Caller appends.
  [[nodiscard]] QDomElement saveRightPanelState(QDomDocument& doc) const;

  // Applies <right_panel_state> attributes individually; missing or
  // mismatched values are silently ignored. Never writes to QSettings —
  // layout-driven UI changes don't mutate the global per-user defaults.
  void restoreRightPanelState(const QDomElement& element);

  // Builds <chrome_state left_visible="..." bottom_visible="..."
  // main_splitter_sizes="..." timeline_splitter_sizes="..."/>. Covers
  // cross-panel chrome that isn't owned by an individual panel widget.
  [[nodiscard]] QDomElement saveChromeState(QDomDocument& doc) const;

  // Applies <chrome_state> attributes individually; missing or mismatched
  // values are silently ignored. Visibility goes through the same
  // setVisible + setProperty + setIcon path used by restoreRightPanelState
  // so QSettings stays untouched.
  void restoreChromeState(const QDomElement& element);

  // Applies a panel-visibility flip via the PanelToggle struct that owns
  // the target widget: direct setVisible + icon swap on the toggle
  // button, no QSettings write, no toggle-handler reentry. Used by
  // restoreRightPanelState and restoreChromeState. No-op when target is
  // null OR its current visibility already matches `wanted`.
  void applyPanelVisibility(QWidget* target, bool wanted);

  // Serializes the current app layout state.
  [[nodiscard]] QDomDocument xmlSaveState() const;

  // Loads a previously serialized app layout state.
  bool xmlLoadState(const QDomDocument& state_document);

  // Initializes the undo stack with the post-construction state.
  void pushInitialUndoState();

  // Discards undo/redo history and re-baselines from the current state. Called
  // after a confirmed data removal: prior snapshots are serialized layouts that
  // reference now-deleted curves by key, so replaying one would resurrect a
  // layout pointing at missing data.
  void resetUndoHistory();

  // Adds or replaces the newest undo snapshot.
  void pushUndoState(bool force_new_state = false);

  // Updates enabled state for undo / redo actions.
  void updateUndoRedoActions();

  // Binds the CurveEditor to `plot` and enables/disables the width and
  // style toolbar buttons accordingly (they no-op without an active plot).
  void bindEditorToPlot(PlotWidget* plot);
  [[nodiscard]] PlotWidget* firstPlotOfActiveTab() const;
  // The dock driving the right config panel: the focused dock of the active
  // tab, or its first dock when nothing is focused. Used to refresh the panel
  // on events that don't emit ADS focus (tab switch, data clear) without ever
  // mirroring a dock from a hidden tab.
  [[nodiscard]] DockWidget* activeFocusedDock() const;

 protected:
  // Persists main-window settings before close.
  void closeEvent(QCloseEvent* event) override;

  // Frameless-window edge resize: catches mouse events on ourselves or
  // any descendant widget, updates the cursor near edges, and starts a
  // system-resize on press.
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  // Swaps the chart area (ui_->tabbedPlotWidget) out and presents `panel` in
  // its place; returns false if a panel is already up. restoreCentralArea
  // tears the panel down and restores the chart.
  bool presentPanel(QWidget* panel);
  void restoreCentralArea();

  // Constructs + wires (but does not populate) an object-widget dock of the
  // given kind ("scene3d" / "scene2d"). Shared by both the drop and the
  // layout-restore paths of the object-widget factory; returns nullptr for an
  // unknown kind.
  IDataWidget* makeSceneDock(const QString& kind, QWidget* parent);

  Ui::MainWindow* ui_;
  QtDiagnosticBridge* diagnostic_bridge_ = nullptr;
  DiagnosticHistory* diagnostic_history_ = nullptr;
  QAction* undo_action_ = nullptr;
  QAction* redo_action_ = nullptr;
  std::unique_ptr<AppSession> session_;
  // Owns the per-dataset 3D TF buffers + load-time ingest. Lives here in the
  // shell (not pj_runtime) so the runtime stays domain-neutral. Declared after
  // session_ so it is destroyed first (it holds a reference into session_).
  std::unique_ptr<pj::scene3d::TransformService> transform_service_;
  std::unique_ptr<FileLoader> file_loader_;
  std::unique_ptr<StreamingSourceManager> streaming_manager_;
  // Active streaming dataset id while a session is live (0 = none).
  // Scopes the playback slider range to this dataset's data only so unrelated
  // file/scalar timestamps in the global store don't stretch the slider into
  // ranges where no streamable data exists.
  DatasetId active_streaming_dataset_id_ = 0;
  // Flips to true the first time a streaming topic is dropped into a view,
  // which seeds the playback range + playhead. Until then the slider is left
  // untouched so merely subscribing to topics in the source dialog does not
  // move it. While true, live ingest tracks the live edge until the user
  // pauses.
  bool streaming_playback_seeded_ = false;
  std::unique_ptr<Theme> theme_;
  TitleBar* title_bar_ = nullptr;
  QMenu* recent_layouts_menu_ = nullptr;
  // Help ▸ Installed Extensions — informational, rebuilt on aboutToShow.
  QMenu* installed_extensions_menu_ = nullptr;
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
  QAction* action_preferences_ = nullptr;
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
  // Concrete widget instance behind scene3d_config_page_; held as a
  // distinct member so onDockFocused() can call bindDock() on it
  // without an extra qobject_cast.
  class Scene3DConfigPanel* scene3d_config_panel_ = nullptr;
  // Same, for the 2D scene's layer panel (binds to the focused Scene2DDockWidget).
  class Scene2DConfigPanel* scene2d_config_panel_ = nullptr;
  // Shown when the focused dock holds the 3-icon
  // VisualizationPlaceholderWidget — nothing to configure yet.
  QWidget* empty_dock_page_ = nullptr;

  // Active toolbox panel presented in place of the chart area by
  // presentPanel()/restoreCentralArea(). At most one at a time;
  // panel_parent_/panel_layout_index_ remember where the chart was.
  QWidget* current_panel_ = nullptr;
  int panel_layout_index_ = -1;
  QWidget* panel_parent_ = nullptr;

  // Global-column "Chart" icons — built in buildGlobalToolbar(), so
  // stored as member pointers (no ui_-> accessor).
  QToolButton* button_link_ = nullptr;
  QToolButton* button_time_tracker_ = nullptr;
  QToolButton* button_show_point_ = nullptr;
  QToolButton* button_grid_ = nullptr;
  QToolButton* button_zoom_out_ = nullptr;
  QToolButton* button_ratio_ = nullptr;
  QToolButton* button_dots_ = nullptr;
  QToolButton* button_reference_point_ = nullptr;
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
  // round-tripped; show_points_ applies to plots and 2D image viewers.
  LegendStatus legend_status_ = LegendStatus::kTopRight;
  bool show_points_ = false;
  bool activate_grid_ = false;
  bool dots_ = false;

  // Loaded from QSettings before any child widget is built so the first
  // applyIcons() of each widget already uses the saved metrics.
  ChromeMetrics chrome_metrics_;
  // Three-state cycle for the playback tracker info level (line / +value /
  // +value+name). Default kValue matches PJ3 (mainwindow.cpp:154).
  CurveTracker::Parameter tracker_info_ = CurveTracker::kValue;
  // Session-only — PJ3 doesn't persist this either. Set at toggle-ON to the
  // current playback time; tracker renders Δ values until cleared.
  std::optional<double> reference_time_;
  // PJ3 parity: 1:1 aspect is the expected default for XY plots.
  bool keep_ratio_ = true;
};

}  // namespace PJ
