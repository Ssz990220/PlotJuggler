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
#include <QMetaObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "LayoutXml.h"
#include "pj_base/builtin/builtin_object.hpp"  // sdk::BuiltinObjectType — onPlaceholderTopicDropped's slot parameter
#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/time.hpp"  // PJ::Timepoint — the frame-invariant absolute instant the reference line stores
#include "pj_base/types.hpp"
#include "pj_plotting/CurveTracker.h"
#include "pj_runtime/CurveDescriptor.h"  // openFilterEditor takes std::vector<CurveDescriptor> by value
#include "pj_widgets/ChromeMetrics.h"
#include "pj_widgets/VisualizationKind.h"

class QAction;
class QButtonGroup;
class QCloseEvent;
class QMenu;
class QPushButton;
class QSettings;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace Ui {
class MainWindow;
}

namespace pj::scene3d {
class TransformService;
}  // namespace pj::scene3d

namespace PJ {

class AppSession;
struct CatalogItem;
class CurveEditor;
class DiagnosticHistory;
class DockWidget;
class FileLoader;
class IDataWidget;
class PanelEngine;
class PlotDocker;
class PlotWidget;
class PendingDisplayBinder;
class QtDiagnosticBridge;
class SceneDockWidget;
class StreamingSourceManager;
class IngestProgressWidget;
class SourceTimelineController;
class TopicDemandController;
class SvgButton;
class Timeline;
class RecentFilesMenu;
class Theme;
class TitleBar;
class ToastManager;
class UpdateChecker;
class CoalescingTrigger;

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
  // Headless layout round-trip test reaches the private data-source save/apply
  // seam and the Source Timeline controller through this peer.
  friend class MainWindowSourceLayoutTestPeer;
  friend class MainWindowFanoutAmbiguousTestPeer;
  friend class MainWindowViewportReframeTestPeer;
  friend class MainWindowHistoryTestPeer;

 public:
  // Creates the main window using the default extension directory.
  explicit MainWindow(QWidget* parent = nullptr);

  // Creates the main window using an explicit extension directory.
  explicit MainWindow(QString extensions_dir, QWidget* parent = nullptr);

  // Releases UI resources and the application session.
  ~MainWindow() override;

  // Populates the session with generated data for smoke testing.
  [[nodiscard]] bool populateTestData();

  // Loads `path` on startup and auto-reloads its data source(s) without prompting
  // (the --layout CLI option). Safe no-op if the layout binds to no source.
  void loadLayoutAtStartup(const QString& path);

  // Arms the --autoplay CLI option: begins looping playback the first time a data
  // source provides a non-empty time range (so it works for both the synchronous
  // --test-data path and the async --layout load). Starts once — the user's later
  // pause/seek is respected. No-op if no data ever loads. Call before loading data.
  void enableAutoplay();

  [[nodiscard]] TitleBar* titleBar() const {
    return title_bar_;
  }

  // Diagnostic sink that fans out to the title-bar bell + popup via the
  // DiagnosticHistory service. Use this from tools / CLI / dev feeds to
  // emit through the same pipeline plugins use.
  [[nodiscard]] DiagnosticSink diagnosticSink() const;

  // Shows a transient bottom-right toast. The message may contain rich text
  // (an `<a href>` opens in the system browser). Lazily creates the toast
  // manager on first use.
  void showToast(const QString& message, const QPixmap& icon = QPixmap());

  // Kicks off a one-shot GitHub release check (lazily creating the
  // UpdateChecker). A newer release always pops a toast. `interactive`
  // distinguishes the two entry points: false = automatic startup check, which
  // stays silent unless there's an update (no "up to date" / no error noise);
  // true = the manual Help ▸ Check for Updates action, which also toasts the
  // "you're up to date" and "couldn't check" outcomes.
  void checkForUpdates(bool interactive);

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

  // Persist the current chrome metrics to QSettings in one write. The setters
  // below only apply live (so a Preferences sizing-scrubber drag previews
  // without thrashing the .ini); PreferencesDialog calls this on OK to commit.
  void persistChromeMetrics() const;

  // Plugin search-folder configuration for the Preferences "Plugins" page;
  // delegates to the extension catalog. Custom-folder edits apply on next launch.
  [[nodiscard]] QStringList customPluginFolders() const;
  void setCustomPluginFolders(const QStringList& folders);
  [[nodiscard]] QStringList builtinPluginFolders() const;

 public slots:
  // Apply-only: clamp, update chrome_metrics_, broadcast chromeMetricsChanged.
  // These do NOT write QSettings — persistence is deferred to
  // persistChromeMetrics() so dragging a sizing scrubber previews live but
  // commits once (see PreferencesDialog).
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
  // Layout file flow: open / save / replay-recent. Persists the chosen file
  // path into the recent-layouts list (cap kMaxRecentEntries) regardless of
  // whether the on-disk format is meaningful yet. onLoadRecentLayout is invoked
  // from the LeftPanel recent popup's Layouts section.
  void onLoadLayout();
  void onSaveLayout();
  void onLoadRecentLayout(const QString& path);

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

  // Before deleting `removed_names` (topic/"topic/field" names), find every derived
  // series (transform) that depends on them transitively, warn the user, and — if
  // confirmed — remove those transforms and their Custom Series entries. Returns
  // true to proceed with the deletion, false if the user cancelled. No-op + true
  // when nothing depends on the removed series.
  bool confirmAndRemoveDependentTransforms(const std::vector<std::string>& removed_names);

  // Removes the selected datasets (curve tree "Remove dataset(s)"): shows one
  // combined confirmation, then erases each. Widget sync is signal-driven.
  void onRemoveDatasetsRequested(const QList<DatasetId>& dataset_ids);

  // Merges the selected datasets (curve tree "Merge"): shows the shared
  // destructive-merge confirmation, performs the merge, and marks the result on
  // the Source Timeline so its bar reads as merged.
  void onMergeDatasetsRequested(const QList<DatasetId>& dataset_ids);

  // Erase one dataset's data (TF buffer, object payloads, catalog items, then the
  // engine's scalar storage — in that order) and drop its file association. Shared
  // body of the multi-remove; does NOT confirm, re-seed playback, or reset undo —
  // the caller does those once for the whole batch.
  void removeDatasetData(DatasetId dataset_id);

  void onShowPreferencesDialog();

  // Opens the modal About box (Help ▸ About PlotJuggler…).
  void onShowAboutDialog();

  // Help ▸ Check for Updates… — a manual, always-runs release check that also
  // reports when the app is already current (unlike the silent startup check).
  void onCheckForUpdates();

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
  // `initial_config` (optional) is handed to the toolbox via loadConfig() before
  // its dialog is built — used to open the Transform Editor pre-populated for an
  // in-place edit of an existing derived series.
  void launchToolbox(const QString& plugin_id, const QString& initial_config = QString());

  void onThemeChanged(const QString& theme);

  // Wires callbacks for a newly created plot tab.
  void onPlotTabAdded(PlotDocker* docker);

  // A placeholder 2D/3D icon was clicked: builds an empty scene dock of the
  // matching kind (the shell owns the family→kind mapping) and adopts it into
  // `dock`. Plot clicks are handled inside DockWidget itself.
  void onObjectFamilyRequested(DockWidget* dock, VisualizationKind family);

  // A catalog drop named an advertised placeholder (see
  // DockWidget::placeholderTopicDropped): routes to
  // TopicDemandController::handlePlaceholderPlotDrop (object_type == kNone) or
  // handleSceneDockPlaceholderDrop, instead of the normal add-curve/add-layer path.
  void onPlaceholderTopicDropped(
      DockWidget* dock, DatasetId dataset_id, QString topic_name, sdk::BuiltinObjectType object_type);

  // Routes the focused DockWidget to the right config page and updates
  // the curve-editor binding. Plot-only state changes still go through
  // bindEditorToPlot.
  void onDockFocused(DockWidget* dock);

  // Wires callbacks for a newly created plot widget.
  void onPlotAdded(PlotWidget* plot);

  // Opens the Filter Editor as a chart-area takeover panel (presentPanel) scoped
  // to `sources`. On Apply each filtered output replaces its source curve on
  // `origin` (PlotWidget::replaceCurve — same colour, in place); Close/Apply
  // restore the chart area.
  void openFilterEditor(std::vector<CurveDescriptor> sources, PlotWidget* origin);
  // Push the current global grid / curve-style / curve-width onto a PlotWidget
  // embedded in a plugin toolbox panel (e.g. the Transform Editor plugin), so the
  // app's right-side display buttons drive the plugin preview too.
  void syncPanelPreviewDisplay();
  // Push the current global grid / curve-style / curve-width onto an open Filter
  // Editor's preview plot so it matches the real plots. No-op unless the presented
  // panel is a FilterEditorPanel. Called on open and from the viz-toolbar handlers.
  void syncFilterEditorPreviewDisplay();

  // Mirrors X zoom to linked plots.
  void onPlotZoomChanged(PlotWidget* modified, QRectF rect);

  // Updates playback time from a plot tracker move.
  void onTrackerMovedFromWidget(QPointF point);

  // "Use time offset" toggled: flip the SessionManager frame (plots re-fit via
  // displayOffsetChanged), re-seed the playback range, and shift the playhead by
  // the per-dataset offset delta so the cursor stays on the same real instant.
  void onUseTimeOffsetToggled(bool checked);

  // The representative dataset whose offset frames the global playhead and blue
  // reference line: the active streaming dataset, else the first loaded one (0
  // when the session is empty). Single-dataset sessions are exact; multi-dataset
  // alignment is the future refinement the displayOffset seam leaves room for.
  [[nodiscard]] DatasetId representativeDatasetId() const;

  // The blue reference line's position in display-axis seconds, PROJECTED on
  // demand from reference_instant_ through the current representative-dataset
  // offset. nullopt when no reference is set. Re-evaluated on every frame change
  // (displayOffsetChanged), so the line tracks the offset instead of going stale.
  [[nodiscard]] std::optional<double> referenceDisplaySeconds() const;

  // Push the current reference-line position (referenceDisplaySeconds(), computed
  // once) to every plot and the Source Timeline.
  void broadcastReferenceLine();

  // Drop-time fast path for the live-ingest seed below: if data is already present
  // when the user drops a scalar curve or object topic, set the playback range and
  // playhead immediately. Live ingest can also seed without any drop.
  void seedStreamingPlaybackFromDrop();

  // Put the Source Timeline into read-only mode (called with streaming && playing,
  // from the streaming seek-lock refresh). While the cursor is glued to the live
  // tip, editing source offsets / order / merges or seeking the needle is unsafe;
  // pausing the stream clears it so the retained window can be scrubbed/realigned.
  // Locks the Timeline widget's manipulation gestures (setInteractionLocked) AND
  // greys the external align rail (whose buttons drive the controller directly).
  void setSourceTimelineStreamingLock(bool locked);

  // Records a user-visible plot layout change. `force_new_state` pushes a fresh,
  // non-coalescing undo entry — set it for discrete operations that must never merge
  // into a preceding edit (e.g. applying/removing a filter).
  void onUndoableChange(bool force_new_state = false);

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

  // Applies operation to each dock widget on the active top-level tab only (the
  // on-screen docks). forEachDock's spatial sibling, used by the per-tick fan-out.
  void forEachVisibleDock(const std::function<void(DockWidget*)>& operation);

  // Pushes the current tracker time (display-axis seconds) to every data dock and
  // refreshes the curve-list Value column. The single seam for "the time cursor
  // moved": playback ticks, seeks, and layout/undo restores all route through here.
  void broadcastTrackerTime(double display_seconds);

  // Like broadcastTrackerTime, but only the active top-level tab's docks (plus the
  // always-visible curve-list Value column). The per-tick playback/scrub fan-out
  // uses this so off-screen tabs do no replot/decode/composite work; structural
  // seeds use broadcastTrackerTime (all tabs) and the tab-switch handler re-seeds
  // the newly active tab so a revealed tab is never stale.
  void broadcastTrackerTimeToVisible(double display_seconds);

  // Applies operation to each 2D/3D scene dock hosted in a DockWidget.
  void forEachSceneDock(const std::function<void(SceneDockWidget*)>& operation);

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

  // Builds the timeline panel's align rail — a 24-px icon column on the right
  // edge of the bottom panel that visually continues the global toolbar above
  // it. Holds the Source Timeline alignment actions (align starts / centers);
  // shown only while the strip is open (applyBottomPanelConstraints drives that).
  void buildTimelineAlignRail();

  // Builds the local panel — Curve Width + Curve Style header bands and
  // their flow-layout icon strips above the CurveEditor. Snap/compact
  // behaviour still applies (hidden by the "Toggle Right Panel" button,
  // headers fold below ~72 px wide).
  void buildLocalToolbar();

  // Apply to every curve of the editor's bound plot. No-op when unbound.
  void applyActivePlotWidth(double width);
  void applyActivePlotStyle(int style);

  /// Exact in-memory state of one dataset track. Portable qualifiers are carried
  /// only so a source-replacement snapshot can map a reminted DatasetId; history
  /// replay still requires the raw id to remain live.
  struct TimelineTrackState {
    DatasetId dataset_id = 0;
    qint64 display_offset_ns = 0;
    int timeline_order = -1;
    QString source_name;
    QString source_path;
    int source_index = -1;

    [[nodiscard]] bool operator==(const TimelineTrackState&) const = default;
  };

  /// Source Timeline offsets/order and view chrome kept outside layout XML.
  struct TimelineState {
    std::vector<TimelineTrackState> tracks;
    double zoom = 1.0;
    qint64 scroll_left_ns = 0;
    int scroll_top_px = 0;
    int name_column_width = 0;
    bool snap = true;

    [[nodiscard]] bool operator==(const TimelineState&) const = default;
  };

  /// One atomic workspace snapshot. XML stays on the established schema while
  /// session-only timeline identity and chrome remain in memory.
  struct CapturedWorkspace {
    QByteArray xml;
    TimelineState timeline;

    [[nodiscard]] bool operator==(const CapturedWorkspace&) const = default;
  };

  struct PendingSourceReplacement {
    CapturedWorkspace workspace;
    QString path;
  };

  struct TimelineChromeState {
    double zoom = 1.0;
    qint64 scroll_left_ns = 0;
    int scroll_top_px = 0;
    int name_column_width = 0;
    bool snap = true;
  };

  using TimelineResolutionPlan = std::vector<DatasetId>;

  // Layout helpers.
  void loadLayoutFromPath(const QString& path);
  // Applies a parsed layout to already-loaded data: curve rebind, plot/panel
  // restore, recent-files. The progressive reload path uses beginProgressiveLayoutRestore
  // instead so the structure can appear before the load queue drains.
  void applyRestoredLayout(QDomDocument doc, const QString& path);
  void beginProgressiveLayoutRestore(QDomDocument doc, const QString& path);
  void cancelProgressiveLayoutRestore();
  [[nodiscard]] bool rollbackProgressiveWorkspace();
  [[nodiscard]] bool abortProgressiveRestore();
  void flushPendingCurveBindings(const std::vector<CatalogItem>& items);
  /// Re-registers the document's unresolved curves with the pending binder
  /// (live scene pends survive; see PendingDisplayBinder::collect).
  void collectPendingDisplayBindings(const QDomDocument& doc);
  /// collectPendingDisplayBindings + an immediate drain-pass flush — the
  /// post-replay shape used by restore and rollback.
  void rebuildPendingDisplayBindings(const QDomDocument& doc);
  /// Coalesces plot-owned intent changes into one binder rebuild on the event loop.
  void schedulePendingDisplayBindingRebuild();
  int retryPendingSceneRestores(const std::vector<CatalogItem>& items);
  [[nodiscard]] QStringList unresolvedPendingSceneRestores();
  void clearPendingSceneRestores();
  void onProgressiveLayoutDrained();
  // Re-applies the timeline state (offsets + track order) stashed by a progressive
  // restore, now that the async worker has registered the reloaded datasets' source
  // paths. Called from onProgressiveLayoutDrained BEFORE the viewport re-frame so the
  // saved absolute window converts with the settled offset. Returns whether any
  // offset moved. No-op when nothing was stashed.
  bool applyPendingTimelineState();
  void restoreChromeAndPanels(const QDomDocument& doc, const QString& path);
  void saveLayoutToPath(const QString& path, bool include_data_source);
  void recordRecentLayout(const QString& path);
  [[nodiscard]] QStringList recentLayouts() const;

  // Rewrites every curve's stable topic+field path to a concrete catalog key,
  // resolving across all loaded datasets (first dataset that has the path).
  // Returns the stable paths no loaded dataset could provide. Shared by layout
  // load (which prompts on the unresolved set) and undo/redo restore.
  [[nodiscard]] QList<layout_xml::SeriesPath> rebindCurvesToLoadedDatasets(QDomDocument& doc);

  // How a complete-snapshot restore handles curves no loaded dataset can provide.
  enum class MissingCurvePolicy {
    kPrompt,      ///< layout load: prompt the user (cancel aborts, remove strips them)
    kSilentDrop,  ///< compatibility restore: unresolved curves may be discarded
    kExact,       ///< history/rollback: fail rather than drop unresolved state
  };
  enum class TimelineRestoreMode {
    kExact,
    kPortableSourceReplacement,
  };
  // Outcome of restoreWorkspaceState.
  enum class RestoreResult {
    kApplied,    ///< plots/toggles applied to the session
    kCancelled,  ///< user cancelled at the missing-curve prompt (kPrompt only)
    kFailed,     ///< xmlLoadState rejected the document
  };
  // Restore a COMPLETE workspace snapshot (filters + curve rebinding + plots/toggles)
  // onto the live session — the single restore path shared by layout load and undo/redo,
  // so the two can never drift (that drift is what let undo silently drop filters).
  // Order matters: recreate the snapshot's filters FIRST (so each derived output topic
  // is in the catalog), then rebind curve keys, then apply plots+toggles via
  // xmlLoadState. Snapshots carry stable topic/field paths, not per-load keys, so one
  // survives an intervening data reload. Callers run it under applying_state_ as needed.
  [[nodiscard]] RestoreResult restoreWorkspaceState(
      QDomDocument& doc, MissingCurvePolicy policy, const CapturedWorkspace* rollback_to = nullptr);
  [[nodiscard]] RestoreResult restoreWorkspaceState(
      const CapturedWorkspace& target, MissingCurvePolicy policy, TimelineRestoreMode timeline_mode,
      const CapturedWorkspace* rollback_to = nullptr);

  /// Capture/apply helpers shared by history, rollback, progressive restore, and
  /// source replacement. Timeline validation resolves every id and overflow
  /// guard before the first offset is written.
  [[nodiscard]] CapturedWorkspace captureWorkspace() const;
  [[nodiscard]] CapturedWorkspace capturePortableWorkspace() const;
  [[nodiscard]] TimelineState captureTimelineState() const;
  [[nodiscard]] TimelineChromeState captureTimelineChrome() const;
  void applyTimelineChrome(const TimelineChromeState& state);
  [[nodiscard]] std::optional<DatasetId> resolveTimelineTrack(
      const TimelineTrackState& track, TimelineRestoreMode mode,
      const std::vector<std::pair<DatasetId, QString>>& live_datasets) const;
  [[nodiscard]] std::optional<TimelineResolutionPlan> validateTimelineState(
      const TimelineState& state, TimelineRestoreMode mode) const;
  [[nodiscard]] bool applyTimelineState(const TimelineState& state, const TimelineResolutionPlan& plan);
  [[nodiscard]] RestoreResult applyWorkspace(
      QDomDocument& doc, MissingCurvePolicy policy, const TimelineState* timeline_state,
      const TimelineResolutionPlan* timeline_plan);
  [[nodiscard]] RestoreResult restoreWorkspaceStateImpl(
      QDomDocument& doc, MissingCurvePolicy policy, const TimelineState* timeline_state,
      TimelineRestoreMode timeline_mode, const CapturedWorkspace* rollback_to);

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

  // Re-applies the per-source Source Timeline state saved in `sources` (one
  // DataSourceRef per <fileInfo>) after the layout's datasets are (re)loaded.
  // DatasetIds are re-minted each session, so each saved entry is matched back
  // to a live dataset by source path; matched datasets get their display offset
  // restored and the timeline's vertical track order rebuilt. No-op for
  // generic (data-less) layouts and pre-v3 layouts that carry no timeline attrs.
  // Returns true iff at least one dataset's display offset actually MOVED, so the
  // caller can re-frame plots whose viewport was restored under the pre-apply offset.
  bool applyTimelineStateFromLayout(const QList<layout_xml::DataSourceRef>& sources);

  // Builds <source_timeline zoom="…" scroll_left_ns="…" name_column_width="…"
  // snap="…"/> — the timeline's global VIEW chrome (independent of per-source
  // offsets/order, which ride <fileInfo>). Always emitted (pure UI chrome).
  [[nodiscard]] QDomElement saveSourceTimelineViewState(QDomDocument& doc) const;

  // Applies <source_timeline> view attributes: zoom first, then scroll, then the
  // name-column width, then the snap toggle. Missing attributes are left at their
  // current value. Must run AFTER the datasets reload so zoom/scroll map onto the
  // rebuilt scene.
  void restoreSourceTimelineViewState(const QDomElement& element);

  // Builds <right_panel_state visible="…" width="…" style="…"
  // splitter_sizes="…"/> from the four right-panel state sources. Always
  // emits an element (none of the attributes are gated). Caller appends.
  [[nodiscard]] QDomElement saveRightPanelState(QDomDocument& doc) const;

  // Applies <right_panel_state> attributes individually; missing or
  // mismatched values are silently ignored. Never writes to QSettings —
  // layout-driven UI changes don't mutate the global per-user defaults.
  void restoreRightPanelState(const QDomElement& element);

  // Builds <chrome_state main_splitter_sizes="..." timeline_splitter_sizes="..."/>.
  // Covers cross-panel chrome geometry that isn't owned by an individual panel
  // widget. Panel open/closed state is NOT serialized — it is a QSettings-backed
  // app preference, not document state.
  [[nodiscard]] QDomElement saveChromeState(QDomDocument& doc) const;

  // Applies <chrome_state> splitter geometry individually; missing or mismatched
  // values are silently ignored. Panel visibility is intentionally left as-is
  // (see saveChromeState).
  void restoreChromeState(const QDomElement& element);

  // Serializes the DataProcessorService snapshot into a <data_processors> block:
  // per-curve filters as <processor> entries (INPUT stored as a stable
  // (topic, field) path so it rebinds on reload) and plugin-created transforms as
  // <transform> entries (script + bindings carried by value, inputs/outputs by
  // name). Restore re-applies each filter onto the target dataset BEFORE curve-key
  // rebinding, so the materialized output topics are in the catalog and the
  // filtered curves resolve like any other curve, then replays the transforms.
  [[nodiscard]] QDomElement saveDataProcessors(QDomDocument& doc) const;
  // Resolves each saved filter's input against whichever loaded dataset holds it
  // (first match in load order, mirroring rebindCurvesToLoadedDatasets), so a
  // multi-file layout restores each filter against its own source.
  [[nodiscard]] bool restoreDataProcessors(const QDomElement& root);

  // Size the bottom panel from the Source Timeline strip's open/closed state:
  // when OPEN, pin a minimum height so the strip can't be dragged to a clipped
  // sliver; when CLOSED, fix the panel to the playback bar so the splitter
  // handle can't drag the (hidden) strip open. The single source of truth for
  // bottom-panel height constraints, called from every site that changes the
  // strip's visibility (restore, toggle, layout load) + the chrome-metrics pass.
  void applyBottomPanelConstraints();

  // Align the Source Timeline's left name column so its right separator sits
  // exactly under the start of the playback bar's slider track. Measures the
  // slider's left x within the playback bar and pins the column to it. No-op
  // until the playback bar is laid out (post first show).
  void alignNameColumnToPlayback();

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

  // Replace the current history tip after non-undoable additive data growth or
  // successful navigation, without creating a data-load undo operation.
  void hydrateCurrentUndoState(bool refresh_data_universe = true);
  void restoreHistoryState(const CapturedWorkspace& target, bool undo);
  void reconcileHistoryWithDataUniverse();

  // Exact raw storage identities that make existing history snapshots safe to
  // replay. Processor outputs are workspace state and are excluded.
  [[nodiscard]] QSet<QString> captureHistoryDataUniverse() const;

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

  // First-show hook (runs once the splitters finally have real geometry):
  // (1) restores the remembered left-panel splitter width — one-shot, guarded by
  // left_splitter_restored_; a later --layout load still overrides it; and
  // (2) pins the bottom timeline panel to a clean open/closed height
  // (constructor-time sizing doesn't stick and left the strip squashed), sized
  // deferred after the first layout pass.
  void showEvent(QShowEvent* event) override;

  // Keeps the toast stack pinned to the bottom-right corner as the window
  // resizes.
  void resizeEvent(QResizeEvent* event) override;

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

  // makeSceneDock + seed the new dock's playhead to the current time, so a
  // freshly built empty dock renders at the right moment (currentTimeChanged
  // only fires on changes). Shared by the factory's restore branch and the
  // click-to-create path. Returns nullptr for an unknown kind.
  IDataWidget* makeSeededEmptyObjectDock(const QString& kind, QWidget* parent);

  Ui::MainWindow* ui_;
  QtDiagnosticBridge* diagnostic_bridge_ = nullptr;
  DiagnosticHistory* diagnostic_history_ = nullptr;
  ToastManager* toast_manager_ = nullptr;
  UpdateChecker* update_checker_ = nullptr;
  // Outcome handlers are rebound on each checkForUpdates() call so the check's
  // interactivity (silent startup vs. noisy Help ▸ Check for Updates) is captured
  // per-request rather than living in shared mutable state.
  QMetaObject::Connection update_available_conn_;
  QMetaObject::Connection up_to_date_conn_;
  QMetaObject::Connection check_failed_conn_;
  QAction* undo_action_ = nullptr;
  QAction* redo_action_ = nullptr;
  // App-wide QSettings instance injected into each Scene3DDockWidget
  // (setSettings) for URDF package-resolver persistence — per-source package
  // mappings and global search roots. Owned here so every dock shares one
  // instance whose lifetime outlasts them all.
  std::unique_ptr<QSettings> app_settings_;
  std::unique_ptr<AppSession> session_;
  std::unique_ptr<PendingDisplayBinder> pending_binder_;
  // Resolves displayed plots/scene docks to (DatasetId, topic_name) demand
  // references (see TopicDemandTracker). Declared after pending_binder_, which
  // it forwards placeholder scalar drops to.
  std::unique_ptr<TopicDemandController> topic_demand_controller_;
  // Owns the per-dataset 3D TF buffers + load-time ingest. Lives here in the
  // shell (not pj_runtime) so the runtime stays domain-neutral. Declared after
  // session_ so it is destroyed first (it holds a reference into session_).
  std::unique_ptr<pj::scene3d::TransformService> transform_service_;
  std::unique_ptr<FileLoader> file_loader_;
  std::unique_ptr<StreamingSourceManager> streaming_manager_;
  // ~30 Hz rate cap for the tracker-time fan-out. Every per-cursor-move driver
  // (playback ticks, scrubbing, seeks) routes through one coalescer so the
  // expensive per-widget work (plot replot, scene decode/composite, value column)
  // runs at most ~30 Hz regardless of how fast the cursor changes. Structural
  // seeds (load/restore/undo) call broadcastTrackerTime() directly, unthrottled,
  // so a freshly shown dock is never blank waiting for the trailing edge.
  // pending_tracker_time_ holds the newest cursor time the trailing edge will use.
  std::unique_ptr<CoalescingTrigger> tracker_broadcast_trigger_;
  double pending_tracker_time_ = 0.0;
  // Binds the Source Timeline widget (mounted in timelineStrip) to the runtime.
  // Parented to this MainWindow (QObject-owned), so it is a raw pointer.
  SourceTimelineController* source_timeline_controller_ = nullptr;
  // The Source Timeline widget itself (QObject-owned by timelineStrip). Held so
  // the reference-line toggle can drive its blue reference needle.
  Timeline* source_timeline_ = nullptr;
  // User's chosen name-column width (px); 0 = none, use the playback-aligned
  // floor. Tracked from Timeline::nameColumnWidthChanged and restored from a
  // layout so alignNameColumnToPlayback keeps the column at this width.
  int timeline_name_column_width_ = 0;
  // Active streaming dataset id while a session is live (0 = none).
  // Scopes the playback slider range to this dataset's data only so unrelated
  // file/scalar timestamps in the global store don't stretch the slider into
  // ranges where no streamable data exists.
  DatasetId active_streaming_dataset_id_ = 0;
  // Flips to true the first time a streaming topic is dropped into a view —
  // a scalar curve into a plot (PlotWidget::curvesDropped) or an object topic
  // into a 2D/3D dock (PlotDocker::firstObjectTopicAdded) — which seeds the
  // playback range + playhead. Until then the slider is left untouched so merely
  // subscribing to topics in the source dialog does not move it. Seeding is
  // one-shot per session (seedStreamingPlaybackFromDrop early-returns once set),
  // so a later drop can't re-snap a paused, scrubbed-back cursor. While true,
  // live ingest tracks the live edge until the user pauses.
  bool streaming_playback_seeded_ = false;
  std::unique_ptr<Theme> theme_;
  TitleBar* title_bar_ = nullptr;
  // Non-modal load progress strip parked in the title bar's center region (owned
  // by title_bar_ once injected). Shown after a short delay so quick loads don't
  // flash it; hidden a moment after the load queue drains.
  IngestProgressWidget* ingest_progress_ = nullptr;
  QTimer* ingest_show_timer_ = nullptr;
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
  std::deque<CapturedWorkspace> undo_states_;
  std::deque<CapturedWorkspace> redo_states_;
  QSet<QString> history_data_universe_;
  QElapsedTimer undo_timer_;
  bool applying_state_ = false;
  bool progressive_layout_in_flight_ = false;
  QMetaObject::Connection pending_items_added_conn_;
  QMetaObject::Connection pending_queue_drained_conn_;
  bool pending_binding_rebuild_scheduled_ = false;
  // Timeline state (per-source offsets + track order) extracted during a progressive
  // restore but not yet applicable: the async worker had not registered the reloaded
  // datasets' source paths when restoreChromeAndPanels ran, so the offsets were
  // skipped. onProgressiveLayoutDrained re-applies these once the paths settle. Empty
  // outside a progressive restore.
  QList<layout_xml::DataSourceRef> pending_timeline_sources_;
  // The saved target must outlive begin so processors can replay only at drain.
  QDomDocument progressive_layout_doc_;
  std::optional<CapturedWorkspace> progressive_previous_workspace_;
  std::optional<PendingSourceReplacement> pending_source_replacement_;
  // Set only while a --layout CLI load runs, so loadLayoutFromPath auto-reloads the
  // layout's source(s) instead of prompting.
  bool startup_auto_reload_ = false;
  // True between enableAutoplay() and the first range-driven playback start (the
  // --autoplay one-shot); cleared once playback begins so user control is respected.
  bool autoplay_pending_ = false;
  // One-shot guard so the remembered left-panel splitter width is restored only on
  // the first showEvent (later shows must not clobber a user/layout adjustment).
  bool left_splitter_restored_ = false;
  bool bottom_panel_sized_ = false;  // first-show guard for the bottom-panel sizing
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
  // current_panel_engine_ is the owning PanelEngine when the panel is a
  // toolbox dialog (null for non-toolbox panels like the console / filter
  // editor); presentPanel() uses it to tear the previous toolbox down before
  // showing a new one, so launching a toolbox replaces the open panel.
  QWidget* current_panel_ = nullptr;
  PanelEngine* current_panel_engine_ = nullptr;
  int panel_layout_index_ = -1;
  QWidget* panel_parent_ = nullptr;
  // The plot a Filter Editor panel was opened on. Its style/width drive the
  // before/after preview (the preview mirrors THAT plot, not a global default).
  // Set after presentPanel() succeeds; cleared in restoreCentralArea(). QPointer so
  // a torn-down origin reads back null. Only non-null while a FilterEditorPanel is up.
  QPointer<PlotWidget> filter_editor_origin_;

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
  // Re-bases the time axis to start near zero (each dataset relative to its own
  // earliest sample) instead of absolute Unix-epoch time. The frame state lives
  // in SessionManager; this button mirrors it.
  QToolButton* button_t0_ = nullptr;
  // Global-column "Legend" button — single icon that combines a corner
  // picker (left-click) with a show/hide toggle (right-click). Checked
  // while the legend is shown at one of the four corners; unchecked
  // when hidden. The icon always reflects the "current position": the
  // active corner while checked, or the saved corner that will be
  // restored on the next show while unchecked.
  SvgButton* button_legend_ = nullptr;  // SvgButton so setLegendStatus can setIconPath() the corner glyph
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
  // playback instant; tracker renders Δ values until cleared. Stored as a
  // frame-invariant absolute Timepoint (NOT a display coordinate) so the blue
  // line holds its instant when the display offset shifts — the "Use time
  // offset" toggle, or a load that lowers a dataset's earliest stamp. The axis
  // position is derived on demand by referenceDisplaySeconds().
  std::optional<Timepoint> reference_instant_;
  // PJ3 parity: 1:1 aspect is the expected default for XY plots.
  bool keep_ratio_ = true;
};

}  // namespace PJ
