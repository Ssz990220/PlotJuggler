// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "MainWindow.h"

#include <qwt_plot_curve.h>
#include <qwt_text.h>

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QByteArray>
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHash>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <utility>
#include <vector>

#include "DatasetMergeActions.h"
#include "DebugUi.h"
#include "FileLoader.h"
#include "LayoutXml.h"
#include "PendingDisplayBinder.h"
#include "PreferencesDialog.h"
#include "RasterKeyMap.h"
#include "SourceTimelineController.h"
#include "StreamingSourceManager.h"
#include "Theme.h"
#include "TitleBar.h"
#include "TopicDemandController.h"
#include "pj_base/dataset.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/data_processor.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_marketplace/marketplace_window.hpp"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_plotting/CurveEditor.h"
#include "pj_plotting/CurveTracker.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/FilterEditorPanel.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_plugins/host/dialog_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_plugins/host/toolbox_handle.hpp"
#include "pj_plugins/host_qt/panel_engine.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/DataProcessorsRuntimeHost.h"
#include "pj_runtime/DiagnosticHistory.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/IObjectViewer.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/QSettingsBackend.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/ToolboxRuntimeHost.h"
#include "pj_runtime/UpdateChecker.h"
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#include "pj_scene2d_widgets/media_viewer_widget.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/transform_service.h"
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_widgets/CoalescingTrigger.h"
#include "pj_widgets/FileDialog.h"
#include "pj_widgets/FlowLayout.h"
#include "pj_widgets/IngestProgressWidget.h"
#include "pj_widgets/MessageBox.h"
#include "pj_widgets/RasterStreamView.h"
#include "pj_widgets/SectionHeaderBand.h"
#include "pj_widgets/SvgButton.h"
#include "pj_widgets/SvgUtil.h"
#include "pj_widgets/Timeline.h"
#include "pj_widgets/ToastManager.h"
#include "scene_object_classification.h"
#include "ui/AboutDialog.h"
#include "ui/CurveListPanel.h"
#include "ui/DiagnosticsDetailDialog.h"
#include "ui/LeftPanel.h"
#include "ui/Scene2DConfigPanel.h"
#include "ui/Scene3DConfigPanel.h"
#include "ui/TimelineWidget.h"
#include "ui_MainWindow.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcMain, "pj.app.main")

// Check the button with id `id` in an exclusive QButtonGroup as a passive UI
// resync, with the group's signals blocked. The block is load-bearing: a passive
// resync (e.g. binding the toolbar to a newly-focused plot) must not run the Curve
// Width/Style groups' click side effects — refreshing the open preview panels.
void checkGroupButton(QButtonGroup* group, int id) {
  if (group == nullptr) {
    return;
  }
  if (auto* btn = group->button(id)) {
    const QSignalBlocker blocker(group);
    btn->setChecked(true);
  }
}

// Layout-relative form of `absolute_path` for persisting a data-source
// reference: the relative path when the data lives at or beneath `layout_dir`,
// else the absolute path. This diverges from PJ3 (which always stores relative)
// — PJ4 avoids brittle ../.. paths so moving a layout file doesn't silently break
// the data reference. A relative path counts as a "subpath" only when Qt's
// relativeFilePath did NOT emit a "../" prefix or the literal ".." path; the
// simpler `!rel.startsWith("..")` check would misclassify legitimate filenames
// like "..foo" or "..bar/data.csv" as escaping the dir.
[[nodiscard]] QString relocatableSubpath(const QString& absolute_path, const QDir& layout_dir) {
  const QString relative = layout_dir.relativeFilePath(absolute_path);
  const bool is_subpath = relative != ".."_L1 && !relative.startsWith("../"_L1);
  return is_subpath ? relative : absolute_path;
}

constexpr auto kDefaultRegistryUrl =
    "https://raw.githubusercontent.com/PlotJuggler/pj-plugin-registry/"
    "refs/heads/development/registry.json";
constexpr auto kRegistryUrlSettingsKey = "Marketplace/registryUrl";
constexpr auto kPanelBottomExpandedKey = "MainWindow.panelBottomExpandedHeight";
// Minimum height (px) of the Source Timeline strip when the bottom panel is open
// — enough for the ruler + a few source bars so it never opens clipped.
constexpr int kMinTimelineStripHeight = 150;

// Directory of the most recently saved or loaded layout. Re-using PJ3's
// QSettings key keeps cross-version migration trivial (a user who
// upgrades from PJ3 lands in their existing layout directory). Fallback
// when unset is QDir::currentPath() — matches PJ3.
constexpr auto kLastLayoutDirKey = "MainWindow.lastLayoutDirectory";

// Layout schema version. Bumped only on incompatible changes (additive
// elements/attributes don't need a bump — the loader silently skips
// unknown content). Read by loadLayoutFromPath to flag layouts saved
// by newer PJ4 builds; the layout still loads best-effort.
// v2: curves identified by stable topic+field path (rebound per-dataset on
// load) instead of the opaque per-load catalog key; <root binding=...> marks
// generic vs source-bound layouts.
// v3: per-plot <range> stores the X (time) axis in ABSOLUTE seconds — the per-dataset
// display offset is a visualization concern applied at load, never persisted. No marker
// is written; the plot mode (time-series vs XY) decides on load whether to undo the
// offset, and an unmarked legacy range is also read as absolute. The global toolbar
// toggles + panel visibility are no longer serialized (they are QSettings-only app
// preferences, not document state).
// v3 also round-trips the Source Timeline. Per-source: each <fileInfo> carries
// display_offset_ns + timeline_order, re-bound by source path so a dataset's bar
// offset and vertical slot restore exactly. Global view chrome: a <source_timeline>
// element carries zoom + scroll_left_ns + name_column_width + snap. Additive — older
// readers ignore the new attributes/element; this build tolerates their absence in
// pre-v3 layouts.
constexpr int kLayoutSchemaVersion = 3;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr int kTestSampleCount = 1000;
constexpr double kTestDurationSeconds = 10.0;
constexpr int kResizeMargin = 6;
// ~30 Hz cap for the tracker-time fan-out (plots/scenes/value column). Kept equal
// to PlaybackEngine's kTickIntervalMs (30 Hz) so playback passes through ~1:1; the
// coalescer's job is to additionally cap faster drivers (scrubbing at mouse-move
// rate, programmatic seeks) that bypass the playback tick.
constexpr int kTrackerBroadcastIntervalMs = 33;
// Per-section cap for the recent popup — at most this many Layouts AND this
// many Files are retained (the two lists are independent).
constexpr int kMaxRecentEntries = 8;
constexpr auto kRecentLayoutsKey = "Layout/recent";
constexpr auto kLayoutFilter = "PlotJuggler 4 Layout (*.pj4.xml)";
// Extension itself is the single source of truth in LayoutXml::kLayoutExtension.
constexpr int kMaxUndoStates = 100;
constexpr qint64 kUndoCoalesceMs = 100;
constexpr auto kIconSizeKey = "ui/icon_size";
constexpr auto kIconPaddingKey = "ui/icon_padding";
constexpr auto kLayoutPaddingKey = "ui/layout_padding";
constexpr auto kLayoutSpacingKey = "ui/layout_spacing";
constexpr Range<int> kIconSizeRange{12, 48};
constexpr Range<int> kIconPaddingRange{0, 32};
constexpr Range<int> kLayoutPaddingRange{0, 16};
constexpr Range<int> kLayoutSpacingRange{0, 16};
constexpr int kIconSizeDefault = 24;
constexpr int kIconPaddingDefault = 4;
constexpr int kLayoutPaddingDefault = 2;
constexpr int kLayoutSpacingDefault = 2;

Qt::Edges edgesAtPoint(const QSize& window_size, const QPoint& pos) {
  Qt::Edges edges;
  if (pos.x() <= kResizeMargin) {
    edges |= Qt::LeftEdge;
  } else if (pos.x() >= window_size.width() - kResizeMargin) {
    edges |= Qt::RightEdge;
  }
  if (pos.y() <= kResizeMargin) {
    edges |= Qt::TopEdge;
  } else if (pos.y() >= window_size.height() - kResizeMargin) {
    edges |= Qt::BottomEdge;
  }
  return edges;
}

Qt::CursorShape cursorForEdges(Qt::Edges edges) {
  switch (static_cast<int>(edges)) {
    case Qt::TopEdge | Qt::LeftEdge:
    case Qt::BottomEdge | Qt::RightEdge:
      return Qt::SizeFDiagCursor;
    case Qt::TopEdge | Qt::RightEdge:
    case Qt::BottomEdge | Qt::LeftEdge:
      return Qt::SizeBDiagCursor;
    case Qt::TopEdge:
    case Qt::BottomEdge:
      return Qt::SizeVerCursor;
    case Qt::LeftEdge:
    case Qt::RightEdge:
      return Qt::SizeHorCursor;
    default:
      return Qt::ArrowCursor;
  }
}

struct PanelToggle {
  QPushButton* button;
  QWidget* target;
  const char* icon_path_on;   // filled glyph — panel visible.
  const char* icon_path_off;  // unfilled glyph — panel hidden.
};

std::array<PanelToggle, 3> panelToggles(Ui::MainWindow* ui) {
  // Material's "Dock to Left" / "Dock to Right" glyphs fill the half
  // of the frame opposite to the side they nominally dock toward, so
  // the left-panel toggle reads correctly with the panel_right.svg
  // asset and vice versa.
  return {{
      {ui->tabbedPlotWidget->leftPanelButton(), ui->leftColumn, ":/resources/svg/panel_right.svg",
       ":/resources/svg/panel_right_off.svg"},
      // Toggle target is timelineStrip, NOT the whole bottomPanel — the
      // playback strip (timelineWidget) sits above the strip in the same
      // panel and must remain visible at all times. Resize of the
      // bottomPanel via the splitter handle grows the strip; the playback
      // keeps its fixed height (sizePolicy Fixed-vertical in MainWindow.ui).
      {ui->tabbedPlotWidget->bottomPanelButton(), ui->timelineStrip, ":/resources/svg/panel_bottom.svg",
       ":/resources/svg/panel_bottom_off.svg"},
      {ui->tabbedPlotWidget->rightPanelButton(), ui->localToolbarWidget, ":/resources/svg/panel_left.svg",
       ":/resources/svg/panel_left_off.svg"},
  }};
}

QUrl registryUrlFromSettings() {
  const QString raw = QSettings().value(kRegistryUrlSettingsKey, kDefaultRegistryUrl).toString();
  const QUrl url(raw);
  if (!url.isValid() || url.scheme().isEmpty()) {
    qCWarning(lcMain) << "Invalid" << kRegistryUrlSettingsKey << "in QSettings:" << raw << "— falling back to default.";
    return QUrl(QString::fromLatin1(kDefaultRegistryUrl));
  }
  return url;
}

// Curve-Width radio mapping. Hoisted from buildLocalToolbar so the
// layout-save path can encode the float value (rebuild-stable) and the
// layout-load path can map the stored value back to a button.
inline constexpr std::array<std::pair<const char*, double>, 4> kWidthButtonSpecs{{
    {"globalWidth1_0", 1.0},
    {"globalWidth1_5", 1.5},
    {"globalWidth2_0", 2.0},
    {"globalWidth3_0", 3.0},
}};
}  // namespace

MainWindow::MainWindow(QWidget* parent) : MainWindow(QString{}, parent) {}

MainWindow::MainWindow(QString extensions_dir, QWidget* parent)
    : QMainWindow(parent),
      ui_(new Ui::MainWindow),
      diagnostic_bridge_(new QtDiagnosticBridge(this)),
      app_settings_(std::make_unique<QSettings>()),
      session_(std::make_unique<AppSession>(std::move(extensions_dir), diagnostic_bridge_->sink())),
      pending_binder_(
          std::make_unique<PendingDisplayBinder>(session_->catalogModel(), &session_->topicDemandTracker())),
      topic_demand_controller_(
          std::make_unique<TopicDemandController>(
              session_->catalogModel(), session_->topicDemandTracker(),
              session_->sessionManager().dataProcessorService(), *pending_binder_,
              session_->sessionManager().dataEngine())),
      theme_(std::make_unique<Theme>()) {
  // The 3D transform service owns the per-dataset TF buffers + load-time
  // ingest. It lives in the shell (not pj_runtime) so the runtime stays
  // domain-neutral; it reads only SessionManager's neutral surface.
  transform_service_ = std::make_unique<pj::scene3d::TransformService>(session_->sessionManager());
  // Pull saved icon metrics before setupUi so the literals we feed into
  // build*Toolbar() pick up the correct values on first paint. Widgets
  // that auto-construct from the .ui still draw at their default sizes
  // during setupUi, but we re-broadcast at the end of the constructor.
  {
    QSettings s;
    chrome_metrics_.icon_size =
        kIconSizeRange.clamp(s.value(QString::fromLatin1(kIconSizeKey), kIconSizeDefault).toInt());
    chrome_metrics_.icon_padding =
        kIconPaddingRange.clamp(s.value(QString::fromLatin1(kIconPaddingKey), kIconPaddingDefault).toInt());
    chrome_metrics_.layout_padding =
        kLayoutPaddingRange.clamp(s.value(QString::fromLatin1(kLayoutPaddingKey), kLayoutPaddingDefault).toInt());
    chrome_metrics_.layout_spacing =
        kLayoutSpacingRange.clamp(s.value(QString::fromLatin1(kLayoutSpacingKey), kLayoutSpacingDefault).toInt());
  }

  ui_->setupUi(this);

  // Hard-zero contents margins on the QMainWindow itself, the central
  // widget, and every intermediate container down to the chrome rows.
  // Qt's main-window layout or platform style can otherwise add a tiny
  // implicit gap below the menuWidget (TitleBar), which the user sees
  // as a strip of titlebar-background between the title bar and the
  // first chrome row of the central area.
  setContentsMargins(0, 0, 0, 0);
  ui_->centralWidget->setContentsMargins(0, 0, 0, 0);
  ui_->upperArea->setContentsMargins(0, 0, 0, 0);
  ui_->leftColumn->setContentsMargins(0, 0, 0, 0);
  ui_->bottomPanel->setContentsMargins(0, 0, 0, 0);
  ui_->leftPanel->setContentsMargins(0, 0, 0, 0);
  ui_->curveListPanel->setContentsMargins(0, 0, 0, 0);
  ui_->tabbedPlotWidget->setContentsMargins(0, 0, 0, 0);
  ui_->timelineSplitter->setContentsMargins(0, 0, 0, 0);
  ui_->mainSplitter->setContentsMargins(0, 0, 0, 0);
  ui_->rightToolbarSplitter->setContentsMargins(0, 0, 0, 0);
  ui_->plotsAndGlobalContainer->setContentsMargins(0, 0, 0, 0);
  ui_->globalToolbarWidget->setContentsMargins(0, 0, 0, 0);

  // Qt 6.8 QRhiWidget needs an RHI-capable top-level backing store from
  // the first show(). Keep a zero-size viewer in an existing visible layout
  // so image docks created later can initialize their QRhi.
  auto* rhi_bootstrap = new MediaViewerWidget(ui_->globalToolbarWidget);
  rhi_bootstrap->setObjectName(u"rhi_bootstrap"_s);
  rhi_bootstrap->setMaximumSize(0, 0);
  rhi_bootstrap->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  if (auto* global_toolbar_layout = qobject_cast<QVBoxLayout*>(ui_->globalToolbarWidget->layout())) {
    global_toolbar_layout->addWidget(rhi_bootstrap);
  }

  ui_->mainSplitter->setHandleWidth(1);
  ui_->timelineSplitter->setHandleWidth(1);
  ui_->rightToolbarSplitter->setHandleWidth(1);
  // Plain QWidget doesn't paint QSS borders unless this attribute is
  // set; needed for #timelineStrip's `border-top` (the 1-px separator
  // between the playback bar and the timeline strip).
  ui_->timelineStrip->setAttribute(Qt::WA_StyledBackground, true);

  // Vertical splitter between the upper plot/panels area and the
  // timeline strip — extra window height grows the upper area, the
  // timeline keeps its requested size unless the user drags the
  // handle. Initial bottom size matches the playback bar's height
  // exactly so no empty strip is allocated below the playback on
  // first launch; user grows the strip by dragging the handle.
  ui_->timelineSplitter->setStretchFactor(0, 1);
  ui_->timelineSplitter->setStretchFactor(1, 0);
  ui_->timelineSplitter->setSizes({1000, ui_->timelineWidget->minimumHeight()});

  // Horizontal splitter with two panes — the plots-and-global-column
  // container (left sidebar + plot area + fixed 24-px global icon
  // column, separated by a static
  // 1-px QFrame border) and the foldable local panel. The only
  // draggable boundary is between the global column and the local
  // panel. Toggle Right Panel hides/shows the local panel; there is no
  // snap or compact mode.
  ui_->rightToolbarSplitter->setStretchFactor(0, 1);
  ui_->rightToolbarSplitter->setStretchFactor(1, 0);
  // Right side-panel floor: wide enough for the scene config panels' label
  // columns (and well past the 6 × 24-px Curve Style icon strip so the
  // FlowLayout never wraps it into two rows).
  ui_->localToolbarWidget->setMinimumWidth(250);
  ui_->rightToolbarSplitter->setSizes({2000, 240});
  ui_->rightToolbarSplitter->setOpaqueResize(true);

  // Pin the playback strip's minimum height to its preferred height so
  // the bottomPanel's minimumSizeHint (computed by its QVBoxLayout) ends
  // up at exactly the playback's natural height. With childrenCollapsible
  // false on the splitter, this becomes the floor: the user can't drag
  // the splitter handle low enough to clip the playback controls.
  ui_->timelineWidget->setMinimumHeight(ui_->timelineWidget->sizeHint().height());

  // Frameless window: drop the WM-drawn chrome so our TitleBar can own
  // the top of the window. Edge resize is implemented via an
  // application-level event filter installed below.
  setWindowFlag(Qt::FramelessWindowHint, true);
  setMouseTracking(true);

  // The TitleBar owns the QMenuBar's popup menus; we just push actions
  // into them.
  title_bar_ = new TitleBar(this);
  connect(title_bar_, &TitleBar::diagnosticActivated, this, [this](const DiagnosticRecord& r) {
    auto* dlg = new DiagnosticsDetailDialog(r, this);
    dlg->show();
  });

  // File menu: layout persistence + marketplace + preferences + quit.
  // Recent layouts are no longer a File-menu submenu — they live in the
  // LeftPanel "recent" popup (Layouts section) alongside recent data files,
  // wired below via LeftPanel::recentLayoutSelected.
  QMenu* file_menu = title_bar_->fileMenu();
  action_load_layout_ = file_menu->addAction(tr("Load Layout..."), this, &MainWindow::onLoadLayout);
  action_save_layout_ = file_menu->addAction(tr("Save Layout..."), this, &MainWindow::onSaveLayout);
  file_menu->addSeparator();
  // Extensions Marketplace temporarily hidden — uncomment to restore the
  // File-menu entry. The actionMarketplace QAction, its triggered() wiring,
  // and onOpenMarketplace() are all left intact below; this only removes the
  // visible menu item.
  // file_menu->addAction(ui_->actionMarketplace);
  action_preferences_ = file_menu->addAction(tr("Preferences..."), this, &MainWindow::onShowPreferencesDialog);
  file_menu->addSeparator();
  file_menu->addAction(ui_->actionExit);

  // Toolbox menu: lazily lists the launchable (non-cloud) toolboxes so
  // the list reflects whatever the catalog currently has loaded.
  connect(title_bar_->toolboxMenu(), &QMenu::aboutToShow, this, &MainWindow::onRebuildToolboxMenu);

  // Help menu: About + web links + the informational extension list
  // (rebuilt on aboutToShow; managing extensions happens in the
  // Marketplace, reached from the File menu).
  QMenu* help_menu = title_bar_->helpMenu();
  help_menu->addAction(tr("About PlotJuggler..."), this, &MainWindow::onShowAboutDialog);
  help_menu->addAction(tr("Check for Updates..."), this, &MainWindow::onCheckForUpdates);
  help_menu->addAction(
      tr("Documentation"), this, []() { QDesktopServices::openUrl(QUrl(u"https://plotjuggler.io"_s)); });
  help_menu->addAction(tr("Report an Issue"), this, []() {
    QDesktopServices::openUrl(QUrl(u"https://github.com/PlotJuggler/PJ4/issues"_s));
  });
  help_menu->addSeparator();
  installed_extensions_menu_ = help_menu->addMenu(tr("Installed Extensions"));
  installed_extensions_menu_->setObjectName(u"PJMenu"_s);
  connect(installed_extensions_menu_, &QMenu::aboutToShow, this, &MainWindow::onRebuildExtensionsMenu);

  setMenuWidget(title_bar_);

  // The panel toggles live in the title bar's right cluster (left of
  // the bell). TabbedPlotWidget creates them — reparenting here keeps
  // the panelToggles() wiring and QSettings persistence untouched.
  title_bar_->addRightClusterWidget(ui_->tabbedPlotWidget->leftPanelButton());
  title_bar_->addRightClusterWidget(ui_->tabbedPlotWidget->bottomPanelButton());
  title_bar_->addRightClusterWidget(ui_->tabbedPlotWidget->rightPanelButton());

  // The event filter sees mouse events delivered to any descendant of
  // this window. Required so a click on the TimelineWidget's empty
  // bottom 6px still starts a window resize.
  qApp->installEventFilter(this);

  // The DiagnosticHistory is the single source of truth for diagnostics.
  // It listens to the bridge (so plugins / core services flow in) and is
  // observed by the title-bar bell + popup.
  diagnostic_history_ = new DiagnosticHistory(this);
  diagnostic_history_->connectBridge(diagnostic_bridge_);
  title_bar_->setDiagnosticHistory(diagnostic_history_);

  ui_->tabbedPlotWidget->setDataServices(&session_->sessionManager(), &session_->catalogModel());
  ui_->tabbedPlotWidget->setObjectWidgetFactory(
      [this](const QString& kind, const ObjectDropSeed* seed, QWidget* dock_parent) -> IDataWidget* {
        // One factory for both paths. Layout restore passes the saved XML tag as
        // `kind` with a null seed (the dock reloads its own state); a catalog
        // drop passes an empty kind with a seed, which we classify into a kind,
        // construct, and populate. Family routing for v0:
        //   3D-ish (kPointCloud/kFrameTransforms/kOccupancyGrid)                     → scene3d
        //   2D-ish (kImage/kDepthImage/kImageAnnotations/kSceneEntities/kVideoFrame) → scene2d
        QString resolved_kind = kind;
        if (resolved_kind.isEmpty() && seed != nullptr) {
          // Classify the dropped object into a scene kind.
          //  - Image-family types (incl. depth-encoded kImage) open the 2D viewer
          //    on a placeholder drop. A depth image is also hostable in 3D, but
          //    depth→3D is an explicit action (drop onto an existing 3D dock, or
          //    "Open in 3D view"); routing 2D-first also means a *color* image is
          //    never auto-routed to a 3D dock that would then reject it.
          //  - kSceneEntities is both 2D and 3D; is3d wins (markers are primarily
          //    3D), but a 2D dock still accepts markers dropped onto it.
          //  - Neither → "".
          resolved_kind = isImageFamilyObjectType(seed->object_type) ? u"scene2d"_s
                          : is3dSceneObjectType(seed->object_type)   ? u"scene3d"_s
                          : is2dSceneObjectType(seed->object_type)   ? u"scene2d"_s
                                                                     : QString();
        }
        if (seed == nullptr) {
          // Restore / click-create path: build the empty dock and seed its
          // playhead. On restore, PlotDocker then calls xmlLoadState() to
          // repopulate topics + per-layer config; on click-create the dock waits
          // for its first dropped topic. Seeding first means each restored layer
          // comes up at the current playhead, not its first sample
          // (currentTimeChanged only fires on changes; this path also rebuilds
          // docks on undo/redo — M.1). An unknown kind yields nullptr, which on
          // this (seedless) path just means "not my kind" and is silent.
          return makeSeededEmptyObjectDock(resolved_kind, dock_parent);
        }

        if (seed->topic_id == ObjectTopicId{}) {
          // Placeholder drop: the topic is advertised (classified) but has no
          // storage id yet — data only starts flowing once the drop registers
          // demand. Build the empty dock of the resolved family; the drop site
          // stages a pending drop that completes with the real ObjectTopicId
          // when the subscription delivers the first sample
          // (TopicDemandController::handleSceneDockPlaceholderDrop).
          return makeSeededEmptyObjectDock(resolved_kind, dock_parent);
        }

        // Drop path: build, then populate the first topic and apply view
        // side-effects. An unknown kind here is a real failure — tell the user.
        IDataWidget* widget = makeSceneDock(resolved_kind, dock_parent);
        if (widget == nullptr) {
          MessageBox::warning(
              this, tr("Cannot display topic"),
              tr("This object topic cannot be displayed (object_type=%1).").arg(static_cast<int>(seed->object_type)));
          return nullptr;
        }
        QWidget* qwidget = widget->widget();
        if (auto* scene3d = qobject_cast<Scene3DDockWidget*>(qwidget)) {
          if (!scene3d->addTopic(seed->topic_id, seed->object_type, seed->title)) {
            scene3d->deleteLater();
            MessageBox::warning(
                this, tr("Cannot display topic"),
                tr("This object topic cannot be displayed in a 3D view (object_type=%1). "
                   "The 3D view requires a registered parser that emits one of its supported "
                   "canonical objects (PointCloud, CompressedPointCloud, OccupancyGrid, "
                   "SceneEntities, or FrameTransforms).")
                    .arg(static_cast<int>(seed->object_type)));
            return nullptr;
          }
          // A 3D-only stream must seed playback here too (see header doc).
          seedStreamingPlaybackFromDrop();
          // currentTimeChanged only fires on changes, so a brand-new widget
          // never gets the current playhead — seed it now to render at the right
          // time immediately.
          scene3d->onTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
        } else if (auto* media2d = qobject_cast<Scene2DDockWidget*>(qwidget)) {
          if (!media2d->setImageTopic(seed->topic_id, seed->object_type, seed->title)) {
            media2d->deleteLater();
            MessageBox::warning(
                this, tr("Cannot display topic"),
                tr("This object topic cannot be displayed in a 2D view (object_type=%1). "
                   "Typically this means the source did not register a parser for the topic, "
                   "or the type is not yet supported by the built-in viewer.")
                    .arg(static_cast<int>(seed->object_type)));
            return nullptr;
          }
          seedStreamingPlaybackFromDrop();
          media2d->setPointInspectorEnabled(show_points_);
          media2d->onTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
        } else {
          // makeSceneDock produced a kind this populate switch doesn't handle —
          // a programming error if a new family is added without a branch here.
          // Fail loudly rather than returning an unpopulated dock.
          qWarning("MainWindow: makeSceneDock returned an unhandled object-widget kind");
          widget->widget()->deleteLater();
          return nullptr;
        }
        return widget;
      });
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::tabAdded, this, &MainWindow::onPlotTabAdded);
  wireExistingPlots();
  ui_->curveListPanel->setCatalog(&session_->catalogModel());
  ui_->curveListPanel->setTopicDemandTracker(&session_->topicDemandTracker());
  ui_->curveListPanel->setTopicDemandController(topic_demand_controller_.get());

  // Keep data widgets coherent with catalog removals, whoever triggers them.
  // Each widget prunes its OWN pieces against the live catalog/store. One pass
  // per removal.
  connect(&session_->catalogModel(), &CatalogModel::cleared, this, [this]() { syncWidgetsToCatalog(); });
  connect(&session_->catalogModel(), &CatalogModel::itemsRemoved, this, [this](const QStringList&) {
    syncWidgetsToCatalog();
  });
  connect(session_.get(), &AppSession::datasetsMerged, this, [this](DatasetId anchor, QList<DatasetId> consumed) {
    if (transform_service_ == nullptr) {
      return;
    }
    for (const DatasetId id : consumed) {
      transform_service_->invalidateDataset(id);
    }
    transform_service_->invalidateDataset(anchor);
    transform_service_->ingestFrameTransformsForDataset(anchor);
  });

  connect(ui_->curveListPanel, &CurveListPanel::trashRequested, this, &MainWindow::onCatalogTrashRequested);
  connect(ui_->curveListPanel, &CurveListPanel::removeDatasetsRequested, this, &MainWindow::onRemoveDatasetsRequested);
  connect(ui_->curveListPanel, &CurveListPanel::mergeDatasetsRequested, this, &MainWindow::onMergeDatasetsRequested);
  // The "+" in Custom Series opens the Transform Editor — now provided by the
  // toolbox plugin (the native panel was retired). Launch it like any toolbox.
  connect(ui_->curveListPanel, &CurveListPanel::createCustomSeriesRequested, this, [this]() {
    launchToolbox(u"toolbox-transform-editor"_s);
  });
  // Edit (pencil): open the Transform Editor pre-populated with the selected custom
  // series' saved editor state (stored in the recipe's params_json at create time),
  // so the user can Modify it in place.
  connect(ui_->curveListPanel, &CurveListPanel::editCustomSeriesRequested, this, [this](const QString& catalog_key) {
    // The custom view yields a catalog KEY (same as the delete path); resolve it to
    // the output topic name, then find that transform's saved editor state.
    QString output_name;
    for (const auto& item : session_->catalogModel().items()) {
      if (item.key == catalog_key) {
        output_name = item.topic_name;
        break;
      }
    }
    auto& dp = session_->sessionManager().dataProcessorService();
    QString initial_config;
    for (const auto& recipe : dp.transformRecipes()) {
      const bool owns = std::any_of(recipe.outputs.begin(), recipe.outputs.end(), [&](const std::string& o) {
        return QString::fromStdString(o) == output_name;
      });
      if (owns) {
        initial_config = QString::fromStdString(recipe.params_json);
        break;
      }
    }
    launchToolbox(u"toolbox-transform-editor"_s, initial_config);
  });
  connect(ui_->curveListPanel, &CurveListPanel::deleteCustomSeriesRequested, this, [this](const QString& catalog_key) {
    QString output_name;
    for (const auto& item : session_->catalogModel().items()) {
      if (item.key == catalog_key) {
        output_name = item.topic_name;
        break;
      }
    }
    // Cascade FIRST to any derivative built on top of this one (and its chain):
    // its output name is the input of those, so the transitive resolver finds them.
    // Warns + removes them; cancel aborts (this series is left intact too).
    if (!confirmAndRemoveDependentTransforms({output_name.toStdString()})) {
      return;
    }
    auto& dp = session_->sessionManager().dataProcessorService();
    for (const auto& recipe : dp.transformRecipes()) {
      const bool owns = std::any_of(recipe.outputs.begin(), recipe.outputs.end(), [&](const std::string& o) {
        return QString::fromStdString(o) == output_name;
      });
      if (owns) {
        (void)dp.removeTransform(recipe.key);
        break;
      }
    }
    session_->catalogModel().rebuildFromDatastore();
    ui_->curveListPanel->removeCustomCurve(catalog_key);
  });
  connect(ui_->curveListPanel, &CurveListPanel::clearAllCurvesRequested, this, [this]() {
    if (streaming_manager_ != nullptr && streaming_manager_->hasActiveSession()) {
      streaming_manager_->stopAllAndWait(tr("dataset removed"));
      active_streaming_dataset_id_ = 0;
      streaming_playback_seeded_ = false;
    }
    // Confirmed full wipe: free all objects before mutating the catalog, so the
    // cleared() subscription sees the topics gone and resets the object viewers.
    // Eviction lives at the confirmed-removal site, not in clearAll(), so the
    // low-level catalog op stays safe for speculative callers. Keep
    // lastLoadedSource for the quick-reload path (#99).
    session_->sessionManager().clearAllObjects();
    // TF buffers derive from the just-evicted objects; drop them with the data.
    if (transform_service_ != nullptr) {
      transform_service_->invalidateAll();
    }
    // REAL delete: drop the catalog (no tombstone — nothing is kept) so widgets tear down
    // their curve adapters via cleared(), THEN erase every dataset's scalar storage from
    // the engine (adapters gone, so DataEngine::removeDataset's invalidate-first contract
    // holds). Without the engine erase a later reload would reattach to the emptied shells.
    session_->catalogModel().clearAll(/*tombstone=*/false);
    SessionManager& session_manager = session_->sessionManager();
    for (const DatasetId id : session_manager.dataEngine().listDatasets()) {
      session_manager.removeDataset(id);
    }
    resetUndoHistory();
  });

  QSettings settings;
  // Right-toolbar global view toggles persisted across sessions. Buttons
  // themselves are created later by buildGlobalToolbar(); we load state
  // first so the buttons can pick up the correct initial check state.
  show_points_ = settings.value(u"MainWindow.buttonShowpoint"_s, true).toBool();
  activate_grid_ = settings.value(u"MainWindow.buttonActivateGrid"_s, false).toBool();
  dots_ = settings.value(u"MainWindow.buttonDots"_s, false).toBool();
  tracker_info_ = static_cast<CurveTracker::Parameter>(
      settings.value(u"MainWindow.timeTrackerSetting"_s, static_cast<int>(CurveTracker::kValue)).toInt());
  keep_ratio_ = settings.value(u"MainWindow.buttonRatio"_s, true).toBool();
  legend_status_ = static_cast<LegendStatus>(
      settings.value(u"MainWindow.legendStatus"_s, static_cast<int>(LegendStatus::kHidden)).toInt());

  // Push the just-loaded toggle states into every plot already created by
  // wireExistingPlots(). New plots will pick this up via onPlotAdded.
  forEachPlot([this](PlotWidget* plot) { applyGlobalToggles(plot); });
  applyShowPointsTo2DWidgets();

  // Panel toggle buttons in the tab strip drive shell-level visibility
  // for the left column, the timeline strip, and the right toolbar.
  // The buttons are NOT checkable — visibility is communicated through
  // the icon glyph itself (filled = panel visible, outlined = hidden),
  // swapped via the "iconPath" dynamic property + applyIcons() so
  // theme-tinting flows through one codepath. Fixed launch layout, not persisted:
  // left panel visible, right panel and bottom timeline strip collapsed.
  for (const PanelToggle& toggle : panelToggles(ui_)) {
    const bool visible = (toggle.target == ui_->leftColumn);
    toggle.target->setVisible(visible);
    toggle.button->setCheckable(false);
    toggle.button->setProperty("iconPath", QString::fromLatin1(visible ? toggle.icon_path_on : toggle.icon_path_off));
    // The bottom panel's open/closed height constraints are applied centrally by
    // applyBottomPanelConstraints() — driven here by the chrome-metrics pass
    // emitted later in the constructor (which also resolves the playback height).
    QPushButton* button = toggle.button;
    QWidget* target = toggle.target;
    const QString icon_on = QString::fromLatin1(toggle.icon_path_on);
    const QString icon_off = QString::fromLatin1(toggle.icon_path_off);
    connect(button, &QPushButton::clicked, this, [this, button, target, icon_on, icon_off]() {
      const bool now_visible = !target->isVisible();
      target->setVisible(now_visible);
      const QString icon = now_visible ? icon_on : icon_off;
      button->setProperty("iconPath", icon);
      button->setIcon(loadSvg(icon, theme_->currentTheme()));
      // Bottom-panel toggle: collapse/restore the splitter so the playback stays
      // glued to the top with no empty gap when folded, and the strip's previous
      // expanded height is preserved across fold cycles. applyBottomPanelConstraints
      // owns the open-min / closed-lock invariants.
      if (target == ui_->timelineStrip) {
        const QList<int> sizes = ui_->timelineSplitter->sizes();
        const int total = sizes[0] + sizes[1];
        if (now_visible) {
          applyBottomPanelConstraints();  // lifts the cap, pins the open minimum
          const int floor = ui_->bottomPanel->minimumHeight();
          const int expanded = std::max(QSettings().value(kPanelBottomExpandedKey, sizes[1]).toInt(), floor);
          ui_->timelineSplitter->setSizes({total - expanded, expanded});
          // Fit all source bars horizontally each time the strip is opened, once the
          // strip's new geometry has settled (deferred a tick so the view is sized).
          if (source_timeline_ != nullptr) {
            QTimer::singleShot(0, source_timeline_, [tl = source_timeline_]() { tl->zoomToFit(); });
          }
        } else {
          QSettings().setValue(kPanelBottomExpandedKey, sizes[1]);
          applyBottomPanelConstraints();  // pins the panel to the playback bar (drag can't reopen)
        }
      }
    });
  }

  applyIcons(theme_->currentTheme());

  // Apply QSS + force ToolTip palette to the theme. QToolTip's background
  // is decided by both QSS and QPalette::ToolTipBase; setting only the
  // QSS sometimes leaves the platform palette's tooltip colour in place
  // (showing as a yellow / brown box). Setting the palette here and on
  // every theme change keeps them in sync.
  auto apply_theme_chrome = [this]() {
    qApp->setStyleSheet(theme_->expandedQss());
    const bool light = theme_->currentTheme().contains("light");
    const QColor tip_bg = light ? QColor(0xF5, 0xF5, 0xF5) : QColor(0x44, 0x44, 0x44);
    const QColor tip_fg = light ? QColor(0x11, 0x11, 0x11) : QColor(0xF0, 0xF0, 0xF0);
    QPalette p = qApp->palette();
    p.setColor(QPalette::ToolTipBase, tip_bg);
    p.setColor(QPalette::ToolTipText, tip_fg);
    qApp->setPalette(p);
    // QToolTip keeps its own palette separate from QApplication's; in
    // Qt 6 it's this one that wins for the actual tooltip widget.
    QPalette tp = QToolTip::palette();
    tp.setColor(QPalette::ToolTipBase, tip_bg);
    tp.setColor(QPalette::ToolTipText, tip_fg);
    tp.setColor(QPalette::Window, tip_bg);
    tp.setColor(QPalette::WindowText, tip_fg);
    QToolTip::setPalette(tp);
  };

  connect(theme_.get(), &Theme::themeChanged, this, &MainWindow::onThemeChanged, Qt::QueuedConnection);
  connect(theme_.get(), &Theme::qssChanged, this, apply_theme_chrome);
  connect(this, &MainWindow::stylesheetChanged, ui_->leftPanel, &LeftPanel::onStylesheetChanged);
  connect(this, &MainWindow::stylesheetChanged, ui_->curveListPanel, &CurveListPanel::onStylesheetChanged);
  connect(this, &MainWindow::stylesheetChanged, ui_->timelineWidget, &TimelineWidget::onStylesheetChanged);
  connect(this, &MainWindow::stylesheetChanged, ui_->tabbedPlotWidget, &TabbedPlotWidget::onStylesheetChanged);
  connect(this, &MainWindow::stylesheetChanged, title_bar_, &TitleBar::onStylesheetChanged);

  // Icon-metrics broadcast — runs alongside the theme broadcast. Each
  // listener caches both values and re-runs its applyIcons pass. The
  // tabbedPlotWidget is not currently in scope; see
  // docs/superpowers/specs/2026-05-15-icon-size-preference-design.md
  // ("panel-toggle buttons out of scope").
  connect(this, &MainWindow::chromeMetricsChanged, ui_->leftPanel, &LeftPanel::onChromeMetricsChanged);
  connect(this, &MainWindow::chromeMetricsChanged, ui_->curveListPanel, &CurveListPanel::onChromeMetricsChanged);
  connect(this, &MainWindow::chromeMetricsChanged, ui_->timelineWidget, &TimelineWidget::onChromeMetricsChanged);
  connect(this, &MainWindow::chromeMetricsChanged, ui_->tabbedPlotWidget, &TabbedPlotWidget::onChromeMetricsChanged);
  connect(this, &MainWindow::chromeMetricsChanged, title_bar_, &TitleBar::onChromeMetricsChanged);

  apply_theme_chrome();

  // Reflow chrome dimensions after each widget has re-applied its
  // icons — runs LAST so the per-widget slots above have already
  // updated sizeHint(). The timeline strip's minimum height tracks
  // its content; the local toolbar's minimum width fits six chrome
  // buttons on one row plus a small flow-layout margin.
  //
  // The splitter and the bottomPanel cap need explicit nudges: Qt's
  // QSplitter doesn't re-layout on a child's min-height change, and
  // when the bottom strip is hidden we clamp bottomPanel's max-height
  // to the playback's old min-height during toggle. Without these
  // pushes, the playback bar grows upward only and its icons get
  // clipped against bottomPanel's stale bottom edge.
  connect(this, &MainWindow::chromeMetricsChanged, this, [this](const ChromeMetrics& metrics) {
    // The .ui pins timelineWidget to maximumHeight 24 — override both
    // min and max so the playback bar grows with its buttons. sizePolicy
    // is Preferred-Fixed in the .ui, so the widget's height stays equal
    // to sizeHint regardless of available space.
    const int playback_height = ui_->timelineWidget->sizeHint().height();
    ui_->timelineWidget->setMinimumHeight(playback_height);
    ui_->timelineWidget->setMaximumHeight(playback_height);

    const int band_extent = (metrics.icon_size + metrics.icon_padding) + (2 * metrics.layout_padding);
    // Local toolbar fits six chrome buttons on one row at the band extent, plus
    // a small margin for the FlowLayout to wrap — but never narrower than the
    // 250-px side-panel floor.
    ui_->localToolbarWidget->setMinimumWidth(qMax((6 * band_extent) + 6, 250));

    // Re-apply the bottom panel's open/closed height constraints against the
    // freshly-resolved playback height (the QSplitter caches stale minimums, so
    // this also re-floors the strip and re-pins the closed lock).
    applyBottomPanelConstraints();
    // The playback controls' size feeds the slider's start x; re-align the name
    // column separator to it (no-op until the playback bar is laid out, post-show).
    alignNameColumnToPlayback();
  });

  // Dev-only widget inspector: Ctrl+Shift+D = pesticide outline overlay,
  // Ctrl+Shift+Q = QSS debug border layer. Drop the include + this call +
  // DebugUi.{cpp,h} (plus the CMakeLists entries) to remove the feature.
  DebugUi::installInto(this, theme_.get());

  auto& playback = session_->playbackEngine();
  // Start in the empty state: no data loaded means an empty range, so the
  // transport renders disabled until the first dataset arrives (same state a
  // full removal returns to).
  playback.setRangeAndCurrentTime(displayRange(0.0, 0.0), displaySeconds(0.0));
  ui_->timelineWidget->setPlaybackEngine(&playback);
  // Cap the cursor-move fan-out at ~30 Hz (kTrackerBroadcastIntervalMs). currentTimeChanged
  // fires on playback ticks, scrubbing, and seeks — all of which can exceed 30 Hz (a fast
  // scrub emits at mouse-move rate). The coalescer runs broadcastTrackerTime(latest) on the
  // leading edge and once more on the trailing edge, dropping the intermediate cursor times.
  tracker_broadcast_trigger_ = std::make_unique<CoalescingTrigger>(
      kTrackerBroadcastIntervalMs, [this]() { broadcastTrackerTimeToVisible(pending_tracker_time_); });
  connect(&playback, &PlaybackEngine::currentTimeChanged, this, [this](double time) {
    pending_tracker_time_ = time;
    tracker_broadcast_trigger_->request();
  });
  // Frame change (the "Use time offset" toggle today, any future re-base): the
  // blue reference line stores a frame-invariant instant, so re-project it
  // through the new offset and re-push — keeping it pinned to its instant rather
  // than stranded off the re-fitted axis.
  connect(&session_->sessionManager(), qOverload<>(&SessionManager::displayOffsetChanged), this, [this]() {
    // The global frame moved: re-project the reference and re-push it to the plots
    // and (re-bridged into the Timeline frame) the Source Timeline.
    broadcastReferenceLine();
  });

  // Mount the multi-track Source Timeline into the reserved bottom strip and
  // bind it to the runtime. timelineStrip is an empty native widget in the .ui;
  // give it a layout and host the widget. The existing bottom-panel toggle +
  // splitter target timelineStrip, so show/hide/resize come for free.
  auto* source_timeline = new PJ::Timeline(ui_->timelineStrip);
  source_timeline_ = source_timeline;
  auto* strip_layout = new QVBoxLayout(ui_->timelineStrip);
  strip_layout->setContentsMargins(0, 0, 0, 0);
  strip_layout->addWidget(source_timeline);
  source_timeline_controller_ = new PJ::SourceTimelineController(source_timeline, session_.get(), this);
  // Remember the user's name-column width so it sticks across rebuilds / panel
  // toggles (alignNameColumnToPlayback re-applies it over the playback-aligned
  // floor) and is persisted into layouts.
  connect(source_timeline, &PJ::Timeline::nameColumnWidthChanged, this, [this](int width_px) {
    timeline_name_column_width_ = width_px;
  });
  // Timeline auto-zoom is always on (no longer a user preference): a newly-loaded
  // file (and each alignment) zooms the timeline to its full extent.
  source_timeline->setAutoZoomEnabled(true);
  // Align rail on the right edge of the timeline panel; binds its buttons to the
  // controller above, so it must be built after the controller exists.
  buildTimelineAlignRail();
  // Dragging the timeline's blue reference needle repositions the reference and
  // re-renders every plot's delta-from-reference (mirrors the toolbar toggle).
  connect(source_timeline, &PJ::Timeline::referenceLineMoved, this, [this](double display_seconds) {
    // The needle reports a playback-frame display-seconds position (the widget undoes
    // its time-frame offset before emitting); store it frame-invariantly as an
    // absolute instant through the representative's full displayOffset (source +
    // global), so it survives offset changes, then re-push the position to plots.
    reference_instant_ = toAbsolute(
        fromAxisDouble(display_seconds), session_->sessionManager().displayOffset(representativeDatasetId()));
    const std::optional<double> ref_sec = referenceDisplaySeconds();
    forEachPlot([ref_sec](PlotWidget* plot) { plot->setReferenceLine(ref_sec); });
  });

  streaming_manager_ = std::make_unique<StreamingSourceManager>(
      session_->sessionManager(), session_->extensionCatalog(), session_->catalogModel(),
      session_->topicDemandTracker(), this, this);
  // Interactive placeholder drops stage pending binds OUTSIDE any layout restore
  // (which has its own restore-scoped duplicate of this connection); flush them
  // whenever the catalog gains topics. Idempotent — an entry binds once and the
  // no-entries early-return makes the steady-state cost nil.
  connect(&session_->catalogModel(), &CatalogModel::itemsAdded, this, &MainWindow::flushPendingCurveBindings);
  connect(
      ui_->leftPanel, &LeftPanel::streamingSourceChanged, streaming_manager_.get(),
      &StreamingSourceManager::onSourceChanged);
  connect(
      ui_->leftPanel, &LeftPanel::streamingBufferChanged, streaming_manager_.get(),
      &StreamingSourceManager::onBufferChanged);
  connect(
      ui_->leftPanel, &LeftPanel::streamingStartRequested, streaming_manager_.get(),
      &StreamingSourceManager::onStartRequested);
  connect(
      ui_->leftPanel, &LeftPanel::streamingPauseToggled, streaming_manager_.get(),
      &StreamingSourceManager::onPauseToggled);

  const auto refresh_streaming_seek_lock = [this]() {
    if (session_ == nullptr) {
      return;
    }
    // While a stream is live AND playback is playing, the cursor stays glued to the
    // tip (held there by PlaybackEngine::onTick + each live ingest) and the user must
    // not be able to drag it off: grey the playback slider and put the Source Timeline
    // read-only (scene edits + needle seek). Pausing (stream still on) clears the lock
    // so the retained window can be scrubbed/realigned.
    const bool lock = active_streaming_dataset_id_ != 0 && session_->playbackEngine().isPlaying();
    ui_->timelineWidget->setSeekLocked(lock);
    setSourceTimelineStreamingLock(lock);
  };
  const auto snap_streaming_playback_to_live_edge = [this]() -> bool {
    if (active_streaming_dataset_id_ == 0 || session_ == nullptr) {
      return false;
    }
    if (const auto range = session_->sessionManager().datasetDisplayRange(active_streaming_dataset_id_);
        range.has_value()) {
      streaming_playback_seeded_ = true;
      session_->playbackEngine().setRangeAndCurrentTime(*range, range->max);
      pending_tracker_time_ = toAxisDouble(range->max);
      broadcastTrackerTimeToVisible(pending_tracker_time_);
      return true;
    }
    return false;
  };

  // Streaming → playback range wiring. The slider range is scoped to the
  // active streaming dataset only — unioning with every catalog-visible
  // dataset (as AppSession::seedPlaybackFromSession does for file loads) can
  // stretch the slider across unrelated historical data, leaving the actual
  // streamed window as a sliver where intermediate scrub positions resolve to
  // "before first entry" or "at the live edge" with nothing in between.
  //
  // The slider starts at the placeholder range, then the first live ingest with a
  // real range seeds it from the active streaming dataset. A curve/object drop can
  // still force the same seed immediately, but it is not required: streaming data
  // itself must be enough to advance playback.
  //
  // Timeline playback state is the follow-live switch. While playing, the slider
  // is locked to the live edge and the engine is held at rangeMax between ingest
  // ticks instead of auto-pausing at the end. Pausing playback unlocks scrubbing
  // within the retained window; pressing play snaps back to the newest sample.
  connect(
      streaming_manager_.get(), &StreamingSourceManager::streamStarted, this,
      [this, refresh_streaming_seek_lock](DatasetId id) {
        active_streaming_dataset_id_ = id;
        session_->setActiveStreamingDataset(id);  // scope recomputeRange to the live tip while playing
        streaming_playback_seeded_ = false;
        auto& engine = session_->playbackEngine();
        engine.setHoldAtRangeMax(true);
        engine.setCurrentTime(engine.rangeMax());
        engine.play();
        refresh_streaming_seek_lock();
      });
  connect(
      streaming_manager_.get(), &StreamingSourceManager::streamStopped, this,
      [this, refresh_streaming_seek_lock](DatasetId id, const QString&) {
        if (active_streaming_dataset_id_ == id) {
          active_streaming_dataset_id_ = 0;
          session_->setActiveStreamingDataset(0);
          streaming_playback_seeded_ = false;
          session_->playbackEngine().setHoldAtRangeMax(false);
          refresh_streaming_seek_lock();
        }
      });
  connect(
      &playback, &PlaybackEngine::playingChanged, this,
      [this, snap_streaming_playback_to_live_edge, refresh_streaming_seek_lock](bool playing) {
        if (active_streaming_dataset_id_ != 0 && playing) {
          session_->playbackEngine().setHoldAtRangeMax(true);
          (void)snap_streaming_playback_to_live_edge();
        }
        refresh_streaming_seek_lock();
      });
  // Bound the 3D TF buffer's history for live-streaming datasets in step with
  // the ObjectStore retention window, so a long streaming session doesn't retain
  // every TF sample forever (H.11). File loads keep the buffer's kKeepAll
  // default. The factor keeps TF resolvable slightly past the oldest scrubbable
  // object entry — the store and the TF buffer trim on independent ticks, so the
  // headroom avoids a TF lookup failing on a frame whose object still exists.
  if (transform_service_ != nullptr) {
    connect(
        streaming_manager_.get(), &StreamingSourceManager::retentionWindowChanged, this,
        [this](DatasetId id, qint64 window_ns) {
          constexpr int kTfWindowHeadroomFactor = 2;
          transform_service_->setLiveCacheWindow(id, std::chrono::nanoseconds(kTfWindowHeadroomFactor * window_ns));
        });
  }
  connect(
      &session_->sessionManager(), &SessionManager::samplesIngested, this,
      [this, refresh_streaming_seek_lock](const QVector<TopicId>&, bool live) {
        auto& engine = session_->playbackEngine();
        if (live) {
          // Streaming: follow the live edge (range grows, cursor tracks newest).
          if (active_streaming_dataset_id_ == 0) {
            return;
          }
          if (const auto range = session_->sessionManager().datasetDisplayRange(active_streaming_dataset_id_);
              range.has_value()) {
            streaming_playback_seeded_ = true;
            if (engine.isPlaying()) {
              engine.setRangeAndCurrentTime(*range, range->max);
              pending_tracker_time_ = toAxisDouble(range->max);
              broadcastTrackerTimeToVisible(pending_tracker_time_);
            } else {
              engine.setRange(*range);
            }
            refresh_streaming_seek_lock();
          }
          return;
        }
        // Non-live (file) ingest: grow the playback range from the UNION of every
        // loaded dataset (recomputeRange) so the timeline + auto-fitting plots reveal
        // data as it arrives. Using a SINGLE dataset's range here was a bug: with a
        // multi-file (multi-select) load, each file's range excludes the earlier ones,
        // so setRange clamps the cursor FORWARD to each new file's start. The union
        // keeps the seeded cursor in range. The authoritative cursor is still set once
        // on completion by AppSession::seedPlaybackFromSession (onFileLoaded).
        session_->recomputeRange();
      });

  // Populate the combo only after the manager is wired so the initial
  // streamingSourceChanged emission from setStreamingSources() reaches it.
  refreshStreamingCombo();
  connect(
      &session_->extensionCatalog(), &ExtensionCatalogService::catalogChanged, this,
      &MainWindow::refreshStreamingCombo);

  // Populate the Cloud page from the catalog now and on every catalog change.
  ui_->leftPanel->populateCloudToolboxes(session_->extensionCatalog().toolboxes());
  connect(&session_->extensionCatalog(), &ExtensionCatalogService::catalogChanged, this, [this]() {
    ui_->leftPanel->populateCloudToolboxes(session_->extensionCatalog().toolboxes());
  });

  file_loader_ = std::make_unique<FileLoader>(
      session_->sessionManager(), session_->extensionCatalog(), session_->catalogModel(), this);
  file_loader_->setTransformService(transform_service_.get());
  // Passing the MainWindow as the metrics source primes the file dialog's
  // toolbar icon size and keeps it in step via chromeMetricsChanged. Injected
  // here so FileLoader itself never links MainWindow (keeps it testable).
  file_loader_->setFilePicker(
      [](QWidget* dialog_parent, const QString& caption, const QString& dir, const QString& filter) {
        auto* metrics_source = dialog_parent != nullptr ? qobject_cast<MainWindow*>(dialog_parent->window()) : nullptr;
        return FileDialog::getOpenFileNames(dialog_parent, caption, dir, filter, metrics_source);
      });
  connect(ui_->leftPanel, &LeftPanel::loadDataRequested, this, &MainWindow::onLoadDataRequested);
  connect(ui_->leftPanel, &LeftPanel::reloadDataRequested, this, &MainWindow::onReloadDataRequested);
  connect(ui_->leftPanel, &LeftPanel::cloudToolboxRequested, this, [this](const QString& id) { launchToolbox(id); });
  connect(file_loader_.get(), &FileLoader::fileLoaded, this, &MainWindow::onFileLoaded);
  // Track successful loads for the recent-files popup.
  connect(
      file_loader_.get(), &FileLoader::fileLoaded, this,
      [this](
          const QString& path, const QString& /*prefix*/, const QString& /*plugin_id*/,
          const QString& /*plugin_config_json*/) {
        QSettings recent_settings;
        QStringList recent = recent_settings.value(u"File/recent"_s).toStringList();
        recent.removeAll(path);
        recent.prepend(path);
        while (recent.size() > kMaxRecentEntries) {
          recent.removeLast();
        }
        recent_settings.setValue(u"File/recent"_s, recent);
        ui_->leftPanel->setRecentEnabled(true);
      });
  // Recent files reopen through the normal load flow, including the dialog.
  connect(ui_->leftPanel, &LeftPanel::recentFileSelected, this, [this](const QString& path) {
    file_loader_->loadFile(path, this);
  });
  // Recent layouts load through the same validated path as the (removed) File
  // menu submenu — onLoadRecentLayout checks existence and prunes dead entries.
  connect(ui_->leftPanel, &LeftPanel::recentLayoutSelected, this, &MainWindow::onLoadRecentLayout);
  // Enable the popup immediately when prior sessions recorded recent files or
  // layouts (either section is enough to make the popup worth showing).
  const bool had_recent = !settings.value(u"File/recent"_s).toStringList().isEmpty() ||
                          !settings.value(u"Layout/recent"_s).toStringList().isEmpty();
  ui_->leftPanel->setRecentEnabled(had_recent);

  // Title-bar load progress strip — the non-modal replacement for the import
  // dialog on single-instance loads. Shown after a 500 ms delay so quick loads
  // never flash it; hidden 1 s after the load queue drains. The single stop
  // button opens a confirmation dialog that routes to FileLoader::cancelCurrent.
  ingest_progress_ = new IngestProgressWidget(this);
  ingest_progress_->setActive(false);
  // One icon-only stop button on the left. The three real choices (keep /
  // discard / resume) live in the confirmation dialog below, so the button
  // itself is meaning-light — its tooltip just invites a stop.
  ingest_progress_->setPrimaryButton({}, u":/resources/svg/cancel.svg"_s, tr("Stop loading…"));
  title_bar_->setCenterWidget(ingest_progress_);
  ingest_show_timer_ = new QTimer(this);
  ingest_show_timer_->setSingleShot(true);
  connect(ingest_show_timer_, &QTimer::timeout, this, [this]() { ingest_progress_->setActive(true); });
  connect(ingest_progress_, &IngestProgressWidget::actionRequested, this, [this](IngestProgressWidget::Action) {
    // Stop pressed → ask what to do with the in-progress load. The worker keeps
    // loading (and the bar keeps updating) while this modal dialog is up.
    if (!file_loader_->isBusy()) {
      // The load already finished (e.g. clicked during the post-completion
      // linger): there is nothing to stop, so just dismiss the strip.
      ingest_progress_->setActive(false);
      return;
    }
    // Instance (not the static question()) so it can be closed programmatically.
    MessageBox dialog(this);
    dialog.setTitle(tr("Stop loading?"));
    dialog.setText(tr("This data is still loading. Keep what has loaded so far, remove all of it, or keep loading?"));
    dialog.addButton(tr("Remove All"), MessageBox::kDestructiveRole);  // index 0 — stop and discard
    dialog.addButton(tr("Stop and Keep"), MessageBox::kPrimaryRole);   // index 1 — stop, keep partial
    dialog.addButton(tr("Cancel"), MessageBox::kCancelRole);           // index 2 — resume loading
    // If the load completes while the dialog is open, its premise is gone and
    // its actions would be misleading no-ops, so auto-dismiss it as a no-op.
    const QMetaObject::Connection drained =
        connect(file_loader_.get(), &FileLoader::queueDrained, &dialog, [&dialog]() { dialog.reject(); });
    dialog.exec();
    QObject::disconnect(drained);
    switch (dialog.clickedIndex()) {
      case 0:
        file_loader_->cancelCurrent(/*keep_partial=*/false);  // Remove All — stop and discard
        break;
      case 1:
        file_loader_->cancelCurrent(/*keep_partial=*/true);  // Stop and Keep
        break;
      default:
        break;  // Cancel, or auto-dismissed on completion — leave the load alone
    }
  });
  connect(
      file_loader_.get(), &FileLoader::ingestStarted, this,
      [this](const QString& title, int index, int total, bool /*determinate*/) {
        ingest_progress_->setTitle(title);
        ingest_progress_->setCounterText(total > 1 ? u"%1/%2"_s.arg(index).arg(total) : QString());
        ingest_progress_->setRange(0, 0);  // busy until the first determinate progress tick
        ingest_show_timer_->start(500);
      });
  connect(file_loader_.get(), &FileLoader::ingestProgress, this, [this](int current, int maximum) {
    ingest_progress_->setRange(0, maximum);  // maximum 0 keeps the bar in busy/indeterminate mode
    ingest_progress_->setValue(current);
  });
  connect(file_loader_.get(), &FileLoader::queueDrained, this, [this]() {
    ingest_show_timer_->stop();
    QTimer::singleShot(1000, this, [this]() { ingest_progress_->setActive(false); });
  });

  connect(ui_->actionMarketplace, &QAction::triggered, this, &MainWindow::onOpenMarketplace);
  connect(ui_->actionExit, &QAction::triggered, this, &QWidget::close);

  // Undo/Redo apply to plot-layout snapshots. They are keyboard-only
  // (Ctrl+Z / Ctrl+Y) and deliberately NOT in any menu. Registering them on
  // the main window via addAction is what makes the shortcuts fire window-wide;
  // an action that lives only in a popup menu (as these used to) never
  // activates its shortcut while that menu is closed.
  undo_action_ = new QAction(tr("Undo"), this);
  undo_action_->setShortcuts(QKeySequence::Undo);
  connect(undo_action_, &QAction::triggered, this, &MainWindow::onUndo);
  addAction(undo_action_);
  redo_action_ = new QAction(tr("Redo"), this);
  // The platform's standard redo set (Ctrl+Y and/or Ctrl+Shift+Z depending on
  // desktop-theme detection) plus both explicitly, so redo works the same
  // everywhere regardless of how Qt resolves QKeySequence::Redo.
  QList<QKeySequence> redo_shortcuts = QKeySequence::keyBindings(QKeySequence::Redo);
  redo_shortcuts.append(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z));
  redo_shortcuts.append(QKeySequence(Qt::CTRL | Qt::Key_Y));
  redo_action_->setShortcuts(redo_shortcuts);
  connect(redo_action_, &QAction::triggered, this, &MainWindow::onRedo);
  addAction(redo_action_);

  // Plot-layout changes from TabbedPlotWidget feed the undo stack. Wrapped in a lambda
  // because onUndoableChange now takes a (defaulted) force_new_state arg, and Qt's
  // function-pointer connect rejects a slot whose arity exceeds the signal's.
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::undoableChange, this, [this] { onUndoableChange(); });

  // Apply the persisted "align starts" frame before the toolbar is built so its
  // button seeds from the live SessionManager state. Default OFF: datasets show at
  // their natural relative times so the Source Timeline reveals their offsets and
  // the user aligns them (manually, or via this toggle). No data yet, so this is a
  // no-op at startup; toggling it later bulk-writes each dataset's offset.
  session_->sessionManager().setUseTimeOffset(QSettings().value(u"MainWindow.useTimeOffset"_s, false).toBool());

  // Global column on the right of the plot area — Chart + Legend icons,
  // pinned at 24 px wide, never collapses. Always visible regardless of
  // the local panel's toggle state.
  buildGlobalToolbar();

  // Right sidepanel content switches by focused widget type via a
  // QStackedWidget. Page 0 is the plot-config UI (Curve Width / Style
  // strips + CurveEditor). Pages 1 and 2 are per-family placeholders
  // (Scene2D, Scene3D); onDockFocused() picks the active page.
  auto* outer_layout = qobject_cast<QVBoxLayout*>(ui_->localToolbarWidget->layout());
  outer_layout->setSpacing(0);
  outer_layout->setContentsMargins(0, 0, 0, 0);
  right_panel_stack_ = new QStackedWidget(ui_->localToolbarWidget);
  outer_layout->addWidget(right_panel_stack_, /*stretch=*/1);

  plot_config_page_ = new QWidget(right_panel_stack_);
  auto* plot_config_layout = new QVBoxLayout(plot_config_page_);
  plot_config_layout->setSpacing(0);
  plot_config_layout->setContentsMargins(0, 0, 0, 0);
  right_panel_stack_->addWidget(plot_config_page_);

  auto make_placeholder = [this](const QString& text) {
    auto* page = new QWidget(right_panel_stack_);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addStretch(1);
    auto* label = new QLabel(text, page);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    layout->addWidget(label);
    layout->addStretch(1);
    return page;
  };
  scene2d_config_panel_ = new Scene2DConfigPanel(right_panel_stack_);
  scene2d_config_page_ = scene2d_config_panel_;
  scene2d_config_panel_->onStylesheetChanged(theme_->currentTheme());
  connect(this, &MainWindow::stylesheetChanged, scene2d_config_panel_, &Scene2DConfigPanel::onStylesheetChanged);
  scene3d_config_panel_ = new Scene3DConfigPanel(right_panel_stack_);
  scene3d_config_page_ = scene3d_config_panel_;
  // Push the active theme into the panel so its row icons paint in the
  // right ink on first show, then keep them tracking subsequent theme
  // toggles via the standard stylesheetChanged signal.
  scene3d_config_panel_->onStylesheetChanged(theme_->currentTheme());
  connect(this, &MainWindow::stylesheetChanged, scene3d_config_panel_, &Scene3DConfigPanel::onStylesheetChanged);
  empty_dock_page_ = make_placeholder(tr("No widget selected"));
  right_panel_stack_->addWidget(scene2d_config_page_);
  right_panel_stack_->addWidget(scene3d_config_page_);
  right_panel_stack_->addWidget(empty_dock_page_);
  right_panel_stack_->setCurrentWidget(plot_config_page_);

  // Local panel to the right of the global column — Curve Width / Curve
  // Style header bands and their FlowLayout icon strips, followed by the
  // per-curve CurveEditor. Hidden by the "Toggle Right Panel" button;
  // its width snaps/folds as the user drags the splitter handle.
  buildLocalToolbar();

  // Push the loaded metrics through every chrome-aware widget so
  // anything that picked up .ui defaults — including the global and local
  // toolbar columns, which install their own listeners inside the build*
  // functions above — re-renders at the saved values.
  emit chromeMetricsChanged(chrome_metrics_);

  // CurveEditor lives below the icon strips inside the plot-config
  // page; same right-panel toggle controls visibility (the stack is
  // a child of localToolbarWidget). Rebinds to the active plot on tab
  // changes.
  curve_editor_ = new CurveEditor(plot_config_page_);
  plot_config_layout->addWidget(curve_editor_, /*stretch=*/1);
  // Trailing stretch keeps the icon strips anchored at the top of the
  // panel when the CurveEditor below is hidden. CurveEditor's own
  // stretch factor (1) outweighs the spacer's default (0), so while
  // the editor is visible it still fills the remaining vertical space.
  plot_config_layout->addStretch(0);
  curve_editor_->onStylesheetChanged(theme_->currentTheme());
  curve_editor_->onChromeMetricsChanged(chrome_metrics_);
  connect(this, &MainWindow::stylesheetChanged, curve_editor_, &CurveEditor::onStylesheetChanged);
  connect(this, &MainWindow::chromeMetricsChanged, curve_editor_, &CurveEditor::onChromeMetricsChanged);
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::currentTabChanged, this, [this](PlotDocker* /*docker*/) {
    // Switching tabs doesn't emit ADS focus, so drive the right panel (and the
    // editor binding it subsumes) from the new tab's focused dock.
    onDockFocused(activeFocusedDock());
    // Catch-up for the visibility gate: the per-tick fan-out skips inactive tabs,
    // so the tab being switched TO missed every cursor move while hidden — and
    // currentTimeChanged only fires on a time CHANGE, not on a tab switch. Re-seed
    // the now-visible tab with the current cursor so its plots/scenes aren't stale.
    if (session_ != nullptr) {
      broadcastTrackerTimeToVisible(toAxisDouble(session_->playbackEngine().currentTime()));
    }
  });
  bindEditorToPlot(firstPlotOfActiveTab());

  pushInitialUndoState();
  updateUndoRedoActions();
}

IDataWidget* MainWindow::makeSceneDock(const QString& kind, QWidget* parent) {
  // Single construct + wire site for object-widget docks, shared by the drop
  // and layout-restore paths. Wiring (session / transform service / theme) is
  // identical regardless of how the dock is later populated.
  if (kind == u"scene3d"_s) {
    auto* widget = new Scene3DDockWidget(parent);
    widget->setSessionManager(&session_->sessionManager());
    widget->setTransformService(transform_service_.get());
    widget->setSettings(app_settings_.get());
    topic_demand_controller_->registerSceneDock(widget);
    // Seed the resolver's per-source remembered-roots map + auto search roots
    // from the most recent load. Single-path approximation (the dock's own
    // dataset may differ in a multi-file session); see setSourcePath's doc.
    if (const auto src = session_->sessionManager().lastLoadedSource(); src.has_value()) {
      widget->setSourcePath(src->path);
    }
    // Apply the persisted scene controls once the lazily-created view exists, so
    // a dock created by layout restore (never bound to the panel) still matches
    // the shared look. Connect for the deferred view, and apply now if it is
    // already realized (M.3). The QSettings schema stays owned by the panel.
    if (scene3d_config_panel_ != nullptr) {
      auto* panel = scene3d_config_panel_;
      connect(widget, &Scene3DDockWidget::sceneViewReady, panel, [panel, widget]() {
        panel->applySceneControlsTo(widget);
      });
      if (widget->sceneView() != nullptr) {
        panel->applySceneControlsTo(widget);
      }
    }
    // No theme push needed: SceneViewWidget derives dark/light from its own
    // palette luminance and repaints on QEvent::PaletteChange.
    return widget;
  }
  if (kind == u"scene2d"_s) {
    auto* widget = new Scene2DDockWidget(parent);
    widget->setSessionManager(&session_->sessionManager());
    topic_demand_controller_->registerSceneDock(widget);
    return widget;
  }
  return nullptr;
}

IDataWidget* MainWindow::makeSeededEmptyObjectDock(const QString& kind, QWidget* parent) {
  IDataWidget* widget = makeSceneDock(kind, parent);
  if (widget != nullptr) {
    widget->onTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
  }
  return widget;
}

void MainWindow::onObjectFamilyRequested(DockWidget* dock, VisualizationKind family) {
  if (dock == nullptr) {
    return;
  }
  // The shell is the only module that maps a UI family to a concrete object-widget
  // kind. A switch (no default) makes a future enumerator a compile error here —
  // the one site that owns the mapping. Plot never reaches this slot (DockWidget
  // builds plots itself), so it is a defensive no-op.
  QString kind;
  switch (family) {
    case VisualizationKind::kScene2D:
      kind = u"scene2d"_s;
      break;
    case VisualizationKind::kScene3D:
      kind = u"scene3d"_s;
      break;
    case VisualizationKind::kPlot:
      return;
  }
  IDataWidget* widget = makeSeededEmptyObjectDock(kind, dock);
  if (widget == nullptr) {
    // Both scene kinds are valid, so this is a programming error rather than a
    // user-facing one — log it instead of silently leaving the placeholder.
    qCWarning(lcMain) << "onObjectFamilyRequested: could not build scene dock for kind" << kind;
  }
  dock->adoptObjectWidget(widget);  // null still reverts to a usable placeholder
}

void MainWindow::onPlaceholderTopicDropped(
    DockWidget* dock, DatasetId dataset_id, QString topic_name, sdk::BuiltinObjectType object_type) {
  if (dock == nullptr) {
    return;
  }
  if (object_type == sdk::BuiltinObjectType::kNone) {
    if (PlotWidget* plot = dock->plotWidget(); plot != nullptr) {
      topic_demand_controller_->handlePlaceholderPlotDrop(plot, dataset_id, topic_name);
    }
    return;
  }
  IDataWidget* object_widget = dock->objectWidget();
  if (auto* scene_dock = qobject_cast<SceneDockWidget*>(object_widget != nullptr ? object_widget->widget() : nullptr);
      scene_dock != nullptr) {
    topic_demand_controller_->handleSceneDockPlaceholderDrop(scene_dock, dataset_id, topic_name);
  }
}

MainWindow::~MainWindow() {
  // Break the widget-owned pointers to services before session_ destroys
  // the engine — guarantees no late signal dereferences a dead pointer.
  ui_->timelineWidget->setPlaybackEngine(nullptr);
  ui_->curveListPanel->setCatalog(nullptr);
  delete ui_;
}

bool MainWindow::populateTestData() {
  DataEngine& engine = session_->sessionManager().dataEngine();
  auto time_domain_or = engine.createTimeDomain("test_data");
  if (!time_domain_or.has_value()) {
    qCWarning(lcMain) << "createTimeDomain failed:" << QString::fromStdString(time_domain_or.error());
    return false;
  }

  auto dataset_or =
      engine.createDataset(DatasetDescriptor{.source_name = "test-data", .time_domain_id = *time_domain_or});
  if (!dataset_or.has_value()) {
    qCWarning(lcMain) << "createDataset failed:" << QString::fromStdString(dataset_or.error());
    return false;
  }

  DataWriter writer = engine.createWriter();
  auto sin_or = writer.registerScalarSeries(*dataset_or, "test/sin", NumericType::kFloat64);
  auto cos_or = writer.registerScalarSeries(*dataset_or, "test/cos", NumericType::kFloat64);
  if (!sin_or.has_value() || !cos_or.has_value()) {
    qCWarning(lcMain) << "registerScalarSeries failed:"
                      << QString::fromStdString(!sin_or.has_value() ? sin_or.error() : cos_or.error());
    return false;
  }

  for (int index = 0; index < kTestSampleCount; ++index) {
    const double fraction = static_cast<double>(index) / static_cast<double>(kTestSampleCount - 1);
    const double time_sec = fraction * kTestDurationSeconds;
    const auto timestamp = static_cast<Timestamp>(std::llround(time_sec * kNanosecondsPerSecond));
    const double phase = kTwoPi * time_sec;
    writer.appendScalar(*sin_or, timestamp, std::sin(phase));
    writer.appendScalar(*cos_or, timestamp, std::cos(phase));
  }

  const std::vector<TopicId> changed_topics = session_->sessionManager().commitChunks(writer.flushAll());
  if (changed_topics.empty()) {
    qCWarning(lcMain) << "test data commit produced no datastore changes";
    return false;
  }
  session_->catalogModel().rebuildFromDatastore();
  session_->playbackEngine().setRange(displayRange(0.0, kTestDurationSeconds));
  emitDiagnostic(DiagnosticLevel::kInfo, "TestData", "loaded", tr("Loaded test sin/cos data"));
  return true;
}

void MainWindow::onOpenMarketplace() {
  auto& catalog = session_->extensionCatalog();
  MarketplaceWindow dlg(&catalog.extensionManager(), registryUrlFromSettings(), this);
  dlg.resize(900, 600);
  dlg.exec();
  if (dlg.installationsChanged()) {
    catalog.reload();
  }
}

void MainWindow::onLoadDataRequested() {
  file_loader_->openFromDialog(this);
}

void MainWindow::onReloadDataRequested() {
  const auto src = session_->sessionManager().lastLoadedSource();
  if (!src.has_value()) {
    return;
  }
  // Prevent re-entry; success re-enables via onFileLoaded.
  ui_->leftPanel->setReloadEnabled(false);
  LoadHints hints{
      .expected_plugin_id = src->plugin_id,
      .preset_config_json = src->plugin_config_json,
      .skip_dialog = !src->plugin_id.isEmpty() && !src->plugin_config_json.isEmpty(),
  };
  file_loader_->loadFile(src->path, this, hints);
}

void MainWindow::onFileLoaded(
    const QString& path, const QString& prefix, const QString& plugin_id, const QString& plugin_config_json) {
  // MainWindow is the shell that wires load completion to the runtime —
  // recording the source (incl. plugin id + json) and seeding playback are
  // pj_runtime concerns; we just relay. Prefix is empty in v1 until the
  // load dialog gains a prefix input.
  session_->sessionManager().recordLoadedSource(path, prefix, plugin_id, plugin_config_json);
  // Feed the loaded source path to every existing 3D dock so its URDF package
  // resolver can key per-source remembered roots and auto-seed search roots from
  // the file's directory (M.2/M.16). Docks created later pick it up from
  // lastLoadedSource() in makeSceneDock.
  forEachDock([&path](DockWidget* dock) {
    if (dock->objectWidget() == nullptr) {
      return;
    }
    if (auto* scene3d = qobject_cast<Scene3DDockWidget*>(dock->objectWidget()->widget())) {
      scene3d->setSourcePath(path);
    }
  });
  // TODO(embedded-assets): route in-band embedded assets to Scene3D docks once the
  // load path surfaces the extracted asset map (resolver step 0).
  session_->seedPlaybackFromSession();
  // A same-source reload evicts the old dataset's objects AFTER the removeDataset
  // signal fired, so re-run the coherence pass here to reset any 2D viewer still
  // bound to an evicted topic. Idempotent for a first/additive load.
  syncWidgetsToCatalog();
  // Seed every data widget AND the curve-list Value column with the just-set
  // playhead. seedPlaybackFromSession positions the cursor, but currentTimeChanged
  // only fires on an actual change — so a fresh load that lands the cursor where it
  // already was (e.g. 0) would otherwise leave the value column blank until the
  // first scrub. Mirrors PJ3's update2ndColumnValues-after-load.
  broadcastTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
  ui_->leftPanel->setReloadEnabled(true);
}

bool MainWindow::confirmAndRemoveDependentTransforms(const std::vector<std::string>& removed_names) {
  auto& dp = session_->sessionManager().dataProcessorService();
  const auto affected = dp.transformsDependingOn(removed_names);
  if (affected.empty()) {
    return true;  // nothing depends on it — proceed
  }
  QStringList names;
  for (const auto& recipe : affected) {
    for (const auto& out : recipe.outputs) {
      names << QString::fromStdString(out);
    }
  }
  const auto answer = QMessageBox::warning(
      this, tr("Delete derived series?"),
      tr("These derived series depend on what you are deleting and will also be removed:\n\n• %1")
          .arg(names.join(u"\n• "_s)),
      QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
  if (answer != QMessageBox::Ok) {
    return false;  // user cancelled — leave everything intact
  }
  // Remove each derivative: drop its Custom Series entry BY NAME (robust against
  // catalog-key churn — same as PJ3's removeCurve(name)), then the transform node.
  for (const auto& recipe : affected) {
    for (const auto& out : recipe.outputs) {
      ui_->curveListPanel->removeCustomCurveByName(QString::fromStdString(out));
    }
    (void)dp.removeTransform(recipe.key);
  }
  return true;
}

void MainWindow::onCatalogTrashRequested(QStringList keys, bool covers_all) {
  CatalogModel& catalog = session_->catalogModel();
  if (covers_all) {
    if (streaming_manager_ != nullptr && streaming_manager_->hasActiveSession()) {
      streaming_manager_->stopAllAndWait(tr("dataset removed"));
      active_streaming_dataset_id_ = 0;
      streaming_playback_seeded_ = false;
    }
    // Free ObjectStore topics before the catalog wipe so the cleared()
    // subscription sees them gone and resets 2D viewers (symmetric with the
    // "Remove all Datasets" path). Keep lastLoadedSource for reload.
    session_->sessionManager().clearAllObjects();
    // TF buffers derive from the just-evicted objects; drop them with the data.
    if (transform_service_ != nullptr) {
      transform_service_->invalidateAll();
    }
    // REAL delete: drop the catalog (no tombstone), then erase every dataset's scalar
    // storage from the engine. clearAll()'s cleared() tears down curve adapters first, so
    // the engine erase satisfies DataEngine::removeDataset's invalidate-first contract
    // (and resets the transport to empty via the AppSession cleared() hook).
    catalog.clearAll(/*tombstone=*/false);
    SessionManager& session_manager = session_->sessionManager();
    for (const DatasetId id : session_manager.dataEngine().listDatasets()) {
      session_manager.removeDataset(id);
    }
    resetUndoHistory();
    return;
  }
  // Cascade to derived series that depend on the trashed ones (warn + remove).
  {
    std::vector<std::string> removed_names;
    for (const QString& key : keys) {
      if (const auto d = catalog.curveDescriptor(key)) {
        removed_names.push_back(d->topic_name.toStdString());
        if (!d->field_name.isEmpty()) {
          removed_names.push_back((d->topic_name + "/" + d->field_name).toStdString());
        }
      }
    }
    if (!confirmAndRemoveDependentTransforms(removed_names)) {
      return;  // user cancelled
    }
  }

  // Evict the trashed object topics before mutating the catalog, so the
  // itemsRemoved subscription's revalidateObjects() (which checks the
  // ObjectStore, not the catalog) drops their 2D layers. Scalar keys are ignored
  // here (engine is append-only).
  std::vector<ObjectTopicId> trashed_objects;
  for (const QString& key : keys) {
    if (const auto item = catalog.itemDescriptor(key); item.has_value()) {
      if (const ObjectTopicPayload* obj = asObjectTopic(*item)) {
        trashed_objects.push_back(obj->object_topic_id);
      }
    }
  }
  session_->sessionManager().evictObjectTopics(trashed_objects);
  catalog.removeItems(std::vector<QString>(keys.begin(), keys.end()));
  // Shrink the playback range to the surviving visible data right away,
  // rather than only on the next load — unless a streaming dataset exists:
  // the slider is then scoped to the active stream (see the streaming range
  // wiring in the constructor) and a catalog-wide recompute would stomp it.
  if (active_streaming_dataset_id_ == 0) {
    session_->seedPlaybackFromSession();
  }
  resetUndoHistory();
}

void MainWindow::removeDatasetData(DatasetId dataset_id) {
  if (dataset_id == active_streaming_dataset_id_) {
    if (streaming_manager_ != nullptr) {
      streaming_manager_->stopDatasetAndWait(dataset_id, tr("dataset removed"));
    }
    active_streaming_dataset_id_ = 0;
    streaming_playback_seeded_ = false;
  }
  // A REAL delete: erase the dataset's data from the object store AND the engine —
  // no tombstone — so a later load of the same source is a clean fresh load (mints
  // a new DatasetId), not a reattach to an emptied shell.
  //
  // Order matters. Invalidate the TF buffer first (it was built from these object
  // topics; invalidateDataset walks listTopics(dataset_id), empty once eviction
  // runs). Then evict the object payloads (so the catalog's itemsRemoved prunes
  // 2D/3D layers). Then drop the catalog items (tearing down every curve adapter).
  // ONLY THEN erase the engine's scalar storage: DataEngine::removeDataset requires
  // all readers/adapters invalidated first, which the synchronous teardown above guarantees.
  if (transform_service_ != nullptr) {
    transform_service_->invalidateDataset(dataset_id);
  }
  session_->sessionManager().evictDatasetObjects(dataset_id);
  session_->catalogModel().removeDataset(dataset_id, /*tombstone=*/false);
  session_->sessionManager().removeDataset(dataset_id);
  // Drop this dataset's file association (hygiene). Resurrection is prevented by
  // the layout-save liveness filter (appendDataSourceElement), not by mutating
  // loaded_sources_ — that list is kept whole so the quick-reload button still works.
  file_loader_->untrackDataset(dataset_id);
}

void MainWindow::onRemoveDatasetsRequested(const QList<DatasetId>& dataset_ids) {
  if (dataset_ids.isEmpty()) {
    return;
  }
  // One combined confirmation. Single removal keeps the named wording; a batch
  // shows the count.
  QString message;
  if (dataset_ids.size() == 1) {
    QString name = QString::number(dataset_ids.front());
    for (const auto& [id, dataset_name] : session_->catalogModel().datasets()) {
      if (id == dataset_ids.front()) {
        name = dataset_name;
        break;
      }
    }
    message = tr("Are you sure you want to remove '%1' and its data?").arg(name);
  } else {
    message = tr("Are you sure you want to remove these %1 datasets and their data?").arg(dataset_ids.size());
  }
  const int choice = MessageBox::question(
      this, dataset_ids.size() == 1 ? tr("Remove dataset") : tr("Remove datasets"), message,
      {{tr("Remove"), MessageBox::kDestructiveRole}, {tr("Cancel"), MessageBox::kCancelRole}});
  if (choice != 0) {
    return;  // Cancel / Esc
  }

  // Cascade to derived series whose input lives in any of these datasets (their
  // output is typically in another dataset, so removeDataset alone would orphan
  // them). Warn + remove those transforms and their Custom Series entries; abort
  // the whole removal on cancel. No-op when nothing depends on the removed data.
  {
    std::vector<std::string> removed_names;
    for (const auto& item : session_->catalogModel().items()) {
      if (std::find(dataset_ids.begin(), dataset_ids.end(), item.dataset_id) != dataset_ids.end()) {
        removed_names.push_back(item.topic_name.toStdString());
      }
    }
    if (!confirmAndRemoveDependentTransforms(removed_names)) {
      return;  // user cancelled
    }
  }

  for (const DatasetId id : dataset_ids) {
    removeDatasetData(id);
  }
  // A confirmed removal changes the data universe under the playhead, so halt
  // playback and shrink the range to the survivors (or reset to empty when none
  // remain) — unless a live stream is active, where the slider stays scoped to
  // the tip and follow-live keeps running. Reset undo once.
  if (active_streaming_dataset_id_ == 0) {
    session_->playbackEngine().pause();
    session_->seedPlaybackFromSession();
  }
  resetUndoHistory();
}

void MainWindow::onMergeDatasetsRequested(const QList<DatasetId>& dataset_ids) {
  std::vector<DatasetId> ids;
  ids.reserve(static_cast<std::size_t>(dataset_ids.size()));
  for (const DatasetId id : dataset_ids) {
    ids.push_back(id);
  }
  // confirmAndMergeDatasets shows the shared destructive-merge warning and gates on
  // ≥2 data-bearing datasets; mark the surviving anchor on the Source Timeline.
  if (const auto anchor = confirmAndMergeDatasets(this, *session_, ids)) {
    if (source_timeline_controller_ != nullptr) {
      source_timeline_controller_->markDatasetMerged(*anchor);
    }
  }
}

void MainWindow::onShowPreferencesDialog() {
  PreferencesDialog dlg(*theme_, this);
  dlg.exec();
}

void MainWindow::onShowAboutDialog() {
  AboutDialog dialog(this);
  dialog.exec();
}

void MainWindow::showToast(const QString& message, const QPixmap& icon) {
  if (!toast_manager_) {
    toast_manager_ = new ToastManager(this);
  }
  toast_manager_->showToast(message, icon);
}

void MainWindow::checkForUpdates(bool interactive) {
  if (!update_checker_) {
    update_checker_ = new UpdateChecker(this);
  }

  // Rebind the outcome handlers so this call's `interactive` is captured
  // per-request: checkLatestRelease() aborts any prior in-flight check (whose
  // handlers are disconnected here regardless), so the "up to date" / "couldn't
  // check" toasts can never be attributed to the wrong request.
  disconnect(update_available_conn_);
  disconnect(up_to_date_conn_);
  disconnect(check_failed_conn_);

  update_available_conn_ =
      connect(update_checker_, &UpdateChecker::updateAvailable, this, [this](const ReleaseInfo& release) {
        QString message = tr("New release available: <b>%1</b>").arg(release.name.toHtmlEscaped());
        if (!release.html_url.isEmpty()) {
          message += u"<br>"_s + tr("<a href=\"%1\">View on GitHub</a>").arg(release.html_url);
        }
        showToast(message, QPixmap(u":/resources/success_kid.png"_s));
      });

  up_to_date_conn_ = connect(update_checker_, &UpdateChecker::upToDate, this, [this, interactive]() {
    if (interactive) {
      showToast(tr("PlotJuggler is up to date."));
    }
  });

  check_failed_conn_ =
      connect(update_checker_, &UpdateChecker::checkFailed, this, [this, interactive](const QString& reason) {
        qWarning("Update check failed: %s", qUtf8Printable(reason));
        if (interactive) {
          showToast(tr("Could not check for updates. Please try again later."));
        }
      });

  update_checker_->checkLatestRelease();
}

void MainWindow::onCheckForUpdates() {
  checkForUpdates(/*interactive=*/true);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  if (toast_manager_) {
    toast_manager_->updatePosition();
  }
}

void MainWindow::onThemeChanged(const QString& theme) {
  // Connected with Qt::QueuedConnection (see ctor): this slot's heavy work —
  // applyIcons, the stylesheetChanged fan-out, and replotting every plot — must
  // NOT run synchronously inside Theme::setTheme, which is emitted from a
  // PreferencesDialog widget event. Running it reentrantly there segfaulted, so
  // it is deferred to the next event-loop turn.
  //
  // The QSS itself and the tooltip palette are applied by apply_theme_chrome
  // (the qssChanged handler), which fires synchronously inside setTheme — i.e.
  // BEFORE this queued slot. We deliberately do NOT re-apply them here: it would
  // duplicate that work and, worse, run last and overwrite any QSS that a later
  // qssChanged listener (e.g. DebugUi) layered on top of expandedQss().
  applyIcons(theme);
  emit stylesheetChanged(theme);
  forEachPlot([](PlotWidget* plot) { plot->replot(); });
}

void MainWindow::setIconSize(int size) {
  const int clamped = kIconSizeRange.clamp(size);
  if (clamped == chrome_metrics_.icon_size) {
    return;
  }
  chrome_metrics_.icon_size = clamped;
  emit chromeMetricsChanged(chrome_metrics_);
}

void MainWindow::setIconPadding(int padding) {
  const int clamped = kIconPaddingRange.clamp(padding);
  if (clamped == chrome_metrics_.icon_padding) {
    return;
  }
  chrome_metrics_.icon_padding = clamped;
  emit chromeMetricsChanged(chrome_metrics_);
}

void MainWindow::setLayoutPadding(int padding) {
  const int clamped = kLayoutPaddingRange.clamp(padding);
  if (clamped == chrome_metrics_.layout_padding) {
    return;
  }
  chrome_metrics_.layout_padding = clamped;
  emit chromeMetricsChanged(chrome_metrics_);
}

void MainWindow::setLayoutSpacing(int spacing) {
  const int clamped = kLayoutSpacingRange.clamp(spacing);
  if (clamped == chrome_metrics_.layout_spacing) {
    return;
  }
  chrome_metrics_.layout_spacing = clamped;
  emit chromeMetricsChanged(chrome_metrics_);
}

void MainWindow::persistChromeMetrics() const {
  // One QSettings instance ⇒ one INI rewrite for all four keys on sync().
  QSettings settings;
  settings.setValue(kIconSizeKey, chrome_metrics_.icon_size);
  settings.setValue(kIconPaddingKey, chrome_metrics_.icon_padding);
  settings.setValue(kLayoutPaddingKey, chrome_metrics_.layout_padding);
  settings.setValue(kLayoutSpacingKey, chrome_metrics_.layout_spacing);
}

QStringList MainWindow::customPluginFolders() const {
  return session_->extensionCatalog().customPluginFolders();
}

void MainWindow::setCustomPluginFolders(const QStringList& folders) {
  session_->extensionCatalog().setCustomPluginFolders(folders);
}

QStringList MainWindow::builtinPluginFolders() const {
  return session_->extensionCatalog().builtinPluginFolders();
}

void MainWindow::applyIcons(QString theme) {
  // Right-side buttons (Chart + Legend in the global column, Width and
  // Line-style in the local panel) are created programmatically by
  // buildGlobalToolbar() / buildLocalToolbar() and re-tinted by their
  // own stylesheetChanged hooks via each button's "iconPath" property.
  for (const PanelToggle& toggle : panelToggles(ui_)) {
    // The "iconPath" property carries whichever variant (on / off) the
    // toggle currently shows; falls back to the on variant on first
    // paint before the toggle handler has run.
    const QString icon_path = toggle.button->property("iconPath").toString();
    const QString resolved = icon_path.isEmpty() ? QString::fromLatin1(toggle.icon_path_on) : icon_path;
    toggle.button->setIcon(loadSvg(resolved, theme));
  }
  // Title-bar menus: their QActions persist across theme changes, so
  // re-tint here.
  ui_->actionExit->setIcon(QIcon(loadSvg(":/resources/svg/logout.svg", theme)));
  ui_->actionMarketplace->setIcon(QIcon(loadSvg(":/resources/svg/archive.svg", theme)));
  if (action_preferences_ != nullptr) {
    action_preferences_->setIcon(QIcon(loadSvg(":/resources/svg/settings_cog_light.svg", theme)));
  }
  if (action_load_layout_ != nullptr) {
    action_load_layout_->setIcon(QIcon(loadSvg(":/resources/svg/dashboard_load.svg", theme)));
  }
  if (action_save_layout_ != nullptr) {
    action_save_layout_->setIcon(QIcon(loadSvg(":/resources/svg/save_as.svg", theme)));
  }
}

void MainWindow::onPlotTabAdded(PlotDocker* docker) {
  if (docker == nullptr) {
    return;
  }
  connect(docker, &PlotDocker::plotWidgetAdded, this, &MainWindow::onPlotAdded, Qt::UniqueConnection);
  // ADS focus is the single source of truth for "which widget is active":
  // it fires on canvas click and on tab-titlebar click. PlotDocker re-emits
  // it as dockFocused(DockWidget*) so MainWindow can route to the right
  // sidepanel page (plot config / 2D / 3D) without pulling in ADS types.
  connect(docker, &PlotDocker::dockFocused, this, &MainWindow::onDockFocused, Qt::UniqueConnection);
  // Placeholder 2D/3D icon click → build + adopt the empty scene dock here (the
  // shell owns the family→kind mapping). The first topic dropped into such an
  // empty dock seeds streaming playback, matching the placeholder→drop path.
  connect(docker, &PlotDocker::objectFamilyRequested, this, &MainWindow::onObjectFamilyRequested, Qt::UniqueConnection);
  connect(
      docker, &PlotDocker::firstObjectTopicAdded, this, &MainWindow::seedStreamingPlaybackFromDrop,
      Qt::UniqueConnection);
  connect(
      docker, &PlotDocker::placeholderTopicDropped, this, &MainWindow::onPlaceholderTopicDropped, Qt::UniqueConnection);
  for (int index = 0; index < docker->plotCount(); ++index) {
    if (DockWidget* dock = docker->plotAt(index)) {
      onPlotAdded(dock->plotWidget());
    }
  }
  bindEditorToPlot(firstPlotOfActiveTab());
}

void MainWindow::onPlotAdded(PlotWidget* plot) {
  if (plot == nullptr) {
    return;
  }
  connect(plot, &PlotWidget::rectChanged, this, &MainWindow::onPlotZoomChanged, Qt::UniqueConnection);
  connect(plot, &PlotWidget::trackerMoved, this, &MainWindow::onTrackerMovedFromWidget, Qt::UniqueConnection);
  // Dropping a scalar curve into a plot during streaming seeds playback, exactly
  // like dropping an object topic into a 2D/3D dock (PlotDocker::firstObjectTopicAdded).
  // Without this, a scalar-only stream never sets streaming_playback_seeded_, so the
  // live-ingest handler early-returns forever and the slider stays at the placeholder
  // range. seedStreamingPlaybackFromDrop is a no-op unless a stream is active and is
  // one-shot per session, so file-curve drops and later stream drops are harmless.
  connect(plot, &PlotWidget::curvesDropped, this, &MainWindow::seedStreamingPlaybackFromDrop, Qt::UniqueConnection);
  connect(plot, &PlotWidget::filterEditorRequested, this, &MainWindow::openFilterEditor, Qt::UniqueConnection);
  connect(plot, &PlotWidget::statusMessageRequested, this, [this](const QString& message) {
    emitDiagnostic(DiagnosticLevel::kInfo, "Plot", "status", message);
  });
  // registerPlot is idempotent (layout load/undo re-runs onPlotAdded for live
  // plots) and owns the placeholderCurveDropped routing.
  topic_demand_controller_->registerPlot(plot);
  plot->setTrackerPosition(toAxisDouble(session_->playbackEngine().currentTime()));
  applyGlobalToggles(plot);
  if (curve_editor_ != nullptr && curve_editor_->plot() == nullptr) {
    bindEditorToPlot(plot);
  }
}

void MainWindow::openFilterEditor(std::vector<CurveDescriptor> sources, PlotWidget* origin) {
  if (origin == nullptr || session_ == nullptr) {
    return;
  }
  // Snapshot the source colours so the preview's before/after curves match the
  // post-Apply result (the filtered output inherits the source's colour).
  QHash<QString, QColor> source_colors;
  for (const auto& [key, color] : origin->curveColors()) {
    source_colors.insert(key, color);
  }

  auto* panel = new FilterEditorPanel(
      &session_->sessionManager(), &session_->catalogModel(), std::move(sources), std::move(source_colors));

  // Apply: replace each source curve with its filtered output, in place, on the
  // originating plot, then restore the chart area. Guard `origin` with QPointer in
  // case its dock was torn down while the panel was open.
  const QPointer<PlotWidget> origin_guard(origin);
  connect(
      panel, &FilterEditorPanel::applied, this,
      [this, origin_guard](const QList<QPair<QString, QString>>& replacements) {
        if (origin_guard != nullptr) {
          for (const auto& replacement : replacements) {
            origin_guard->replaceCurve(replacement.first, replacement.second);
          }
          origin_guard->replot();
          // Force a discrete undo entry: applying/removing a filter must never coalesce
          // into a preceding edit, so each filter op is its own undo step.
          onUndoableChange(/*force_new_state=*/true);
        }
        restoreCentralArea();
      });
  connect(panel, &FilterEditorPanel::closed, this, [this]() { restoreCentralArea(); });
  connect(panel, &FilterEditorPanel::diagnostic, this, [this](const QString& message) {
    emitDiagnostic(DiagnosticLevel::kWarning, "Filter", "apply", message);
  });

  if (!presentPanel(panel)) {
    // Another chart-area panel (e.g. a toolbox) is already open.
    emitDiagnostic(DiagnosticLevel::kWarning, "Filter", "panel", tr("Close the open panel before applying a filter"));
    panel->deleteLater();
    return;
  }
  filter_editor_origin_ = origin;  // the preview mirrors this plot's grid/style/width
  syncFilterEditorPreviewDisplay();
}

void MainWindow::syncFilterEditorPreviewDisplay() {
  auto* panel = qobject_cast<FilterEditorPanel*>(current_panel_);
  if (panel == nullptr || filter_editor_origin_ == nullptr) {
    return;
  }
  // Mirror the plot the editor was opened on. Grid is global (applied to every plot),
  // so activate_grid_ already equals the origin's grid state.
  panel->setPreviewDisplay(
      activate_grid_, filter_editor_origin_->defaultCurveStyle(), filter_editor_origin_->lineWidth());
}

void MainWindow::syncPanelPreviewDisplay() {
  // A plugin toolbox panel (e.g. the Transform Editor plugin) embeds a real
  // PlotWidget inside its chart QFrame. It has no originating plot to mirror, so its
  // preview keeps the PlotWidget default curve style/width; only the global grid
  // toggle is pushed. Reach the plot generically as a child of the presented panel.
  if (current_panel_ == nullptr) {
    return;
  }
  for (auto* plot : current_panel_->findChildren<PlotWidget*>()) {
    // Stash the grid state on the chart frame so the dialog-host binding can re-apply
    // it on every preview rebuild (source/function changes recreate the curves).
    if (QWidget* frame = plot->parentWidget()) {
      frame->setProperty("_pj_view_set", true);
      frame->setProperty("_pj_view_grid", activate_grid_);
    }
    // Apply immediately too, so a grid toggle updates the preview without waiting for
    // the next chart tick.
    plot->setGridVisible(activate_grid_);
    plot->replot();
  }
}

namespace {
// SVG path for the legend button when the legend is at the given corner.
// kHidden is never a valid argument — callers must remap to the saved
// corner (previous_legend_corner_) before looking up the icon.
[[nodiscard]] QString legendCornerIcon(LegendStatus corner) {
  switch (corner) {
    case LegendStatus::kBottomRight:
      return u":/resources/svg/position_bottom_right.svg"_s;
    case LegendStatus::kBottomLeft:
      return u":/resources/svg/position_bottom_left.svg"_s;
    case LegendStatus::kTopRight:
      return u":/resources/svg/position_top_right.svg"_s;
    case LegendStatus::kTopLeft:
      return u":/resources/svg/position_top_left.svg"_s;
    case LegendStatus::kHidden:
      return u":/resources/svg/position_top_right.svg"_s;
  }
  return u":/resources/svg/position_top_right.svg"_s;
}

// Four-corner forward cycle. Used while the legend is visible to walk
// TR → TL → BL → BR; the BR-to-hidden wrap and the hidden-to-visible
// restore are handled by the click handler so it can also reset the
// saved corner to TR at the cycle boundary.
[[nodiscard]] LegendStatus nextLegendCorner(LegendStatus current) {
  switch (current) {
    case LegendStatus::kTopRight:
      return LegendStatus::kTopLeft;
    case LegendStatus::kTopLeft:
      return LegendStatus::kBottomLeft;
    case LegendStatus::kBottomLeft:
      return LegendStatus::kBottomRight;
    case LegendStatus::kBottomRight:
      return LegendStatus::kTopRight;
    case LegendStatus::kHidden:
      return LegendStatus::kTopRight;
  }
  return LegendStatus::kTopRight;
}
}  // namespace

void MainWindow::setLegendStatus(LegendStatus position) {
  legend_status_ = position;
  // Track the "current position" used by the icon while hidden, so
  // right-click → show restores the user's last corner.
  if (position != LegendStatus::kHidden) {
    previous_legend_corner_ = position;
  }
  QSettings().setValue(u"MainWindow.legendStatus"_s, static_cast<int>(legend_status_));
  if (button_legend_ != nullptr) {
    const bool visible = (position != LegendStatus::kHidden);
    button_legend_->setChecked(visible);
    button_legend_->setIconPath(legendCornerIcon(visible ? position : previous_legend_corner_));
  }
  forEachPlot([this](PlotWidget* plot) { applyLegendStatus(plot); });
}

void MainWindow::applyLegendStatus(PlotWidget* plot) {
  if (plot == nullptr) {
    return;
  }
  const bool visible = legend_status_ != LegendStatus::kHidden;
  plot->setLegendVisible(visible);
  if (!visible) {
    return;
  }
  Qt::Alignment alignment;
  switch (legend_status_) {
    case LegendStatus::kBottomRight:
      alignment = Qt::AlignBottom | Qt::AlignRight;
      break;
    case LegendStatus::kBottomLeft:
      alignment = Qt::AlignBottom | Qt::AlignLeft;
      break;
    case LegendStatus::kTopRight:
      alignment = Qt::AlignTop | Qt::AlignRight;
      break;
    case LegendStatus::kTopLeft:
      alignment = Qt::AlignTop | Qt::AlignLeft;
      break;
    case LegendStatus::kHidden:
      return;
  }
  plot->setLegendAlignment(alignment);
}

void MainWindow::onLegendButtonClicked() {
  // Three branches for the cycle:
  //   * Hidden: re-show at previous_legend_corner_. This is TR right
  //     after a BR-to-hidden wrap, or the last visible corner after
  //     a right-click hide.
  //   * BR (cycle end): reset previous_legend_corner_ to TR so the
  //     unchecked button paints the TR icon, then hide. The next
  //     left-click takes the Hidden branch and re-enters at TR.
  //   * Any other visible corner: advance to the next corner.
  if (legend_status_ == LegendStatus::kHidden) {
    setLegendStatus(previous_legend_corner_);
  } else if (legend_status_ == LegendStatus::kBottomRight) {
    previous_legend_corner_ = LegendStatus::kTopRight;
    setLegendStatus(LegendStatus::kHidden);
  } else {
    setLegendStatus(nextLegendCorner(legend_status_));
  }
}

void MainWindow::applyGlobalToggles(PlotWidget* plot) {
  if (plot == nullptr) {
    return;
  }
  plot->setShowPoints(show_points_);
  plot->setGridVisible(activate_grid_);
  applyDots(plot);
  plot->setReferenceLine(referenceDisplaySeconds());
  plot->setKeepRatioXY(keep_ratio_);
  applyLegendStatus(plot);
  plot->setTrackerParameter(tracker_info_);
}

void MainWindow::applyShowPointsToDock(DockWidget* dock) {
  if (dock == nullptr || dock->objectWidget() == nullptr) {
    return;
  }
  auto* media = qobject_cast<Scene2DDockWidget*>(dock->objectWidget()->widget());
  if (media != nullptr) {
    media->setPointInspectorEnabled(show_points_);
  }
}

void MainWindow::applyShowPointsTo2DWidgets() {
  forEachDock([this](DockWidget* dock) { applyShowPointsToDock(dock); });
}

void MainWindow::updateTimeTrackerIcon() {
  if (button_time_tracker_ == nullptr) {
    return;
  }
  // TODO: 3 PNG variants are light-theme only (PJ3 ships no dark equivalents).
  // Dark-theme users see the light icon; replace with tinted SVGs if/when
  // someone designs them.
  switch (tracker_info_) {
    case CurveTracker::kLineOnly:
      button_time_tracker_->setIcon(QIcon(u":/style_light/line_tracker.png"_s));
      break;
    case CurveTracker::kValue:
      button_time_tracker_->setIcon(QIcon(u":/style_light/line_tracker_1.png"_s));
      break;
    case CurveTracker::kValueName:
      button_time_tracker_->setIcon(QIcon(u":/style_light/line_tracker_a.png"_s));
      break;
  }
}

void MainWindow::onTimeTrackerButtonClicked() {
  switch (tracker_info_) {
    case CurveTracker::kLineOnly:
      tracker_info_ = CurveTracker::kValue;
      break;
    case CurveTracker::kValue:
      tracker_info_ = CurveTracker::kValueName;
      break;
    case CurveTracker::kValueName:
      tracker_info_ = CurveTracker::kLineOnly;
      break;
  }
  QSettings().setValue(u"MainWindow.timeTrackerSetting"_s, static_cast<int>(tracker_info_));
  updateTimeTrackerIcon();
  forEachPlot([this](PlotWidget* plot) {
    plot->setTrackerParameter(tracker_info_);
    plot->replot();
  });
}

void MainWindow::applyDots(PlotWidget* plot) {
  if (plot == nullptr) {
    return;
  }
  // The Dots toggle flips the plot-level style between Lines and Lines+Dots,
  // leaving other styles (Dots, Sticks, Steps) chosen in the Curve Style panel
  // alone. setDefaultStyle restyles every curve and is inherited by new ones.
  const auto from = dots_ ? PlotWidgetBase::kLines : PlotWidgetBase::kLinesAndDots;
  const auto to = dots_ ? PlotWidgetBase::kLinesAndDots : PlotWidgetBase::kLines;
  if (plot->defaultCurveStyle() == from) {
    plot->setDefaultStyle(to);
  }
}

void MainWindow::onPlotZoomChanged(PlotWidget* modified, QRectF rect) {
  if (!button_link_->isChecked()) {
    return;
  }
  if (modified == nullptr || modified->isEmpty() || modified->isXYPlot() || !modified->isZoomLinkEnabled()) {
    return;
  }

  forEachPlot([modified, rect](PlotWidget* plot) {
    if (plot == modified || plot->isEmpty() || plot->isXYPlot() || !plot->isZoomLinkEnabled()) {
      return;
    }
    // Linked zoom only ever aligns the horizontal (time) range: copy the source's
    // X extent onto each peer while leaving its vertical range exactly as the user
    // left it. A purely-vertical gesture (wheel on the left axis, "Zoom Out
    // Vertically") therefore carries an unchanged X and resolves to a no-op here.
    QRectF peer_rect = plot->currentBoundingRect();
    peer_rect.setLeft(rect.left());
    peer_rect.setRight(rect.right());
    plot->setZoomRectangle(peer_rect, false);
    plot->replot();
  });
}

void MainWindow::onTrackerMovedFromWidget(QPointF point) {
  session_->playbackEngine().setCurrentTime(displaySeconds(point.x()));
}

DatasetId MainWindow::representativeDatasetId() const {
  if (active_streaming_dataset_id_ != 0) {
    return active_streaming_dataset_id_;
  }
  if (const auto datasets = session_->sessionManager().dataEngine().listDatasets(); !datasets.empty()) {
    return datasets.front();
  }
  return 0;
}

std::optional<double> MainWindow::referenceDisplaySeconds() const {
  if (!reference_instant_.has_value()) {
    return std::nullopt;
  }
  // Project the frame-invariant instant into the CURRENT frame, exactly as a
  // curve adapter projects its raw samples — line and data share one offset, so
  // the line can never drift off the re-fitted axis.
  const DisplayOffset offset = session_->sessionManager().displayOffset(representativeDatasetId());
  return toAxisDouble(toDisplaySeconds(*reference_instant_, offset));
}

void MainWindow::broadcastReferenceLine() {
  const std::optional<double> ref_sec = referenceDisplaySeconds();
  forEachPlot([ref_sec](PlotWidget* plot) { plot->setReferenceLine(ref_sec); });
  if (source_timeline_controller_ != nullptr) {
    source_timeline_controller_->setReferenceLine(ref_sec);
  }
}

void MainWindow::onUseTimeOffsetToggled(bool checked) {
  auto& sm = session_->sessionManager();
  auto& engine = session_->playbackEngine();

  // Representative dataset frames the playhead; the blue reference re-projects
  // through the same dataset (referenceDisplaySeconds, on displayOffsetChanged).
  const DatasetId representative = representativeDatasetId();
  const DisplayOffset old_offset = sm.displayOffset(representative);
  const DisplaySeconds old_time = engine.currentTime();

  sm.setUseTimeOffset(checked);  // emits displayOffsetChanged -> every plot re-fits in the new frame

  const DisplayOffset new_offset = sm.displayOffset(representative);

  // Re-seed the slider range in the new frame (same choke points as a load).
  if (active_streaming_dataset_id_ != 0) {
    if (const auto range = sm.datasetDisplayRange(active_streaming_dataset_id_); range.has_value()) {
      engine.setRange(*range);
    }
  } else {
    session_->seedPlaybackFromSession();
  }
  // Keep the cursor on the same real instant: freeze it as an absolute Timepoint
  // in the old frame, then re-project into the new one — the Time.h round-trip,
  // no hand-rolled chrono delta.
  const Timepoint instant = toAbsolute(old_time, old_offset);
  engine.setCurrentTime(toDisplaySeconds(instant, new_offset));
}

void MainWindow::seedStreamingPlaybackFromDrop() {
  if (active_streaming_dataset_id_ == 0) {
    return;
  }
  // One-shot per streaming session: a drop may seed the range before the next
  // ingest tick, but later drops must not re-snap a paused scrub. Playback state
  // decides whether this also moves the cursor to the live edge.
  if (streaming_playback_seeded_) {
    return;
  }
  streaming_playback_seeded_ = true;
  if (const auto range = session_->sessionManager().datasetDisplayRange(active_streaming_dataset_id_);
      range.has_value()) {
    auto& engine = session_->playbackEngine();
    if (engine.isPlaying()) {
      engine.setRangeAndCurrentTime(*range, range->max);
    } else {
      engine.setRange(*range);
    }
  }
}

void MainWindow::setSourceTimelineStreamingLock(bool locked) {
  if (source_timeline_ != nullptr) {
    source_timeline_->setInteractionLocked(locked);
  }
  // The align/snap/reset rail drives the controller's align slots directly (not
  // through the widget), so the widget lock alone wouldn't stop a rail click —
  // grey the whole rail too. timelineAlignRail is a .ui container member.
  if (ui_->timelineAlignRail != nullptr) {
    ui_->timelineAlignRail->setEnabled(!locked);
  }
}

DiagnosticSink MainWindow::diagnosticSink() const {
  return diagnostic_bridge_->sink();
}

void MainWindow::emitDiagnostic(DiagnosticLevel level, const char* source, const char* id, const QString& message) {
  Diagnostic d;
  d.level = level;
  d.source = (source != nullptr) ? source : "";
  d.id = (id != nullptr) ? id : "";
  d.message = message.toStdString();
  diagnostic_bridge_->sink()(d);
}

void MainWindow::refreshStreamingCombo() {
  QStringList names;
  for (const auto* ds : session_->extensionCatalog().streamSources()) {
    names << QString::fromStdString(ds->name);
  }
  ui_->leftPanel->setStreamingSources(names);
}

void MainWindow::wireExistingPlots() {
  forEachDocker([this](PlotDocker* docker) { onPlotTabAdded(docker); });
}

void MainWindow::forEachDocker(const std::function<void(PlotDocker*)>& operation) {
  for (int index = 0; index < ui_->tabbedPlotWidget->dockerCount(); ++index) {
    if (PlotDocker* docker = ui_->tabbedPlotWidget->dockerAt(index)) {
      operation(docker);
    }
  }
}

void MainWindow::forEachDock(const std::function<void(DockWidget*)>& operation) {
  forEachDocker([&operation](PlotDocker* docker) {
    for (int index = 0; index < docker->plotCount(); ++index) {
      DockWidget* dock = docker->plotAt(index);
      if (dock != nullptr) {
        operation(dock);
      }
    }
  });
}

void MainWindow::forEachVisibleDock(const std::function<void(DockWidget*)>& operation) {
  // Only the active top-level tab's docks are on screen (the other tabs live in a
  // hidden QStackedWidget page). A dock hidden by a maximized sibling still lives
  // in the active docker, so it IS visited — the conservative gate is per-tab.
  //
  // Limitation (pre-existing, NOT introduced here): plotAt() returns each area's
  // currentDockWidget(), so a dock tabbed BEHIND another inside the same area is
  // not visited per-tick — and its sub-tab switch emits no currentTabChanged, so
  // the catch-up does not re-seed it. origin/main's forEachDock had the same blind
  // spot. Normal PJ4 docking never creates inner tabs (splits insert into
  // OuterDockAreas, which exclude the center/tabify area), so this is unreachable
  // via the UI; if inner tabbing is ever enabled, re-seed from CDockAreaWidget::currentChanged.
  PlotDocker* active = ui_->tabbedPlotWidget->currentTab();
  if (active == nullptr) {
    return;
  }
  for (int index = 0; index < active->plotCount(); ++index) {
    if (DockWidget* dock = active->plotAt(index); dock != nullptr) {
      operation(dock);
    }
  }
}

void MainWindow::broadcastTrackerTime(double display_seconds) {
  forEachDock([display_seconds](DockWidget* dock) { dock->onTrackerTime(display_seconds); });
  ui_->curveListPanel->refreshValues(display_seconds);
}

void MainWindow::broadcastTrackerTimeToVisible(double display_seconds) {
  // Per-tick playback/scrub fan-out: only the active top-level tab is on screen, so
  // forEachVisibleDock skips every other tab's docks — a hidden-tab plot's replot
  // still does the full canvas work (and, on an RHI window, the per-frame composite)
  // for nothing. This is the temporal cap's spatial sibling: cap-the-rate (broadcast
  // throttle) x skip-the-invisible (here). Structural seeds keep using
  // broadcastTrackerTime() (ALL tabs) so a hidden tab is correct the instant it is
  // revealed, and the currentTabChanged handler re-seeds the newly active tab.
  forEachVisibleDock([display_seconds](DockWidget* dock) { dock->onTrackerTime(display_seconds); });
  ui_->curveListPanel->refreshValues(display_seconds);
}

void MainWindow::forEachSceneDock(const std::function<void(SceneDockWidget*)>& operation) {
  forEachDock([&operation](DockWidget* dock) {
    if (dock->objectWidget() == nullptr) {
      return;
    }
    QWidget* object_widget = dock->objectWidget()->widget();
    if (auto* scene_dock = qobject_cast<SceneDockWidget*>(object_widget); scene_dock != nullptr) {
      operation(scene_dock);
    }
  });
}

void MainWindow::forEachPlot(const std::function<void(PlotWidget*)>& operation) {
  forEachDock([&operation](DockWidget* dock) {
    if (PlotWidget* plot = dock->plotWidget()) {
      operation(plot);
    }
  });
}

void MainWindow::syncWidgetsToCatalog() {
  // Each widget prunes its OWN now-invalid pieces against the live catalog/store:
  //  - plots drop curves whose source key is gone (empty plot stays, reusable);
  //  - object viewers drop layers whose topic was evicted, reporting empty so the
  //    shell resets that dock to the reusable placeholder.
  forEachPlot([](PlotWidget* plot) { plot->revalidate(); });
  forEachDock([](DockWidget* dock) {
    auto* viewer = dynamic_cast<IObjectViewer*>(dock->objectWidget());
    if (viewer != nullptr && !viewer->revalidateObjects()) {
      dock->clearToPlaceholder();
    }
  });
  // Clearing a dock to its placeholder doesn't change ADS focus, so the right
  // panel won't refresh on its own. Re-evaluate it against the active tab's
  // focused dock: if that dock was just emptied, the panel falls back to the
  // empty page; otherwise this is a no-op. Keying off the active tab (rather
  // than a remembered pointer) avoids ever painting a hidden tab's dock.
  onDockFocused(activeFocusedDock());
}

void MainWindow::linkedZoomOut() {
  if (!button_link_->isChecked()) {
    forEachPlot([](PlotWidget* plot) { plot->zoomOut(false); });
    return;
  }
  forEachDocker([](PlotDocker* docker) {
    auto plot_at = [docker](int index) -> PlotWidget* {
      DockWidget* dock = docker->plotAt(index);
      PlotWidget* plot = dock != nullptr ? dock->plotWidget() : nullptr;
      return (plot != nullptr && !plot->isEmpty()) ? plot : nullptr;
    };

    std::optional<Range<double>> x_union;
    for (int index = 0; index < docker->plotCount(); ++index) {
      PlotWidget* plot = plot_at(index);
      if (plot == nullptr || plot->isXYPlot()) {
        continue;
      }
      const QRectF rect = plot->maxZoomRect();
      if (!x_union) {
        x_union = Range<double>{rect.left(), rect.right()};
      } else {
        x_union->min = std::min(x_union->min, rect.left());
        x_union->max = std::max(x_union->max, rect.right());
      }
    }

    for (int index = 0; index < docker->plotCount(); ++index) {
      PlotWidget* plot = plot_at(index);
      if (plot == nullptr) {
        continue;
      }
      if (plot->isXYPlot() || !x_union) {
        plot->zoomOut(false);
        continue;
      }
      QRectF rect = plot->maxZoomRect();
      rect.setLeft(x_union->min);
      rect.setRight(x_union->max);
      plot->setZoomRectangle(rect, false);
      plot->replot();
    }
  });
}

void MainWindow::closeEvent(QCloseEvent* event) {
  // Stop and join any in-flight worker load (discard) and drain the queue BEFORE
  // the session/datastore tear down, so a worker can't write into a freed engine
  // or fire a queued completion at a half-destroyed window.
  if (file_loader_ != nullptr) {
    file_loader_->joinForShutdown();
  }
  QSettings settings;
  settings.setValue(u"MainWindow.buttonLink"_s, button_link_->isChecked());
  // Remember the left-panel width (the whole splitter layout) so the next launch
  // restores it instead of falling back to the narrow .ui default.
  settings.setValue(u"MainWindow.mainSplitterState"_s, ui_->mainSplitter->saveState());
  QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);

  // (1) Restore the remembered left-panel width once, after the splitter has real
  // geometry. Done here rather than in the constructor so the saved sizes aren't
  // overwritten by the first layout pass; an explicit --layout load runs later
  // and still wins (it re-applies splitter sizes through restoreChromeState).
  if (!left_splitter_restored_) {
    left_splitter_restored_ = true;
    const QByteArray state = QSettings().value(u"MainWindow.mainSplitterState"_s).toByteArray();
    if (state.isEmpty() || !ui_->mainSplitter->restoreState(state)) {
      // First-ever launch (nothing remembered) — or a stale/mismatched saved blob
      // that restoreState rejected: open the left panel at a comfortable default
      // rather than letting the splitter collapse it toward its 280 px minimum.
      // Clamped by the leftColumn's 280..600 px size constraints.
      constexpr int kDefaultLeftPanelWidth = 400;
      const int total = ui_->mainSplitter->width();
      if (total > kDefaultLeftPanelWidth) {
        ui_->mainSplitter->setSizes({kDefaultLeftPanelWidth, total - kDefaultLeftPanelWidth});
      }
    }
  }

  // (2) Size the bottom (timeline) panel definitively on first show. Deferred to
  // after the first layout pass: only once the splitter has real pixel geometry
  // does an absolute setSizes() stick — doing it in the constructor leaves a ratio
  // that resolves to a squashed sliver. Closed → playback bar only; open → a
  // definite strip height (last expanded height, floored at the open minimum).
  if (!bottom_panel_sized_) {
    bottom_panel_sized_ = true;
    QTimer::singleShot(0, this, [this]() {
      applyBottomPanelConstraints();
      if (ui_->timelineStrip == nullptr || ui_->timelineStrip->isHidden()) {
        return;  // closed: applyBottomPanelConstraints already pinned it to the playback bar
      }
      const QList<int> sizes = ui_->timelineSplitter->sizes();
      if (sizes.size() != 2) {
        return;
      }
      const int total = sizes[0] + sizes[1];
      const int floor = ui_->bottomPanel->minimumHeight();  // playback + kMinTimelineStripHeight
      const int expanded = std::max(QSettings().value(kPanelBottomExpandedKey, floor).toInt(), floor);
      ui_->timelineSplitter->setSizes({total - expanded, expanded});
      // The playback bar is laid out now, so the slider's start x is real — line the
      // name column separator up under it.
      alignNameColumnToPlayback();
    });
  }
}

void MainWindow::onLoadLayout() {
  // Remember the last directory across sessions (matches PJ3 behaviour
  // and the data-file loader's pattern at FileLoader.cpp:82). Default
  // to the working directory on first run rather than ~/Documents,
  // since layout files commonly live in project directories.
  const QString start_dir = QSettings().value(kLastLayoutDirKey, QDir::currentPath()).toString();
  // Passing `this` as the metrics source primes the dialog with the
  // current icon size and keeps it in step if chromeMetricsChanged fires.
  const QString path = FileDialog::getOpenFileName(this, tr("Load Layout"), start_dir, tr(kLayoutFilter), this);
  if (path.isEmpty()) {
    return;
  }
  QSettings().setValue(kLastLayoutDirKey, QFileInfo(path).absolutePath());
  loadLayoutFromPath(path);
}

void MainWindow::onSaveLayout() {
  const QString start_dir = QSettings().value(kLastLayoutDirKey, QDir::currentPath()).toString();
  // setDefaultSuffix (passed through PJ::FileDialog) wants the extension
  // without the leading dot.
  const QString default_suffix = QString::fromLatin1(layout_xml::kLayoutExtension).mid(1);

  // Checked = source-bound: embed the data-source reference so opening the
  // layout reloads this exact file. Unchecked = generic: the layout carries
  // no file reference and binds its curves (by topic+field) to whatever
  // dataset is loaded when it's opened — for reuse across similar recordings.
  // Default ON matches PJ3 and the common "save, reload later" workflow.
  const FileDialog::ExtraOption save_source_opt{tr("Bind to this data source"), /*default_checked=*/true};
  const auto result = FileDialog::getSaveFileNameWithOptions(
      this, tr("Save Layout"), start_dir, tr(kLayoutFilter), default_suffix, {save_source_opt}, this);

  if (result.path.isEmpty()) {
    return;
  }
  // Backstops the dialog's defaultSuffix: a bare typed name (no extension)
  // becomes a .pj4.xml file even if the platform dialog skipped the suffix.
  const QString save_path = layout_xml::ensureLayoutExtension(result.path);
  QSettings().setValue(kLastLayoutDirKey, QFileInfo(save_path).absolutePath());
  const bool include_data_source = !result.option_states.empty() && result.option_states[0];
  saveLayoutToPath(save_path, include_data_source);
}

void MainWindow::onLoadRecentLayout(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
  if (!QFileInfo::exists(path)) {
    emitDiagnostic(DiagnosticLevel::kWarning, "Layout", "missing", tr("Layout file no longer exists: %1").arg(path));
    // Drop the dead entry so it stops showing up.
    QStringList recent = recentLayouts();
    recent.removeAll(path);
    QSettings().setValue(kRecentLayoutsKey, recent);
    return;
  }
  loadLayoutFromPath(path);
}

void MainWindow::onRebuildExtensionsMenu() {
  QMenu* menu = installed_extensions_menu_;
  menu->clear();

  const auto& catalog = session_->extensionCatalog();
  bool added_any = false;
  const auto append_plugins = [&]<typename Plugin>(const std::vector<Plugin>& plugins) {
    for (const auto& plugin : plugins) {
      const QString name = QString::fromStdString(plugin.name);
      const QString version = QString::fromStdString(plugin.version);
      const QString label = version.isEmpty() ? name : u"%1 (%2)"_s.arg(name, version);
      QAction* action = menu->addAction(label);
      action->setEnabled(false);  // Informational only — manage via Marketplace.
      added_any = true;
    }
  };
  append_plugins(catalog.dataSources());
  append_plugins(catalog.messageParsers());
  append_plugins(catalog.toolboxes());

  if (!added_any) {
    QAction* placeholder = menu->addAction(tr("(no extensions installed)"));
    placeholder->setEnabled(false);
  }
}

void MainWindow::onRebuildToolboxMenu() {
  QMenu* menu = title_bar_->toolboxMenu();
  menu->clear();
  menu->setToolTipsVisible(true);

  // List the launchable toolboxes: every loaded toolbox EXCEPT the
  // cloud-tagged ones, which are reached from the Sources panel instead
  // (same manifest "tags" check as LeftPanel::populateCloudToolboxes,
  // inverted here). Each entry launches its toolbox into the chart area.
  bool added_any = false;
  for (const auto& toolbox : session_->extensionCatalog().toolboxes()) {
    // A null vtable/manifest is a load failure already reported elsewhere.
    const auto* vtable = toolbox.library.vtable();
    if (vtable == nullptr || vtable->manifest_json == nullptr) {
      continue;
    }
    const auto manifest = nlohmann::json::parse(vtable->manifest_json, nullptr, /*allow_exceptions=*/false);
    // Skip toolboxes that opt out of this menu: "cloud" ones are reached from the
    // Sources panel; "hidden" ones are launched elsewhere (e.g. the Transform
    // Editor, opened from the "+" in Custom Series).
    bool skip = false;
    if (manifest.is_object()) {
      if (auto it = manifest.find("tags"); it != manifest.end() && it->is_array()) {
        for (const auto& tag : *it) {
          if (tag.is_string() && (tag.get<std::string>() == "cloud" || tag.get<std::string>() == "hidden")) {
            skip = true;
            break;
          }
        }
      }
    }
    if (skip) {
      continue;
    }

    const QString id = QString::fromStdString(toolbox.id);
    const QString name = toolbox.name.empty() ? id : QString::fromStdString(toolbox.name);
    QAction* action = menu->addAction(name);
    if (manifest.is_object()) {
      if (auto desc = manifest.find("description"); desc != manifest.end() && desc->is_string()) {
        action->setToolTip(QString::fromStdString(desc->get<std::string>()));
      }
    }
    connect(action, &QAction::triggered, this, [this, id]() { launchToolbox(id); });
    added_any = true;
  }

  if (!added_any) {
    QAction* placeholder = menu->addAction(tr("(no toolboxes available)"));
    placeholder->setEnabled(false);
  }
}

void MainWindow::loadLayoutFromPath(const QString& path) {
  // 1. Open + parse
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    MessageBox::warning(this, tr("Load Layout"), tr("Cannot open '%1' for reading.").arg(path));
    return;
  }
  QDomDocument doc;
  const QDomDocument::ParseResult parse_result = doc.setContent(&file);
  if (!parse_result) {
    file.close();
    MessageBox::warning(
        this, tr("Load Layout"),
        tr("'%1' is not a valid PJ4 layout: %2 (line %3, col %4)")
            .arg(path, parse_result.errorMessage)
            .arg(parse_result.errorLine)
            .arg(parse_result.errorColumn));
    return;
  }
  file.close();

  // Forward-compat schema check. pj4_version is written by xmlSaveState
  // as an integer string; if we encounter a layout from a newer PJ4
  // that introduced incompatible schema changes, warn but proceed —
  // unknown elements are silently ignored downstream anyway. We never
  // hard-fail on this; the user can always re-save under the current
  // schema.
  const QDomElement root = doc.documentElement();
  bool version_ok = false;
  const int version = root.attribute(u"pj4_version"_s, u"0"_s).toInt(&version_ok);
  if (version_ok && version > kLayoutSchemaVersion) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "schema-newer",
        tr("Layout '%1' was saved by a newer PJ4 (pj4_version=%2 > %3); loading best-effort.")
            .arg(QFileInfo(path).fileName())
            .arg(version)
            .arg(kLayoutSchemaVersion));
  }

  // 2. Source-bound layouts may reload their original file; generic layouts
  // (and source-bound ones whose file is missing or where the user opts out)
  // bind straight to the currently-loaded data. The binding attribute records
  // the save-time intent; this is the load-time override.
  const QString binding = root.attribute(u"binding"_s, u"source"_s);
  const QDir layout_dir(QFileInfo(path).absoluteDir());
  // Dataset-path qualifiers are stored relative whenever the data sits beside or
  // beneath the layout. Resolve them to normalized absolute paths before any
  // binder sees the document, so every widget compares full paths while a layout
  // + data directory remains freely relocatable.
  layout_xml::resolveDatasetSourcePaths(doc, layout_dir);
  // A pre-path or source-only layout persisted a numeric id plus a basename-like
  // source. That integer is unsafe outside the session that minted it: strip it on
  // read as well as on write, leaving the source-only fallback to bind only when
  // unique.
  layout_xml::removeUnvalidatedDatasetIds(doc);
  const QList<layout_xml::DataSourceRef> replays = layout_xml::extractDataSource(doc, layout_dir);
  // Set when the user chose "Reload original": the data loads (possibly async on
  // a worker), so the layout apply below must wait for the load queue to drain.
  bool reload_requested = false;
  if (binding != u"generic"_s && !replays.empty()) {
    // Classify each referenced file: already loaded (skip), missing on disk
    // (warn + skip), or reloadable. A file counts as already loaded only while
    // the catalog has data — a remembered-but-cleared source must reload.
    const auto& loaded = session_->sessionManager().loadedSources();
    const bool catalog_has_data = !session_->catalogModel().isEmpty();
    QList<layout_xml::DataSourceRef> pending;
    for (const auto& replay : replays) {
      if (replay.resolved_path.isEmpty()) {
        continue;
      }
      const bool already_loaded =
          catalog_has_data && std::any_of(loaded.begin(), loaded.end(), [&replay](const auto& src) {
            return layout_xml::isSamePath(src.path, replay.resolved_path);
          });
      if (already_loaded) {
        continue;
      }
      if (!QFileInfo::exists(replay.resolved_path)) {
        emitDiagnostic(
            DiagnosticLevel::kWarning, "Layout", "data-source-missing",
            tr("Layout's data source '%1' does not exist on disk; applying to current data.")
                .arg(replay.resolved_path));
        continue;
      }
      pending.push_back(replay);
    }

    if (!pending.empty()) {
      // The --layout CLI option auto-reloads the layout's source(s) without
      // prompting; interactively, one consolidated prompt covers the whole pending
      // set (never one box per file; "Load Layout only" is the fall-through).
      bool do_reload = startup_auto_reload_;
      if (!startup_auto_reload_) {
        QStringList file_lines;
        file_lines.reserve(pending.size());
        for (const auto& replay : pending) {
          file_lines.push_back(u"  %1"_s.arg(replay.resolved_path));
        }
        // Themed prompt (frameless, vertical button column in the app chrome).
        // The button order below defines the index question() returns:
        // 0 = reload, 1 = layout-only (fall-through), 2 = cancel. "Reload source
        // file" is the primary/default action; Esc maps to the kCancelRole button.
        constexpr int kReloadOriginal = 0;
        constexpr int kCancel = 2;
        const int choice = MessageBox::question(
            this, tr("Load Layout"),
            tr("This layout was saved with %n data source(s):\n\n%1", nullptr, static_cast<int>(pending.size()))
                .arg(file_lines.join(QLatin1Char('\n'))),
            {{tr("Reload source file"), MessageBox::kPrimaryRole},
             {tr("Load Layout only"), MessageBox::kNeutralRole},
             {tr("Cancel"), MessageBox::kCancelRole}});
        if (choice == kCancel || choice < 0) {
          return;  // Cancel or dialog dismissed → abort the layout load.
        }
        do_reload = (choice == kReloadOriginal);
      }
      if (do_reload) {
        reload_requested = true;
        // Load each pending file. Distinct files append as separate datasets
        // (FileLoader replaces in place only on a basename match), so the full
        // multi-file session is restored. FileLoader shows its own error dialog
        // on failure; fall through and let the unresolved-curve handling below
        // catch an empty load.
        for (const auto& replay : pending) {
          LoadHints hints{
              .expected_plugin_id = replay.plugin_id,
              .preset_config_json = replay.plugin_config_json,
              .skip_dialog = !replay.plugin_id.isEmpty() && !replay.plugin_config_json.isEmpty(),
              .prefer_reuse = true,
          };
          file_loader_->loadFile(replay.resolved_path, this, hints);
        }
      }
      // choice == 1 (Use current data) → fall through and apply the layout to
      // the currently loaded data.
    }
  }

  // The reloaded data may still be arriving on the worker thread. In that case,
  // build the layout structure now and bind remaining curves live as topics arrive.
  // If nothing is loading (generic / use-current / a reload that finished
  // synchronously) apply immediately through the complete restore path.
  if (reload_requested && file_loader_->isBusy()) {
    beginProgressiveLayoutRestore(doc, path);
    return;
  }
  applyRestoredLayout(doc, path);
}

void MainWindow::loadLayoutAtStartup(const QString& path) {
  // --layout CLI entry: load the layout and auto-reload its data source(s) with no
  // prompt (the flag is read in loadLayoutFromPath's source-classification block).
  startup_auto_reload_ = true;
  loadLayoutFromPath(path);
  startup_auto_reload_ = false;
}

void MainWindow::enableAutoplay() {
  autoplay_pending_ = true;
  // Start looped playback the first time the engine reports a non-empty range — set
  // synchronously by --test-data, or asynchronously once --layout's worker load
  // finishes and seeds the range. Guarded by autoplay_pending_ so it fires exactly
  // once (the rangeChanged connection stays but no-ops afterward), leaving a later
  // manual pause/seek untouched.
  PlaybackEngine& playback = session_->playbackEngine();
  connect(&playback, &PlaybackEngine::rangeChanged, this, [this](double min, double max) {
    if (!autoplay_pending_ || !(max > min)) {
      return;
    }
    autoplay_pending_ = false;
    PlaybackEngine& pb = session_->playbackEngine();
    pb.setLooping(true);
    pb.play();
  });
}

void MainWindow::applyRestoredLayout(QDomDocument doc, const QString& path) {
  // 3. Filters + curve rebinding + plot apply happen together in restoreWorkspaceState
  // below. Each curve resolves against whichever loaded dataset actually holds its
  // topic+field (first match in load order), so a multi-file layout restores
  // each plot against its own source — and a layout built on one recording still
  // reuses on a similar one (same topics/fields). This is the same restore path
  // undo/redo uses; there is deliberately no "apply to which
  // dataset?" prompt — a saved layout binds to its data, not to one chosen set.
  // Paths no loaded dataset can provide are surfaced via the missing-curve prompt.
  if (session_->catalogModel().datasets().empty()) {
    MessageBox::warning(
        this, tr("Load Layout"), tr("No data is loaded. Open a data source before applying this layout."));
    return;
  }
  // 4. Recreate filters, rebind curves, and apply plots/toggles through the ONE restore
  // path shared with undo/redo (kPrompt: a curve no loaded dataset can provide raises the
  // missing-curve prompt). Filters are recreated BEFORE the curve rebind so each derived
  // output topic is in the catalog. Panel/chrome restores below stay layout-only.
  switch (restoreWorkspaceState(doc, MissingCurvePolicy::kPrompt)) {
    case RestoreResult::kCancelled:
      return;  // user aborted at the missing-curve prompt
    case RestoreResult::kFailed:
      // If step 2 already reloaded a data source AND apply failed, the world is left
      // half-mutated: the new data is loaded but the user's plots/panels never came
      // back. Rolling back a synchronous ingest is not currently feasible (FileLoader
      // has no "unload" API and the DataEngine doesn't support transactional commits).
      // The warning is the best signal we can offer.
      MessageBox::warning(
          this, tr("Load Layout"),
          tr("Layout was parsed but could not be applied. If a data source was reloaded, it is still loaded."));
      return;
    case RestoreResult::kApplied:
      break;
  }

  restoreChromeAndPanels(doc, path);
}

void MainWindow::restoreChromeAndPanels(const QDomDocument& doc, const QString& path) {
  // 4a. Restore curve-list content state (filters + show_topics/show_values toggles).
  ui_->curveListPanel->restoreListState(doc.documentElement().firstChildElement(u"curve_list_state"_s));

  // 4b. Restore right-panel state.
  restoreRightPanelState(doc.documentElement().firstChildElement(u"right_panel_state"_s));

  // 4c. Restore LeftPanel Sources tab + streaming controls.
  ui_->leftPanel->restoreSourcesState(doc.documentElement().firstChildElement(u"left_panel_state"_s));

  // 4d. Restore chrome state (panel visibilities + splitter sizes).
  restoreChromeState(doc.documentElement().firstChildElement(u"chrome_state"_s));

  // 4e. Restore Source Timeline state (per-source display offsets + track order).
  // Runs after the datasets are (re)loaded so it re-binds them by path. The data
  // source refs were consumed during the reload classification in
  // loadLayoutFromPath; re-extract them here (a pure parse of doc) so this restore
  // step has them in scope.
  const QDir timeline_layout_dir(QFileInfo(path).absoluteDir());
  applyTimelineStateFromLayout(layout_xml::extractDataSource(doc, timeline_layout_dir));

  // 4f. Restore the timeline's global view chrome (zoom/scroll/name-column/snap),
  // AFTER 4e so zoom/scroll map onto the offset-adjusted, rebuilt scene.
  restoreSourceTimelineViewState(doc.documentElement().firstChildElement(u"source_timeline"_s));

  // 5. Recent files + diagnostic
  recordRecentLayout(path);
  emitDiagnostic(DiagnosticLevel::kInfo, "Layout", "loaded", tr("Loaded layout: %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::beginProgressiveLayoutRestore(QDomDocument doc, const QString& path) {
  cancelProgressiveLayoutRestore();
  progressive_layout_in_flight_ = true;

  bool applied = false;
  {
    QScopedValueRollback guard(applying_state_, true);
    const QDomElement root = doc.documentElement();
    restoreDataProcessors(root);
    // rebindCurvesToLoadedDatasets rewrites doc in place; its unresolved-paths return is
    // not needed here — the progressive binder reports unresolved curves at drain.
    static_cast<void>(rebindCurvesToLoadedDatasets(doc));
    if (xmlLoadState(doc)) {
      QHash<QString, PlotWidget*> plots_by_state_id;
      forEachPlot([&plots_by_state_id](PlotWidget* plot) {
        if (!plot->stateId().isEmpty()) {
          plots_by_state_id.insert(plot->stateId(), plot);
        }
      });
      pending_binder_->collect(doc, plots_by_state_id);
      restoreChromeAndPanels(doc, path);
      broadcastTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
      applied = true;
    }
  }

  if (!applied) {
    cancelProgressiveLayoutRestore();
    MessageBox::warning(this, tr("Load Layout"), tr("Layout was parsed but could not be applied."));
    return;
  }

  pending_items_added_conn_ = connect(
      &session_->catalogModel(), &CatalogModel::itemsAdded, this, [this](const std::vector<CatalogItem>& items) {
        flushPendingCurveBindings(items);
        retryPendingSceneRestores(items);
      });
  pending_queue_drained_conn_ = connect(
      file_loader_.get(), &FileLoader::queueDrained, this, &MainWindow::onProgressiveLayoutDrained,
      Qt::SingleShotConnection);
  flushPendingCurveBindings({});
  retryPendingSceneRestores({});
}

void MainWindow::cancelProgressiveLayoutRestore() {
  QObject::disconnect(pending_items_added_conn_);
  QObject::disconnect(pending_queue_drained_conn_);
  pending_items_added_conn_ = {};
  pending_queue_drained_conn_ = {};
  if (pending_binder_ != nullptr) {
    pending_binder_->clear();
  }
  clearPendingSceneRestores();
  progressive_layout_in_flight_ = false;
}

void MainWindow::flushPendingCurveBindings(const std::vector<CatalogItem>& items) {
  if (pending_binder_ == nullptr || pending_binder_->empty()) {
    return;
  }
  QSet<QString> topics;
  for (const CatalogItem& item : items) {
    if (!item.topic_name.isEmpty()) {
      topics.insert(item.topic_name);
    }
  }
  static_cast<void>(pending_binder_->flush(topics));
}

int MainWindow::retryPendingSceneRestores(const std::vector<CatalogItem>& items) {
  QSet<QString> topics;
  for (const CatalogItem& item : items) {
    if (isObjectTopic(item) && !item.topic_name.isEmpty()) {
      topics.insert(item.topic_name);
    }
  }
  if (!items.empty() && topics.isEmpty()) {
    return 0;
  }

  int restored = 0;
  forEachSceneDock([&](SceneDockWidget* scene_dock) { restored += scene_dock->retryPendingRestores(topics); });
  return restored;
}

QStringList MainWindow::unresolvedPendingSceneRestores() {
  QStringList unresolved;
  QSet<QString> seen;
  forEachSceneDock([&](SceneDockWidget* scene_dock) {
    for (const QString& topic : scene_dock->unresolvedPendingRestores()) {
      if (!topic.isEmpty() && !seen.contains(topic)) {
        seen.insert(topic);
        unresolved.push_back(topic);
      }
    }
  });
  return unresolved;
}

void MainWindow::clearPendingSceneRestores() {
  forEachSceneDock([](SceneDockWidget* scene_dock) { scene_dock->clearPendingRestores(); });
}

void MainWindow::onProgressiveLayoutDrained() {
  QObject::disconnect(pending_items_added_conn_);
  pending_items_added_conn_ = {};

  if (pending_binder_ != nullptr) {
    static_cast<void>(pending_binder_->flush({}));
    // Frame each restored plot to its layout-saved window with the now-settled display
    // offset (the file has finished loading). This previously called zoomOut because the
    // saved range was display-relative and the save vs reload offsets could differ; PR
    // #248 made the saved X ABSOLUTE and applySavedViewportOrZoom converts it with the
    // live offset, so re-applying it is correct — and it pins the plot to its final
    // window so data fills in like streaming rather than the axis auto-fitting/growing.
    // Plots with no/degenerate saved range fall back to zoomOut; clear_after drops the
    // one-shot stash. Still under the in-flight gate, so onUndoableChange stays
    // suppressed until the single snapshot below.
    forEachPlot([](PlotWidget* plot) { plot->applySavedViewportOrZoom(/*clear_after=*/true); });

    QStringList shown;
    QSet<QString> seen;
    for (const layout_xml::SeriesPath& path : pending_binder_->unresolved()) {
      const QString display = path.display();
      if (!display.isEmpty() && !seen.contains(display)) {
        seen.insert(display);
        shown.push_back(display);
      }
    }
    if (!shown.isEmpty()) {
      switch (promptMissingCurves(shown)) {
        case MissingCurveChoice::kRemove:
          break;  // Remaining curves were never bound, so there is nothing to strip from live widgets.
        case MissingCurveChoice::kCancel:
          // The reload has already mutated the live session, so progressive restore is non-transactional:
          // leave the partial layout in place instead of rolling back to a stale pre-load snapshot.
          break;
      }
    }
    pending_binder_->clear();
  }

  retryPendingSceneRestores({});
  broadcastTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
  const QStringList unresolved_scenes = unresolvedPendingSceneRestores();
  if (!unresolved_scenes.isEmpty()) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "scene_restore_pending",
        tr("%n scene layer(s) could not be rebound after progressive load.", nullptr,
           static_cast<int>(unresolved_scenes.size())));
  }
  clearPendingSceneRestores();

  QObject::disconnect(pending_queue_drained_conn_);
  pending_queue_drained_conn_ = {};
  progressive_layout_in_flight_ = false;
  pushUndoState(/*force_new_state=*/true);
}

void MainWindow::saveLayoutToPath(const QString& path, bool include_data_source) {
  QDomDocument doc = xmlSaveState();
  // Record the binding intent so load knows whether to reload the original
  // file (source-bound) or adopt the currently-loaded dataset (generic). The
  // data-source element below is only embedded for source-bound layouts.
  doc.documentElement().setAttribute(u"binding"_s, include_data_source ? u"source"_s : u"generic"_s);
  if (include_data_source) {
    const QDir layout_dir(QFileInfo(path).absoluteDir());
    QDomElement ds = appendDataSourceElement(doc, layout_dir);
    if (!ds.isNull()) {
      doc.documentElement().appendChild(ds);
    }
    // Widget serializers know DatasetIds but intentionally know nothing about
    // FileLoader. Stamp one coherent full-path identity across plots (and any
    // future processor/scene qualifiers) as a post-pass. Use the same relocatable
    // subpath rule as <fileInfo>; load resolves it before binding.
    layout_xml::stampDatasetSourcePaths(doc, [this, &layout_dir](std::uint32_t dataset_id) {
      const QString source_path = file_loader_->sourcePathForDataset(dataset_id);
      if (source_path.isEmpty()) {
        return QString{};
      }
      return relocatableSubpath(QFileInfo(source_path).absoluteFilePath(), layout_dir);
    });
    // A non-file dataset has no path with which a future session can validate a
    // numeric id. Keep its source fallback but drop that volatile id so a
    // duplicate basename cannot silently capture the persisted layout.
    layout_xml::removeUnvalidatedDatasetIds(doc);
  } else {
    // A generic layout is intentionally reusable on another recording. Keep exact
    // DatasetId/source qualifiers in undo and source-bound documents, but strip
    // them from this file copy so topic+field paths bind — only when unambiguous —
    // to whatever data is present when the layout is opened.
    layout_xml::removeDatasetQualifiersForGenericLayout(doc);
  }
  // Always save right-panel state — pure UI chrome, no privacy cost,
  // not gated by Save Data Source.
  doc.documentElement().appendChild(saveRightPanelState(doc));
  // Always save the remaining widget state — pure UI chrome, not gated
  // by Save Data Source.
  doc.documentElement().appendChild(ui_->leftPanel->saveSourcesState(doc));
  doc.documentElement().appendChild(ui_->curveListPanel->saveListState(doc));
  doc.documentElement().appendChild(saveChromeState(doc));
  // NOTE: <data_processors> is already emitted by xmlSaveState() (it is part of THE
  // snapshot now, shared with undo/redo); do not append it again here or the layout
  // would carry two copies.
  // Timeline view chrome (zoom/scroll/name-column/snap). Pure UI, not gated by
  // Save Data Source; the per-source offsets/order ride <fileInfo> separately.
  doc.documentElement().appendChild(saveSourceTimelineViewState(doc));
  // QSaveFile gives us write-temp + rename atomicity: a partial write
  // (disk full, signal, broken NFS) leaves the user's prior layout
  // untouched. commit() does the rename; cancelWriting() abandons the
  // tempfile. A QFile::Truncate path would have destroyed prior state
  // before completing the write.
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    MessageBox::warning(this, tr("Save Layout"), tr("Cannot open '%1' for writing.").arg(path));
    return;
  }
  const QByteArray bytes = doc.toByteArray(2);
  if (file.write(bytes) != bytes.size()) {
    const QString err = file.errorString();
    file.cancelWriting();
    MessageBox::warning(this, tr("Save Layout"), tr("Failed to write layout to '%1': %2").arg(path, err));
    return;
  }
  if (!file.commit()) {
    MessageBox::warning(
        this, tr("Save Layout"), tr("Failed to finalize layout '%1': %2").arg(path, file.errorString()));
    return;
  }
  recordRecentLayout(path);
  emitDiagnostic(DiagnosticLevel::kInfo, "Layout", "saved", tr("Saved layout: %1").arg(QFileInfo(path).fileName()));
}

QDomElement MainWindow::saveDataProcessors(QDomDocument& doc) const {
  QDomElement element = doc.createElement(u"data_processors"_s);
  for (const auto& recipe : session_->sessionManager().dataProcessorService().recipes()) {
    // Resolve the input column to a stable (topic, field) path so it rebinds on
    // reload exactly like a plotted curve.
    const QString input_key = u"dataset:%1/topic:%2/column:%3"_s.arg(recipe.dataset_id)
                                  .arg(recipe.input_topic_id)
                                  .arg(recipe.input_column_index);
    const std::optional<CurveDescriptor> input_desc = session_->catalogModel().curveDescriptor(input_key);
    if (!input_desc.has_value()) {
      continue;  // input field is gone; nothing to persist
    }
    QDomElement processor = doc.createElement(u"processor"_s);
    processor.setAttribute(u"input_topic"_s, input_desc->topic_name);
    processor.setAttribute(u"input_field"_s, input_desc->field_path);
    // Dataset qualifiers so the input rebinds to its own source dataset on restore,
    // never a same-topic sibling (first-match was part of the reported undo bug).
    // input_dataset_path is stamped by stampDatasetSourcePaths at layout-file save.
    processor.setAttribute(u"input_dataset_id"_s, QString::number(recipe.dataset_id));
    if (const auto source = session_->catalogModel().datasetSourceName(recipe.dataset_id); source.has_value()) {
      processor.setAttribute(u"input_dataset_source"_s, *source);
    }
    processor.setAttribute(u"processor_id"_s, QString::fromStdString(recipe.processor_id));
    processor.setAttribute(u"output_name"_s, QString::fromStdString(recipe.output_name));
    if (recipe.processor) {
      layout_xml::appendJsonAsCdata(doc, processor, QString::fromStdString(recipe.processor->saveParams()));
    }
    // Embed the filter's Luau source so the layout reopens on a machine without
    // this filter installed (resolved by restore's source_fallback leg). A named
    // child element, NOT a second direct CDATA child — restore reads params via
    // directCdataText, which ignores child elements. Empty for native C++ builtins.
    if (!recipe.filter_source.empty()) {
      QDomElement source_el = doc.createElement(u"source_fallback"_s);
      layout_xml::appendJsonAsCdata(doc, source_el, QString::fromStdString(recipe.filter_source));
      processor.appendChild(source_el);
    }
    element.appendChild(processor);
  }
  // Plugin-created transforms (pj.data_processors.v1): named, owner-tagged nodes
  // that carry their script + bindings BY VALUE, so they replay on load WITHOUT the
  // originating plugin (needed only for re-editing). Persisted in the same snapshot
  // block as the per-curve filters above; restore re-installs them via
  // DataProcessorService::restoreTransform. Inputs/outputs are topic NAMES (the
  // engine resolves them on restore), so no (topic, field) rebinding is needed here.
  for (const auto& recipe : session_->sessionManager().dataProcessorService().transformRecipes()) {
    QDomElement transform = doc.createElement(u"transform"_s);
    transform.setAttribute(u"owner_plugin"_s, QString::fromStdString(recipe.owner_plugin));
    transform.setAttribute(u"id"_s, QString::fromStdString(recipe.user_id));
    transform.setAttribute(u"backend"_s, QString::fromStdString(recipe.backend));
    transform.setAttribute(u"api_version"_s, QString::fromStdString(recipe.api_version));
    if (!recipe.backend_version.empty()) {
      transform.setAttribute(u"backend_version"_s, QString::fromStdString(recipe.backend_version));
    }
    for (const auto& input_name : recipe.inputs) {
      QDomElement in = doc.createElement(u"input"_s);
      in.setAttribute(u"name"_s, QString::fromStdString(input_name));
      transform.appendChild(in);
    }
    for (const auto& output_name : recipe.outputs) {
      QDomElement out = doc.createElement(u"output"_s);
      out.setAttribute(u"name"_s, QString::fromStdString(output_name));
      transform.appendChild(out);
    }
    // params (create(params), JSON) and script (the full backend payload, Luau today)
    // each get their own CDATA child so restore reads them back independently.
    if (!recipe.params_json.empty()) {
      QDomElement params = doc.createElement(u"params"_s);
      layout_xml::appendJsonAsCdata(doc, params, QString::fromStdString(recipe.params_json));
      transform.appendChild(params);
    }
    QDomElement script = doc.createElement(u"script"_s);
    layout_xml::appendJsonAsCdata(doc, script, QString::fromStdString(recipe.script));
    transform.appendChild(script);
    element.appendChild(transform);
  }
  return element;
}

void MainWindow::restoreDataProcessors(const QDomElement& root) {
  // Reconcile the live filter set to this snapshot: drop ALL current filters first,
  // then recreate the snapshot's set. This makes restore idempotent for undo/redo (no
  // duplicate or output-name-colliding filters) and correct for a layout load onto an
  // existing session. A snapshot with NO <data_processors> still falls through to clear
  // every filter; the rebuild at the end MUST run on both branches so the now-retired
  // outputs leave the catalog.
  auto& service = session_->sessionManager().dataProcessorService();
  service.clearAllFilters();

  const QDomElement element = root.firstChildElement(u"data_processors"_s);
  // A null <data_processors> yields a null firstChildElement, so this loop runs zero
  // times — the clear above is then the whole effect.
  for (QDomElement processor = element.firstChildElement(u"processor"_s); !processor.isNull();
       processor = processor.nextSiblingElement(u"processor"_s)) {
    const QString input_topic = processor.attribute(u"input_topic"_s);
    const QString input_field = processor.attribute(u"input_field"_s);
    // Resolve the input through the same dataset-qualified path a plotted curve
    // uses: the exact source dataset when its qualifiers still agree, a unique
    // fallback otherwise, and never a same-topic sibling by load order.
    const layout_xml::SeriesPath input_path{
        input_topic, input_field, static_cast<DatasetId>(processor.attribute(u"input_dataset_id"_s).toUInt()),
        processor.attribute(u"input_dataset_source"_s), processor.attribute(u"input_dataset_path"_s)};
    std::optional<CurveDescriptor> input_desc;
    if (const auto input_key = resolveSeriesPath(session_->catalogModel(), input_path); input_key.has_value()) {
      input_desc = session_->catalogModel().curveDescriptor(*input_key);
    }
    if (!input_desc.has_value()) {
      // The filter's source signal isn't in any loaded dataset -> the filter is
      // dropped. Tell the user which one, mirroring the unknown/apply-failed cases
      // below (otherwise a derived series silently vanishes on layout load).
      emitDiagnostic(
          DiagnosticLevel::kWarning, "Layout", "processor-input-missing",
          tr("Layout filter on '%1/%2' has no matching data; skipping.").arg(input_topic, input_field));
      continue;
    }
    const std::string id = processor.attribute(u"processor_id"_s).toStdString();
    // Params are the <processor>'s OWN direct CDATA — directCdataText ignores the
    // <source_fallback> child (QDomElement::text() would recurse and merge them).
    const QString params = layout_xml::directCdataText(processor);
    const QString source_fallback = processor.firstChildElement(u"source_fallback"_s).text();
    // Resolve order: live catalogue → embedded source → transitional C++ builtin.
    std::unique_ptr<proc::DataProcessor> built = service.makeRestoredProcessor(
        id, params.isEmpty() ? std::string("{}") : params.toStdString(), source_fallback.toStdString());
    if (!built) {
      emitDiagnostic(
          DiagnosticLevel::kWarning, "Layout", "processor-unknown",
          tr("Layout filter '%1' is unknown to this PlotJuggler; skipping.").arg(QString::fromStdString(id)));
      continue;
    }
    const auto applied = service.applyFilter(
        input_desc->topic_id, input_desc->dataset_id, std::move(built),
        processor.attribute(u"output_name"_s).toStdString(), input_desc->column_index);
    if (!applied.has_value()) {
      emitDiagnostic(
          DiagnosticLevel::kWarning, "Layout", "processor-apply-failed",
          tr("Could not restore filter: %1").arg(QString::fromStdString(applied.error())));
    }
  }

  // Reconcile plugin transforms the same way (clear-all, then replay the snapshot's
  // set) so restore is idempotent for undo/redo and a layout load can't duplicate a
  // transform. Done AFTER filters so a transform whose input is a filter output can
  // resolve it by name. The clear runs unconditionally: a snapshot with no
  // <transform> children then simply leaves every transform torn down.
  service.clearAllTransforms();
  for (QDomElement transform = element.firstChildElement(u"transform"_s); !transform.isNull();
       transform = transform.nextSiblingElement(u"transform"_s)) {
    DataProcessorService::TransformRecipe recipe;
    recipe.owner_plugin = transform.attribute(u"owner_plugin"_s).toStdString();
    recipe.user_id = transform.attribute(u"id"_s).toStdString();
    recipe.key = DataProcessorService::makeTransformKey(recipe.owner_plugin, recipe.user_id);
    recipe.backend = transform.attribute(u"backend"_s, u"luau"_s).toStdString();
    recipe.api_version = transform.attribute(u"api_version"_s, u"1"_s).toStdString();
    recipe.backend_version = transform.attribute(u"backend_version"_s).toStdString();
    for (QDomElement in = transform.firstChildElement(u"input"_s); !in.isNull();
         in = in.nextSiblingElement(u"input"_s)) {
      recipe.inputs.push_back(in.attribute(u"name"_s).toStdString());
    }
    for (QDomElement out = transform.firstChildElement(u"output"_s); !out.isNull();
         out = out.nextSiblingElement(u"output"_s)) {
      recipe.outputs.push_back(out.attribute(u"name"_s).toStdString());
    }
    recipe.params_json = layout_xml::directCdataText(transform.firstChildElement(u"params"_s)).toStdString();
    if (recipe.params_json.empty()) {
      recipe.params_json = "{}";
    }
    recipe.script = layout_xml::directCdataText(transform.firstChildElement(u"script"_s)).toStdString();
    if (const auto restored = service.restoreTransform(recipe); !restored.has_value()) {
      emitDiagnostic(
          DiagnosticLevel::kWarning, "Layout", "transform-restore-failed",
          tr("Could not restore transform '%1': %2")
              .arg(QString::fromStdString(recipe.key), QString::fromStdString(restored.error())));
    }
  }
  session_->catalogModel().rebuildFromDatastore();
}

void MainWindow::recordRecentLayout(const QString& path) {
  QStringList recent = recentLayouts();
  recent.removeAll(path);  // dedupe — most-recent-first
  recent.prepend(path);
  while (recent.size() > kMaxRecentEntries) {
    recent.removeLast();
  }
  QSettings().setValue(kRecentLayoutsKey, recent);
  // A recorded layout populates the popup's Layouts section, so light up the
  // recent button even when no data file has been loaded yet.
  ui_->leftPanel->setRecentEnabled(true);
}

QStringList MainWindow::recentLayouts() const {
  return QSettings().value(kRecentLayoutsKey).toStringList();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  const QEvent::Type type = event->type();
  // QTipLabel is the internal widget Qt creates for tooltips. On Linux
  // KWin / Mutter normally paint a drop shadow around any tool/popup
  // window; setting Qt::NoDropShadowWindowHint asks the platform plugin
  // to suppress it. Polish fires once before the first show and the
  // QTipLabel is reused for all tooltips, so a single tweak covers
  // every subsequent tooltip.
  if (type == QEvent::Polish && watched->inherits("QTipLabel")) {
    if (auto* w = qobject_cast<QWidget*>(watched)) {
      w->setWindowFlag(Qt::NoDropShadowWindowHint, true);
    }
    return false;
  }
  // The align rail sits in the window's bottom-right corner, where the default
  // tooltip placement needs an upward flip that fails under Wayland. Show the
  // rail's tooltips ourselves, to the left of the rail and clamped on-screen.
  if (type == QEvent::ToolTip) {
    auto* btn = qobject_cast<QToolButton*>(watched);
    if (btn != nullptr && ui_->timelineAlignRail != nullptr && btn->parentWidget() == ui_->timelineAlignRail &&
        !btn->toolTip().isEmpty()) {
      const QFontMetrics fm(QToolTip::font());
      const QSize tip_size = fm.size(Qt::TextSingleLine, btn->toolTip()) + QSize(12, 8);  // ~QTipLabel chrome
      const QPoint btn_top_left = btn->mapToGlobal(QPoint(0, 0));
      QPoint pos(
          btn_top_left.x() - tip_size.width() - 4,                      // just left of the rail
          btn_top_left.y() + (btn->height() - tip_size.height()) / 2);  // vertically centered on the button
      if (const QScreen* screen = btn->screen()) {
        const QRect avail = screen->availableGeometry();
        pos.setX(std::clamp(pos.x(), avail.left() + 4, avail.right() - tip_size.width() - 4));
        pos.setY(std::clamp(pos.y(), avail.top() + 4, avail.bottom() - tip_size.height() - 4));
      }
      QToolTip::showText(pos, btn->toolTip(), btn);
      return true;
    }
    return false;
  }
  if (type != QEvent::MouseMove && type != QEvent::MouseButtonPress) {
    return false;
  }
  // Only act on mouse events that target a widget in this window.
  auto* widget = qobject_cast<QWidget*>(watched);
  if (widget == nullptr || widget->window() != this) {
    return false;
  }
  if (isMaximized() || isFullScreen()) {
    return false;
  }

  auto* mouse_event = static_cast<QMouseEvent*>(event);
  const QPoint window_pos = mapFromGlobal(mouse_event->globalPosition().toPoint());
  const Qt::Edges edges = edgesAtPoint(size(), window_pos);

  if (type == QEvent::MouseMove) {
    if (edges != 0) {
      setCursor(cursorForEdges(edges));
    } else {
      unsetCursor();
    }
    return false;
  }

  // MouseButtonPress
  if (mouse_event->button() != Qt::LeftButton || edges == 0) {
    return false;
  }
  if (auto* handle = windowHandle()) {
    handle->startSystemResize(edges);
    return true;
  }
  return false;
}

void MainWindow::onUndoableChange(bool force_new_state) {
  if (applying_state_ || progressive_layout_in_flight_) {
    return;
  }
  pushUndoState(force_new_state);
}

QList<layout_xml::SeriesPath> MainWindow::rebindCurvesToLoadedDatasets(QDomDocument& doc) {
  // Resolve each curve's stable topic+field against whichever loaded dataset
  // actually holds it (first match in load order). Shared by layout load and
  // undo/redo restore so both bind curves identically; returns the paths no
  // loaded dataset could provide (the caller decides whether to prompt).
  return layout_xml::rebindCurveKeys(
      doc, [this](const layout_xml::SeriesPath& p) { return resolveSeriesPath(session_->catalogModel(), p); });
}

MainWindow::RestoreResult MainWindow::restoreWorkspaceState(QDomDocument& doc, MissingCurvePolicy policy) {
  const QDomElement root = doc.documentElement();
  // 1. Recreate the snapshot's filters first, so each derived output topic is in the
  //    catalog and its plotted (derived) curve resolves like any other curve.
  restoreDataProcessors(root);
  // 2. Rebind every curve's stable topic+field to a concrete catalog key.
  const QList<layout_xml::SeriesPath> unresolved = rebindCurvesToLoadedDatasets(doc);
  if (policy == MissingCurvePolicy::kPrompt && !unresolved.isEmpty()) {
    QStringList shown;
    shown.reserve(unresolved.size());
    for (const layout_xml::SeriesPath& sp : unresolved) {
      shown.push_back(sp.display());
    }
    switch (promptMissingCurves(shown)) {
      case MissingCurveChoice::kCancel:
        return RestoreResult::kCancelled;
      case MissingCurveChoice::kRemove:
        layout_xml::stripUnresolvedCurves(doc);
        break;
    }
  }
  // kSilentDrop (undo/redo): unresolved curves are left to drop during xmlLoadState's
  // bind — a snapshot survives an intervening data reload because it carries stable
  // topic/field paths, not per-load keys.
  // 3. Apply plots + global toggles.
  if (!xmlLoadState(doc)) {
    return RestoreResult::kFailed;
  }
  // 4. Seed the just-recreated docks with the current playhead. currentTimeChanged
  // only fires on a CHANGE, so a freshly restored dock would sit at no-tracker-time
  // until the next scrub — scene docks then render blank (TF lookups / image decode
  // key off the tracker instant). Same seeding the drag-drop / click-create paths do
  // (MainWindow.cpp:480, makeSeededEmptyObjectDock); here it covers layout load + undo/redo.
  broadcastTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
  return RestoreResult::kApplied;
}

void MainWindow::onUndo() {
  if (undo_states_.size() <= 1) {
    return;
  }

  redo_states_.push_back(undo_states_.back());
  undo_states_.pop_back();
  QDomDocument doc;
  doc.setContent(undo_states_.back());
  // Restore the COMPLETE snapshot (filters + curves + plots/toggles) under
  // applying_state_ so the catalog-rebuild signals fired by the filter reconcile do not
  // re-enter onUndoableChange and push spurious undo states.
  const bool loaded = [&] {
    QScopedValueRollback guard(applying_state_, true);
    return restoreWorkspaceState(doc, MissingCurvePolicy::kSilentDrop) == RestoreResult::kApplied;
  }();

  if (!loaded) {
    statusBar()->showMessage(tr("Unable to restore undo state"), 3000);
  }
  undo_timer_.restart();
  updateUndoRedoActions();
}

void MainWindow::onRedo() {
  if (redo_states_.empty()) {
    return;
  }

  undo_states_.push_back(redo_states_.back());
  redo_states_.pop_back();
  QDomDocument doc;
  doc.setContent(undo_states_.back());
  // Restore the COMPLETE snapshot (filters + curves + plots/toggles) under
  // applying_state_ so the catalog-rebuild signals fired by the filter reconcile do not
  // re-enter onUndoableChange and push spurious undo states.
  const bool loaded = [&] {
    QScopedValueRollback guard(applying_state_, true);
    return restoreWorkspaceState(doc, MissingCurvePolicy::kSilentDrop) == RestoreResult::kApplied;
  }();

  if (!loaded) {
    statusBar()->showMessage(tr("Unable to restore redo state"), 3000);
  }
  undo_timer_.restart();
  updateUndoRedoActions();
}

QDomElement MainWindow::appendDataSourceElement(QDomDocument& doc, const QDir& layout_dir) const {
  const auto& sources = session_->sessionManager().loadedSources();
  // Do not save a data-source reference after the catalog was cleared.
  if (sources.empty() || session_->catalogModel().isEmpty()) {
    return QDomElement();
  }

  // Persist only sources that still back a live dataset. A dataset whose curves
  // were all removed (Remove Dataset, or trashing every topic) drops out of
  // datasets(), so its file is neither serialized here nor resurrected on the
  // next reload — while loaded_sources_ itself stays intact for the quick-reload
  // button (which keys off lastLoadedSource, not this list). FileLoader owns the
  // DatasetId->path link the engine's basename-only DatasetInfo can't provide.
  QSet<QString> live_paths;
  QHash<QString, DatasetId> dataset_for_path;  // reverse of sourcePathForDataset, for offset lookup
  for (const auto& [id, name] : session_->catalogModel().datasets()) {
    (void)name;
    if (const QString src_path = file_loader_->sourcePathForDataset(id); !src_path.isEmpty()) {
      live_paths.insert(src_path);
      dataset_for_path.insert(src_path, id);
    }
  }

  // Source Timeline arrangement: the bar's top-to-bottom slot, persisted per
  // file so the vertical order round-trips (re-bound by path on reload). Build
  // dataset -> slot once; absent => fall back to load order on restore.
  QHash<DatasetId, int> timeline_order;
  if (source_timeline_controller_ != nullptr) {
    const std::vector<DatasetId> order = source_timeline_controller_->currentTrackOrder();
    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
      timeline_order.insert(order[static_cast<std::size_t>(i)], i);
    }
  }

  QDomElement wrapper = doc.createElement(u"previouslyLoaded_Datafiles"_s);

  // One <fileInfo> per loaded file, in load order, so a multi-file session
  // round-trips. Old PJ4 readers that only read the first child degrade to the
  // first file; new readers restore them all.
  for (const auto& src : sources) {
    if (!live_paths.contains(src.path)) {
      continue;  // dataset removed since load; don't resurrect it on reload
    }
    QDomElement file_info = doc.createElement(u"fileInfo"_s);

    const QString abs = QFileInfo(src.path).absoluteFilePath();
    file_info.setAttribute(u"filename"_s, relocatableSubpath(abs, layout_dir));
    file_info.setAttribute(u"prefix"_s, src.prefix);

    // Source Timeline state for this file, re-bound by path on reload. The
    // display offset is read live from the SessionManager (the per-source
    // display shift the user dragged); timeline_order is its bar's vertical
    // slot. Both keyed by the dataset this path currently backs.
    if (const auto it = dataset_for_path.constFind(src.path); it != dataset_for_path.constEnd()) {
      const DatasetId id = it.value();
      file_info.setAttribute(
          u"display_offset_ns"_s, QString::number(session_->sessionManager().displayOffset(id).value.count()));
      if (const auto order_it = timeline_order.constFind(id); order_it != timeline_order.constEnd()) {
        file_info.setAttribute(u"timeline_order"_s, QString::number(order_it.value()));
      }
    }

    // Emit the plugin sub-element whenever the plugin id is known. An
    // empty saveConfig payload is legitimate (some plugins have no
    // user-tunable state) and must NOT cause us to skip — otherwise
    // those plugins would re-prompt on every layout reload. Empty
    // plugin_id means the loader didn't capture a plugin (legacy path
    // or saveConfig failure); only that case skips the child.
    if (!src.plugin_id.isEmpty()) {
      QDomElement plugin = doc.createElement(u"plugin"_s);
      plugin.setAttribute(u"ID"_s, src.plugin_id);
      // CDATA so the JSON survives round-tripping without XML escape mangling.
      // appendJsonAsCdata splits across multiple CDATA sections when the JSON
      // contains a literal "]]>" sequence (otherwise it'd terminate the
      // CDATA early and corrupt the layout file).
      layout_xml::appendJsonAsCdata(doc, plugin, src.plugin_config_json);
      file_info.appendChild(plugin);
    }

    wrapper.appendChild(file_info);
  }
  // Every source was filtered out (all their datasets are gone): emit nothing
  // rather than an empty <previouslyLoaded_Datafiles> wrapper.
  if (!wrapper.hasChildNodes()) {
    return QDomElement();
  }
  return wrapper;
}

void MainWindow::applyTimelineStateFromLayout(const QList<layout_xml::DataSourceRef>& sources) {
  if (sources.isEmpty()) {
    return;
  }
  SessionManager& mgr = session_->sessionManager();

  // Re-bind each saved <fileInfo> to whichever loaded dataset came from that
  // file (DatasetIds are re-minted per session; the path is the stable key),
  // apply its display offset, and collect (id, slot) for the order rebuild.
  std::vector<std::pair<int, DatasetId>> ordered;  // (timeline_order, id)
  for (const layout_xml::DataSourceRef& ref : sources) {
    DatasetId matched = 0;
    for (const auto& [id, name] : session_->catalogModel().datasets()) {
      (void)name;
      const QString src_path = file_loader_->sourcePathForDataset(id);
      if (!src_path.isEmpty() && layout_xml::isSamePath(src_path, ref.resolved_path)) {
        matched = id;
        break;
      }
    }
    if (matched == 0) {
      continue;  // file referenced by the layout isn't loaded — nothing to restore
    }
    if (ref.has_display_offset) {
      mgr.setDisplayOffset(matched, DisplayOffset{Duration{ref.display_offset_ns}});
    }
    if (ref.timeline_order >= 0) {
      ordered.emplace_back(ref.timeline_order, matched);
    }
  }

  // Rebuild the vertical track order from the saved slots. Sorting by the saved
  // index (not document order) keeps the arrangement exact even if <fileInfo>
  // elements were written in load order rather than display order.
  if (source_timeline_controller_ != nullptr && !ordered.empty()) {
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<DatasetId> order;
    order.reserve(ordered.size());
    for (const auto& [slot, id] : ordered) {
      (void)slot;
      order.push_back(id);
    }
    source_timeline_controller_->setDisplayOrder(std::move(order));
  }
}

QDomElement MainWindow::saveSourceTimelineViewState(QDomDocument& doc) const {
  // Read the view chrome off the widgets; layout_xml owns the XML schema.
  layout_xml::SourceTimelineViewState state;
  if (source_timeline_ != nullptr) {
    state.zoom = source_timeline_->zoom();
    state.scroll_left_ns = source_timeline_->viewportLeftDisplayNs();
    state.name_column_width = source_timeline_->nameColumnWidth();
  }
  if (ui_->timelineAlignRail != nullptr) {
    if (auto* snap = ui_->timelineAlignRail->findChild<QToolButton*>(u"buttonTimelineSnap"_s)) {
      state.snap = snap->isChecked();
    }
  }
  return layout_xml::writeSourceTimelineViewState(doc, state);
}

void MainWindow::restoreSourceTimelineViewState(const QDomElement& element) {
  if (source_timeline_ == nullptr) {
    return;
  }
  const layout_xml::SourceTimelineViewState state = layout_xml::readSourceTimelineViewState(element);
  // Zoom first: it rebuilds the scene (width + scrollbar range), so the scroll
  // restore below maps onto the intended zoom.
  if (state.zoom) {
    source_timeline_->setZoom(*state.zoom);
  }
  if (state.scroll_left_ns) {
    source_timeline_->setViewportLeftDisplayNs(*state.scroll_left_ns);
  }
  if (state.name_column_width) {
    // Remember it so the deferred alignNameColumnToPlayback keeps the column at
    // this width (over the playback-aligned floor), then apply it now.
    timeline_name_column_width_ = *state.name_column_width;
    source_timeline_->resizeNameColumn(*state.name_column_width);
  }
  if (state.snap) {
    // Drive the rail toggle so its checked state and the widget stay in sync
    // (toggled -> Timeline::setSnapEnabled). Fall back to the widget directly.
    auto* btn = ui_->timelineAlignRail != nullptr
                    ? ui_->timelineAlignRail->findChild<QToolButton*>(u"buttonTimelineSnap"_s)
                    : nullptr;
    if (btn != nullptr) {
      btn->setChecked(*state.snap);
    } else {
      source_timeline_->setSnapEnabled(*state.snap);
    }
  }
}

QDomElement MainWindow::saveRightPanelState(QDomDocument& doc) const {
  QDomElement element = doc.createElement(u"right_panel_state"_s);

  // The right panel's open/closed state (the panel-visibility toggle button) is
  // an app-wide QSettings preference, NOT document state, so it is deliberately
  // not serialized into the layout/undo snapshot. Only the panel's geometry and
  // curve width/style live here.

  // Curve Width: the radio's checkedId is a loop index (0..3); map it
  // back to the canonical double via kWidthButtonSpecs so we encode the
  // value (rebuild-stable) rather than the index (depends on button
  // declaration order).
  if (width_button_group_ != nullptr) {
    const int id = width_button_group_->checkedId();
    if (id >= 0 && id < static_cast<int>(kWidthButtonSpecs.size())) {
      element.setAttribute(u"width"_s, QString::number(kWidthButtonSpecs[id].second, 'g'));
    }
  }

  // Curve Style: the radio's checkedId IS the CurveStyle enum value
  // (assigned at button-group construction). Stable across rebuilds.
  if (style_button_group_ != nullptr) {
    const int id = style_button_group_->checkedId();
    if (id >= 0) {
      element.setAttribute(u"style"_s, QString::number(id));
    }
  }

  if (ui_->rightToolbarSplitter != nullptr) {
    QStringList parts;
    const QList<int> sizes = ui_->rightToolbarSplitter->sizes();
    parts.reserve(sizes.size());
    for (int s : sizes) {
      parts.push_back(QString::number(s));
    }
    element.setAttribute(u"splitter_sizes"_s, parts.join(QLatin1Char(',')));
  }

  return element;
}

void MainWindow::applyBottomPanelConstraints() {
  if (ui_->bottomPanel == nullptr || ui_->timelineWidget == nullptr || ui_->timelineStrip == nullptr) {
    return;
  }
  // The playback bar is fixed-height (sizePolicy Fixed in the .ui; min==max is
  // pinned to its sizeHint by the chrome-metrics pass), so its current minimum
  // height is the playback band's extent.
  const int playback_height = ui_->timelineWidget->minimumHeight();
  // Use the strip's *intended* state (!isHidden), NOT isVisible(): this runs during
  // the constructor's chrome-metrics pass, before the window is shown, where
  // isVisible() is still false for an open strip and would wrongly pick the CLOSED
  // branch (pinning the panel to the playback bar) — which left the strip squashed
  // into a sliver on boot.
  const bool strip_visible = !ui_->timelineStrip->isHidden();
  // The align rail (and its 1-px divider) only make sense beside an open strip;
  // hide them with the panel so a closed timeline shows no orphaned right-edge column.
  if (ui_->timelineAlignRail != nullptr) {
    ui_->timelineAlignRail->setVisible(strip_visible);
  }
  if (ui_->timelineAlignBorder != nullptr) {
    ui_->timelineAlignBorder->setVisible(strip_visible);
  }
  // The playback/timeline divider only makes sense with the strip open; hide it
  // with the panel so a closed timeline shows no stray 1-px line under playback.
  if (ui_->timelinePlaybackBorder != nullptr) {
    ui_->timelinePlaybackBorder->setVisible(strip_visible);
  }
  if (strip_visible) {
    // OPEN: floor the panel at playback + a usable strip height so the Source
    // Timeline never opens (or is dragged) into a clipped sliver; no upper cap.
    const int min_panel = playback_height + kMinTimelineStripHeight;
    ui_->bottomPanel->setMaximumHeight(QWIDGETSIZE_MAX);
    ui_->bottomPanel->setMinimumHeight(min_panel);
    // QSplitter caches old slot sizes and won't re-honor a child's new minimum
    // on its own — push it up to the floor when the strip slot is too short.
    const QList<int> sizes = ui_->timelineSplitter->sizes();
    if (sizes.size() == 2 && sizes[1] < min_panel) {
      ui_->timelineSplitter->setSizes({(sizes[0] + sizes[1]) - min_panel, min_panel});
    }
  } else {
    // CLOSED: pin the panel to exactly the playback bar. With min == max the
    // splitter handle can't drag the (hidden) strip open — only the toggle re-opens it.
    ui_->bottomPanel->setMinimumHeight(playback_height);
    ui_->bottomPanel->setMaximumHeight(playback_height);
    // Collapse the splitter slot too, so no empty gap is left below the playback
    // (QSplitter caches sizes and won't shrink the slot from the max change alone).
    const QList<int> sizes = ui_->timelineSplitter->sizes();
    if (sizes.size() == 2 && sizes[1] != playback_height) {
      ui_->timelineSplitter->setSizes({(sizes[0] + sizes[1]) - playback_height, playback_height});
    }
  }
  if (auto* bottom_layout = ui_->bottomPanel->layout()) {
    bottom_layout->invalidate();
    bottom_layout->activate();
  }
}

void MainWindow::alignNameColumnToPlayback() {
  if (source_timeline_ == nullptr || ui_->timelineWidget == nullptr) {
    return;
  }
  // The playback slider's groove has margin 0 (QSS), so the slider widget's left
  // edge is where the blue track starts. Its x within the playback bar — which
  // shares the bottom panel's left origin with the name column — is the column
  // width that lines the separator up exactly under that track start.
  auto* slider = ui_->timelineWidget->findChild<QWidget*>(u"timeSlider"_s);
  if (slider == nullptr) {
    return;
  }
  const int slider_x = slider->mapTo(ui_->timelineWidget, QPoint(0, 0)).x();
  if (slider_x <= 0) {
    return;  // playback bar not laid out yet (pre first show)
  }
  // slider_x is the floor (separator under the playback track start). If the user
  // widened the column (or a layout restored a wider width), re-apply it on top
  // so the column keeps its chosen width rather than snapping back to the floor.
  source_timeline_->setNameColumnWidth(slider_x);
  if (timeline_name_column_width_ > slider_x) {
    source_timeline_->resizeNameColumn(timeline_name_column_width_);
  }
}

void MainWindow::restoreRightPanelState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != u"right_panel_state"_s) {
    return;
  }

  // Panel visibility is intentionally not restored (see saveRightPanelState).

  // Curve Width: look up the button whose canonical value fuzzy-matches
  // the layout's stored value. Block group signals so the idClicked
  // lambda doesn't refresh the previews on this passive resync.
  if (element.hasAttribute(u"width"_s) && width_button_group_ != nullptr) {
    bool ok = false;
    const double wanted = element.attribute(u"width"_s).toDouble(&ok);
    if (ok) {
      for (int i = 0; i < static_cast<int>(kWidthButtonSpecs.size()); ++i) {
        if (qFuzzyCompare(kWidthButtonSpecs[i].second, wanted)) {
          checkGroupButton(width_button_group_, i);
          break;
        }
      }
    }
  }

  // Curve Style: checkedId is the CurveStyle enum value; pass through.
  // Same preview-refresh suppression via QSignalBlocker.
  if (element.hasAttribute(u"style"_s) && style_button_group_ != nullptr) {
    bool ok = false;
    const int wanted = element.attribute(u"style"_s).toInt(&ok);
    if (ok) {
      checkGroupButton(style_button_group_, wanted);
    }
  }

  // Splitter sizes: only apply when the parsed list length matches the
  // splitter's current widget count. A mismatch means the splitter shape
  // changed across PJ4 versions; layout silently skips this piece.
  if (element.hasAttribute(u"splitter_sizes"_s) && ui_->rightToolbarSplitter != nullptr) {
    const QStringList parts = element.attribute(u"splitter_sizes"_s).split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.size() == ui_->rightToolbarSplitter->count()) {
      QList<int> sizes;
      sizes.reserve(parts.size());
      bool all_ok = true;
      for (const QString& p : parts) {
        bool ok = false;
        const int v = p.toInt(&ok);
        if (!ok) {
          all_ok = false;
          break;
        }
        sizes.push_back(v);
      }
      if (all_ok) {
        ui_->rightToolbarSplitter->setSizes(sizes);
      }
    }
  }
}

QDomElement MainWindow::saveChromeState(QDomDocument& doc) const {
  QDomElement element = doc.createElement(u"chrome_state"_s);

  // Persist only the main splitter's left-panel width. Panel visibility and the
  // timeline-strip height are fixed launch state (see the constructor), not saved.

  const auto join_sizes = [](QSplitter* splitter) {
    QStringList parts;
    const QList<int> sizes = splitter->sizes();
    parts.reserve(sizes.size());
    for (int s : sizes) {
      parts.push_back(QString::number(s));
    }
    return parts.join(QLatin1Char(','));
  };

  if (ui_->mainSplitter != nullptr) {
    element.setAttribute(u"main_splitter_sizes"_s, join_sizes(ui_->mainSplitter));
  }

  return element;
}

void MainWindow::restoreChromeState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != u"chrome_state"_s) {
    return;
  }

  // Apply only the main splitter's left-panel width (see saveChromeState).
  // Splitter sizes: only apply when parsed length matches the splitter's
  // widget count. Mismatch -> silent no-op.
  const auto apply_splitter = [](QSplitter* splitter, const QString& raw) {
    if (splitter == nullptr) {
      return;
    }
    const QStringList parts = raw.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.size() != splitter->count()) {
      return;
    }
    QList<int> sizes;
    sizes.reserve(parts.size());
    for (const QString& p : parts) {
      bool ok = false;
      const int v = p.toInt(&ok);
      if (!ok) {
        return;
      }
      sizes.push_back(v);
    }
    splitter->setSizes(sizes);
  };

  if (element.hasAttribute(u"main_splitter_sizes"_s)) {
    apply_splitter(ui_->mainSplitter, element.attribute(u"main_splitter_sizes"_s));
  }
}

MainWindow::MissingCurveChoice MainWindow::promptMissingCurves(const QStringList& names) {
  static constexpr int kMaxShown = 10;
  const int name_count = static_cast<int>(names.size());
  QString body = tr("The layout references %n curve(s) not present in the current data:", "", name_count);
  body += u"\n\n"_s;
  const int shown = std::min(name_count, kMaxShown);
  for (int i = 0; i < shown; ++i) {
    body += u"  • "_s + names[i] + u"\n"_s;
  }
  if (name_count > kMaxShown) {
    body += tr("  … and %n more\n", "", name_count - kMaxShown);
  }
  body += u"\n"_s;
  body += tr("Choose how to handle them:");

  // Themed prompt. "Remove from plots" takes the destructive role (purple ink);
  // there is deliberately no default button, because Remove drops all missing
  // curves from every plot in the layout and an accidental Enter should not
  // trigger it on a layout the user just opened. Esc / dismiss → Cancel.
  constexpr int kRemove = 0;
  const int choice = MessageBox::question(
      this, tr("Missing curves"), body,
      {{tr("Remove from plots"), MessageBox::kDestructiveRole}, {tr("Cancel"), MessageBox::kCancelRole}});
  return choice == kRemove ? MissingCurveChoice::kRemove : MissingCurveChoice::kCancel;
}

QDomDocument MainWindow::xmlSaveState() const {
  QDomDocument doc;
  doc.appendChild(doc.createProcessingInstruction(u"xml"_s, u"version='1.0' encoding='UTF-8'"_s));

  QDomElement root = doc.createElement(u"root"_s);
  root.setAttribute(u"format"_s, u"PlotJuggler"_s);
  root.setAttribute(u"pj4_version"_s, QString::number(kLayoutSchemaVersion));
  doc.appendChild(root);

  root.appendChild(ui_->tabbedPlotWidget->xmlSaveState(doc));

  // The global toolbar toggles (link X, show-point, legend, grid, dots, tracker
  // mode, 1:1 ratio, "Use time offset") are deliberately NOT serialized here.
  // They are app-wide UI preferences persisted in QSettings, not document state,
  // so neither a layout file nor any undo/redo snapshot (which shares this
  // serializer) carries them. This is also what lets the per-plot <range> store
  // ABSOLUTE time safely: the restored range frames the right instant no matter
  // which way the "Use time offset" toggle is set when the layout is loaded.

  // Data-Processor filters are workspace/data state, so they belong in THE snapshot
  // used by undo/redo (not just layout save). Without this, undoing across a filter's
  // creation drops its derived curve (its output topic is never recreated on restore).
  // saveLayoutToPath therefore no longer appends this separately.
  root.appendChild(saveDataProcessors(doc));
  return doc;
}

bool MainWindow::xmlLoadState(const QDomDocument& state_document) {
  const QDomElement root = state_document.documentElement();
  if (root.isNull() || root.tagName() != u"root"_s) {
    qCWarning(lcMain) << "No <root> element found at the top-level of the XML document";
    return false;
  }

  QDomElement main_tabbed_widget;
  for (auto tabbed = root.firstChildElement(u"tabbed_widget"_s); !tabbed.isNull();
       tabbed = tabbed.nextSiblingElement(u"tabbed_widget"_s)) {
    if (main_tabbed_widget.isNull()) {
      main_tabbed_widget = tabbed;
    }
    if (tabbed.attribute(u"parent"_s) == u"main_window"_s) {
      main_tabbed_widget = tabbed;
      break;
    }
  }
  if (main_tabbed_widget.isNull()) {
    qCWarning(lcMain) << "No <tabbed_widget> element found in XML document";
    return false;
  }

  const bool loaded = ui_->tabbedPlotWidget->xmlLoadState(main_tabbed_widget);
  if (!loaded) {
    return false;
  }
  wireExistingPlots();

  // The global toolbar toggles are no longer read from the layout/undo document
  // (see xmlSaveState) — they are app-wide QSettings preferences. Freshly loaded
  // plots still adopt the CURRENT global toggle state below, so a restored
  // layout stays visually consistent with the rest of the app without the
  // document dictating the toggle positions.
  forEachPlot([this](PlotWidget* plot) { applyGlobalToggles(plot); });
  applyShowPointsTo2DWidgets();
  return true;
}

void MainWindow::pushInitialUndoState() {
  undo_states_.clear();
  redo_states_.clear();
  undo_states_.push_back(xmlSaveState().toByteArray(2));
  undo_timer_.start();
  updateUndoRedoActions();
}

void MainWindow::resetUndoHistory() {
  // Re-baselining from the current state is what the initial push does. By the
  // time a removal call returns, the synchronous catalog subscriptions have
  // already pruned the widgets, so this snapshot is clean.
  pushInitialUndoState();
}

void MainWindow::pushUndoState(bool force_new_state) {
  const QByteArray state = xmlSaveState().toByteArray(2);
  if (!undo_states_.empty() && undo_states_.back() == state) {
    updateUndoRedoActions();
    return;
  }

  const bool should_coalesce =
      !force_new_state && undo_timer_.isValid() && undo_timer_.elapsed() < kUndoCoalesceMs && undo_states_.size() > 1;
  if (should_coalesce) {
    undo_states_.back() = state;
  } else {
    undo_states_.push_back(state);
  }

  while (undo_states_.size() > kMaxUndoStates) {
    undo_states_.pop_front();
  }
  redo_states_.clear();
  undo_timer_.restart();
  updateUndoRedoActions();
}

void MainWindow::updateUndoRedoActions() {
  if (undo_action_ != nullptr) {
    undo_action_->setEnabled(undo_states_.size() > 1);
  }
  if (redo_action_ != nullptr) {
    redo_action_->setEnabled(!redo_states_.empty());
  }
}

void MainWindow::bindEditorToPlot(PlotWidget* plot) {
  if (curve_editor_ != nullptr) {
    curve_editor_->setPlot(plot);
  }
  // The width/style buttons act on the editor's plot, so keep them
  // visibly disabled when there is none to act on.
  const bool enable = plot != nullptr;
  // Style/width are plot-level, so the toolbar must reflect the plot we just
  // bound to (not the last button clicked). The group ids are the LineWidth
  // index / CurveStyle enum value. Resync happens on every active-plot change;
  // an in-place state swap on the focused plot (xmlLoadState / paste) is not
  // covered, since the bound plot does not change there.
  if (width_button_group_ != nullptr) {
    for (auto* btn : width_button_group_->buttons()) {
      btn->setEnabled(enable);
    }
    if (plot != nullptr) {
      checkGroupButton(width_button_group_, static_cast<int>(plot->lineWidth()));
    }
  }
  if (style_button_group_ != nullptr) {
    for (auto* btn : style_button_group_->buttons()) {
      btn->setEnabled(enable);
    }
    if (plot != nullptr) {
      checkGroupButton(style_button_group_, static_cast<int>(plot->defaultCurveStyle()));
    }
  }
}

PlotWidget* MainWindow::firstPlotOfActiveTab() const {
  auto* docker = ui_->tabbedPlotWidget->currentTab();
  auto* dock = (docker != nullptr && docker->plotCount() > 0) ? docker->plotAt(0) : nullptr;
  return dock != nullptr ? dock->plotWidget() : nullptr;
}

DockWidget* MainWindow::activeFocusedDock() const {
  auto* docker = ui_->tabbedPlotWidget->currentTab();
  if (docker == nullptr) {
    return nullptr;
  }
  if (DockWidget* focused = docker->focusedDock(); focused != nullptr) {
    return focused;
  }
  return docker->plotCount() > 0 ? docker->plotAt(0) : nullptr;
}

void MainWindow::onDockFocused(DockWidget* dock) {
  bindEditorToPlot(dock != nullptr ? dock->plotWidget() : nullptr);

  // Placeholder docks (3-icon picker) and unknown widget kinds fall
  // through to the empty page — "nothing to configure" is the honest
  // signal when there is no curve, image, or scene to act on.
  QWidget* target = empty_dock_page_;
  Scene3DDockWidget* scene3d_dock = nullptr;
  SceneDockWidget* scene2d_dock = nullptr;
  if (dock != nullptr) {
    if (dock->plotWidget() != nullptr) {
      target = plot_config_page_;
    } else if (dock->objectWidget() != nullptr) {
      QWidget* obj = dock->objectWidget()->widget();
      if (auto* s2d = qobject_cast<Scene2DDockWidget*>(obj); s2d != nullptr) {
        target = scene2d_config_page_;
        scene2d_dock = s2d;
      } else if (auto* s3d = qobject_cast<Scene3DDockWidget*>(obj); s3d != nullptr) {
        target = scene3d_config_page_;
        scene3d_dock = s3d;
      }
    }
  }
  // Bind / unbind the config panels BEFORE switching the stack so the page is
  // already populated when it becomes visible. Passing nullptr when leaving a
  // scene dock detaches signal connections cleanly.
  if (scene2d_config_panel_ != nullptr) {
    scene2d_config_panel_->bindDock(scene2d_dock);
  }
  if (scene3d_config_panel_ != nullptr) {
    scene3d_config_panel_->bindDock(scene3d_dock);
  }
  if (right_panel_stack_ != nullptr) {
    right_panel_stack_->setCurrentWidget(target);
  }
}

void MainWindow::buildGlobalToolbar() {
  // globalToolbarWidget is a 24-px fixed column packed with Chart icons,
  // a 1-px divider, then Legend icons (4 corner picker + eye toggle).
  // No headers — labels would never fit in a 24-px column. Always
  // visible regardless of the "Toggle Right Panel" button state.
  auto* outer = qobject_cast<QVBoxLayout*>(ui_->globalToolbarWidget->layout());
  if (outer == nullptr) {
    return;
  }
  outer->setSpacing(0);
  outer->setContentsMargins(0, 0, 0, 0);

  auto add_button = [this, outer](const char* object_name, const char* icon_path, const char* tooltip) -> SvgButton* {
    auto* btn = new SvgButton(ui_->globalToolbarWidget);
    btn->setObjectName(QString::fromLatin1(object_name));
    btn->setIconPath(QString::fromLatin1(icon_path));
    btn->setExtent(chrome_metrics_.icon_size + chrome_metrics_.icon_padding, chrome_metrics_.icon_size);
    btn->setToolTip(tr(tooltip));
    outer->addWidget(btn);
    return btn;
  };

  // "Chart" group — global plot view toggles. Each button is checkable
  // and wires a slot that updates the matching member flag, persists to
  // QSettings, and calls forEachPlot. applying_state_ no-ops the slot
  // during bulk reload (xmlLoadState / undo / redo).
  button_link_ = add_button("buttonLink", ":/resources/svg/link.svg", "Link X axis");
  button_zoom_out_ = add_button("buttonZoomOut", ":/resources/svg/zoom_max.svg", "Zoom Out All");
  button_grid_ = add_button("buttonActivateGrid", ":/resources/svg/grid.svg", "Show/Hide the grid");
  // buttonTimeTracker cycles through 3 pre-rendered PNG icons by state, so it
  // skips the theme-tinted LoadSvg path baked into add_button. Built inline.
  button_time_tracker_ = new QToolButton(ui_->globalToolbarWidget);
  button_time_tracker_->setObjectName(u"buttonTimeTracker"_s);
  button_time_tracker_->setFocusPolicy(Qt::NoFocus);
  button_time_tracker_->setAutoRaise(true);
  button_time_tracker_->setFixedSize(24, 24);
  button_time_tracker_->setIconSize(QSize(20, 20));
  button_time_tracker_->setToolTip(tr("Cycle TimeTracker display: line only / line + value / line + value + name"));
  outer->addWidget(button_time_tracker_);
  updateTimeTrackerIcon();
  connect(button_time_tracker_, &QToolButton::clicked, this, &MainWindow::onTimeTrackerButtonClicked);

  // "Legend" group — single icon that combines a corner picker with a
  // show/hide toggle.
  //   * Left-click: enable at the current position if hidden, otherwise
  //                 cycle through corners (TR → TL → BL → BR → TR).
  //   * Right-click: toggle show/hide at the current position (no
  //                  cycle).
  // The icon always reflects the "current position": the active corner
  // while checked, or the saved corner that will be restored on the
  // next show while unchecked.
  if (legend_status_ != LegendStatus::kHidden) {
    previous_legend_corner_ = legend_status_;
  }
  const QByteArray initial_icon =
      legendCornerIcon(legend_status_ == LegendStatus::kHidden ? previous_legend_corner_ : legend_status_).toLatin1();
  button_legend_ = add_button(
      "buttonLegendPosition", initial_icon.constData(),
      "Legend position — left-click walks TR → TL → BL → BR, then unchecks at TR before the next "
      "cycle. Right-click toggles show/hide at the current corner.");
  button_legend_->setCheckable(true);
  button_legend_->setChecked(legend_status_ != LegendStatus::kHidden);
  connect(button_legend_, &QToolButton::clicked, this, &MainWindow::onLegendButtonClicked);
  // Right-click: enable customContextMenu so the click event reaches us
  // (Qt's default context menu policy would consume right-clicks for a
  // popup). No menu is shown — the signal is used solely as a
  // right-click hook.
  button_legend_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(button_legend_, &QToolButton::customContextMenuRequested, this, [this](const QPoint& /*pos*/) {
    if (legend_status_ == LegendStatus::kHidden) {
      setLegendStatus(previous_legend_corner_);
    } else {
      setLegendStatus(LegendStatus::kHidden);
    }
  });

  button_show_point_ = add_button("buttonShowpoint", ":/resources/svg/show_point.svg", "Show point in plot");
  button_ratio_ = add_button("buttonRatio", ":/resources/svg/ratio.svg", "Keep aspect ratio of XY plots (1:1)");
  button_dots_ = add_button("buttonDots", ":/resources/svg/scatter_plot.svg", "Show data point markers on curves");
  button_reference_point_ = add_button(
      "buttonReferencePoint", ":/resources/svg/reference_line.svg",
      "Drop a blue reference line at the playback position; values render as delta from there");
  button_t0_ = add_button(
      "buttonTimeOffset", ":/resources/svg/t0.svg",
      "Use time offset: show time relative to each dataset's start (axis begins at 0) instead of "
      "absolute time");
  auto make_checkable = [](QToolButton* btn, bool initial_checked) {
    btn->setCheckable(true);
    btn->setChecked(initial_checked);
  };
  make_checkable(button_link_, QSettings().value(u"MainWindow.buttonLink"_s, true).toBool());
  make_checkable(button_show_point_, show_points_);
  make_checkable(button_grid_, activate_grid_);
  make_checkable(button_ratio_, keep_ratio_);
  make_checkable(button_dots_, dots_);
  make_checkable(button_reference_point_, false);
  // Mirrors the frame the SessionManager already holds (applied from QSettings
  // at startup, before this runs).
  make_checkable(button_t0_, session_->sessionManager().useTimeOffset());
  // buttonZoomOut is a one-shot action, not a toggle — no make_checkable.
  connect(button_zoom_out_, &QToolButton::clicked, this, [this]() {
    linkedZoomOut();
    onUndoableChange();
  });
  connect(button_link_, &QToolButton::toggled, this, [](bool checked) {
    QSettings().setValue(u"MainWindow.buttonLink"_s, checked);
  });
  connect(button_show_point_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    show_points_ = checked;
    QSettings().setValue(u"MainWindow.buttonShowpoint"_s, checked);
    forEachPlot([checked](PlotWidget* plot) { plot->setShowPoints(checked); });
    applyShowPointsTo2DWidgets();
  });
  connect(button_grid_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    activate_grid_ = checked;
    QSettings().setValue(u"MainWindow.buttonActivateGrid"_s, checked);
    forEachPlot([checked](PlotWidget* plot) { plot->setGridVisible(checked); });
    syncFilterEditorPreviewDisplay();
    syncPanelPreviewDisplay();
  });
  connect(button_dots_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    dots_ = checked;
    QSettings().setValue(u"MainWindow.buttonDots"_s, checked);
    forEachPlot([this](PlotWidget* plot) {
      applyDots(plot);
      plot->replot();
    });
    onUndoableChange();
  });
  connect(button_ratio_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    keep_ratio_ = checked;
    QSettings().setValue(u"MainWindow.buttonRatio"_s, checked);
    forEachPlot([checked](PlotWidget* plot) { plot->setKeepRatioXY(checked); });
  });
  // Session-only state — not persisted to QSettings, not in xmlSaveState.
  // Captures the playback INSTANT at the moment of click (frame-invariant, via
  // toAbsolute); subsequent scrubbing does not move the reference, and a frame
  // change re-projects it rather than orphaning a stale display coordinate.
  connect(button_reference_point_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    reference_instant_ = checked ? std::optional<Timepoint>{toAbsolute(
                                       session_->playbackEngine().currentTime(),
                                       session_->sessionManager().displayOffset(representativeDatasetId()))}
                                 : std::nullopt;
    // Push to the plots and mirror it onto the Source Timeline's blue reference
    // needle (the controller bridges the playback frame into the Timeline frame).
    broadcastReferenceLine();
  });
  connect(button_t0_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    QSettings().setValue(u"MainWindow.useTimeOffset"_s, checked);
    onUseTimeOffsetToggled(checked);
  });

  // Trailing stretch pins the icon stack at the top of the column.
  outer->addStretch(1);

  // Resize the global toolbar column. Column width = button_extent +
  // 2 * layout_padding, with the same value pushed as contentsMargins
  // on the inner QVBoxLayout so the buttons grow inward to absorb the
  // padding instead of clipping.
  connect(this, &MainWindow::chromeMetricsChanged, ui_->globalToolbarWidget, [this](const ChromeMetrics& metrics) {
    const int button_extent = metrics.icon_size + metrics.icon_padding;
    const int column_width = button_extent + (2 * metrics.layout_padding);
    ui_->globalToolbarWidget->setMinimumWidth(column_width);
    ui_->globalToolbarWidget->setMaximumWidth(column_width);
    if (auto* layout = ui_->globalToolbarWidget->layout()) {
      layout->setContentsMargins(
          metrics.layout_padding, metrics.layout_padding, metrics.layout_padding, metrics.layout_padding);
      layout->setSpacing(metrics.layout_spacing);
    }
    for (auto* btn : ui_->globalToolbarWidget->findChildren<QToolButton*>()) {
      btn->setIconSize(QSize(metrics.icon_size, metrics.icon_size));
      btn->setFixedSize(button_extent, button_extent);
    }
  });
}

void MainWindow::buildTimelineAlignRail() {
  // A 24-px icon column on the right edge of the timeline panel that visually
  // continues the global toolbar above it (they line up when the right panel is
  // closed). Holds the Source Timeline alignment actions. The buttons mirror the
  // global toolbar's style (theme-tinted SVG, chrome-metrics sizing); the rail's
  // show/hide tracks the strip in applyBottomPanelConstraints.
  auto* outer = qobject_cast<QVBoxLayout*>(ui_->timelineAlignRail->layout());
  if (outer == nullptr) {
    return;
  }
  outer->setSpacing(0);
  outer->setContentsMargins(0, 0, 0, 0);

  auto add_button = [this, outer](const char* object_name, const char* icon_path, const char* tooltip) -> QToolButton* {
    auto* btn = new SvgButton(ui_->timelineAlignRail);
    btn->setObjectName(QString::fromLatin1(object_name));
    btn->setIconPath(QString::fromLatin1(icon_path));
    btn->setExtent(chrome_metrics_.icon_size + chrome_metrics_.icon_padding, chrome_metrics_.icon_size);
    btn->setToolTip(tr(tooltip));
    outer->addWidget(btn);
    return btn;
  };

  QToolButton* align_left = add_button(
      "buttonAlignStarts", ":/resources/svg/align_horizontal_left.svg",
      "Align sources: line every source's start up at the earliest start");

  QToolButton* align_right = add_button(
      "buttonAlignEnds", ":/resources/svg/align_horizontal_right.svg",
      "Align sources: line every source's end up at the latest end");

  // Zoom-out-horizontally: fit all source bars into the view (the timeline's
  // equivalent of the plot's "Zoom Out Horizontally", same icon). Replaces the
  // former align-centers action on the rail.
  QToolButton* zoom_fit =
      add_button("buttonTimelineFit", ":/resources/svg/zoom_horizontal.svg", "Zoom out to fit all sources");

  // "Snap to" toggle — when on, a dragged bar's start/end snaps to a neighbour's
  // edge (with a guide line). Checkable; on by default.
  QToolButton* snap_toggle = add_button(
      "buttonTimelineSnap", ":/resources/svg/transition_push.svg", "Snap to neighbouring dataset edges while dragging");
  snap_toggle->setCheckable(true);
  snap_toggle->setChecked(true);

  // Stretch pins the align/snap icons to the top of the content area; the reset
  // button below it sits at the BOTTOM of the rail.
  outer->addStretch(1);
  QToolButton* reset_all =
      add_button("buttonTimelineReset", ":/resources/svg/restart_alt.svg", "Reset all timeline changes");

  if (source_timeline_controller_ != nullptr) {
    connect(align_left, &QToolButton::clicked, source_timeline_controller_, &SourceTimelineController::alignStarts);
    connect(align_right, &QToolButton::clicked, source_timeline_controller_, &SourceTimelineController::alignEnds);
    connect(reset_all, &QToolButton::clicked, source_timeline_controller_, &SourceTimelineController::resetAll);
  }
  if (source_timeline_ != nullptr) {
    connect(zoom_fit, &QToolButton::clicked, source_timeline_, &Timeline::zoomToFit);
    connect(snap_toggle, &QToolButton::toggled, source_timeline_, &Timeline::setSnapEnabled);
    source_timeline_->setSnapEnabled(snap_toggle->isChecked());  // seed the initial state
  }

  // Match the global rail's column width + button sizing on chrome-metrics change,
  // so the two rails stay the same width and line up.
  connect(this, &MainWindow::chromeMetricsChanged, ui_->timelineAlignRail, [this](const ChromeMetrics& metrics) {
    const int button_extent = metrics.icon_size + metrics.icon_padding;
    const int column_width = button_extent + (2 * metrics.layout_padding);
    ui_->timelineAlignRail->setMinimumWidth(column_width);
    ui_->timelineAlignRail->setMaximumWidth(column_width);
    if (auto* layout = ui_->timelineAlignRail->layout()) {
      // The rail now sits beside the timeline strip only (the full-width playback
      // bar is above it), so the icons begin at the rail's own top — no inset.
      layout->setContentsMargins(
          metrics.layout_padding, metrics.layout_padding, metrics.layout_padding, metrics.layout_padding);
      layout->setSpacing(metrics.layout_spacing);
    }
    for (auto* btn : ui_->timelineAlignRail->findChildren<QToolButton*>()) {
      btn->setIconSize(QSize(metrics.icon_size, metrics.icon_size));
      btn->setFixedSize(button_extent, button_extent);
    }
  });
}

void MainWindow::buildLocalToolbar() {
  // Populates the plot-config page of the right-sidepanel stack: a
  // "Curve Width" header + flow-strip, a "Curve Style" header +
  // flow-strip, then the CurveEditor (added by the caller). Sections
  // wrap as the panel narrows; below ~72 px the headers hide and the
  // icon strips stack into a 1- or 2-col snap.
  auto* outer = qobject_cast<QVBoxLayout*>(plot_config_page_->layout());
  if (outer == nullptr) {
    return;
  }

  struct ToolSpec {
    const char* object_name;
    const char* icon_path;
    const char* tooltip;
    std::function<void()> on_click;
  };
  const auto on_width = [this](double w) { return [this, w]() { applyActivePlotWidth(w); }; };
  const auto on_style = [this](int s) { return [this, s]() { applyActivePlotStyle(s); }; };

  auto build_section = [this, outer](
                           const QString& heading, const QString& header_object_name,
                           const std::vector<ToolSpec>& specs) -> QWidget* {
    // Heading: the shared titlebar-tone band (pj_widgets SectionHeaderBand,
    // themed via the PJ--SectionHeaderBand QSS class rule). The objectName is
    // kept for widget-tree selectors; the chrome-metrics handler resizes it.
    auto* header = new SectionHeaderBand(heading, plot_config_page_);
    header->setObjectName(header_object_name);
    header->setFixedHeight(chrome_metrics_.icon_size + chrome_metrics_.icon_padding);
    outer->addWidget(header);

    // Icon strip: FlowLayout, spacing 0 so icons sit flush with each
    // other and the chrome bands. Each button is the standard chrome
    // 24×24 with a 20×20 icon, matching every other icon in the app.
    //
    // hasHeightForWidth(true) on the size policy is what lets the strip
    // grow tall when it has to wrap — without it, the parent QVBoxLayout
    // asks for sizeHint().height() (one row's worth, 24 px), and the
    // wrapped rows render below the strip's bottom edge and get clipped.
    // Each strip wraps independently inside its own section, so icons
    // never cross a section header.
    auto* strip = new QWidget(plot_config_page_);
    QSizePolicy strip_policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    strip_policy.setHeightForWidth(true);
    strip->setSizePolicy(strip_policy);
    auto* flow = new FlowLayout(strip, /*margin=*/0, /*h_spacing=*/0, /*v_spacing=*/0);
    for (const auto& spec : specs) {
      auto* btn = new SvgButton(strip);
      btn->setObjectName(QString::fromLatin1(spec.object_name));
      btn->setIconPath(QString::fromLatin1(spec.icon_path));
      btn->setExtent(chrome_metrics_.icon_size + chrome_metrics_.icon_padding, chrome_metrics_.icon_size);
      btn->setToolTip(tr(spec.tooltip));
      connect(btn, &QToolButton::clicked, this, spec.on_click);
      flow->addWidget(btn);
    }
    outer->addWidget(strip);
    return header;
  };

  curve_width_header_ = build_section(
      tr("Curve Width"), u"widgetLabelCurveWidth"_s,
      {
          {"globalWidth1_0", ":/resources/svg/line_width_1_0.svg", "Line width 1.0", on_width(1.0)},
          {"globalWidth1_5", ":/resources/svg/line_width_1_5.svg", "Line width 1.5", on_width(1.5)},
          {"globalWidth2_0", ":/resources/svg/line_width_2_0.svg", "Line width 2.0", on_width(2.0)},
          {"globalWidth3_0", ":/resources/svg/line_width_3_0.svg", "Line width 3.0", on_width(3.0)},
      });

  // Curve Width: same exclusive radio-group pattern as Curve Style.
  // Default is 1.0 (index 0); bindEditorToPlot resyncs the checked button to the
  // active plot's lineWidth(). The group's id is the LineWidth enum index (0..3);
  // the matching double is looked up from a parallel array so the click slot below
  // can call applyActivePlotWidth.
  width_button_group_ = new QButtonGroup(this);
  width_button_group_->setExclusive(true);
  const int initial_width_id = 0;
  for (int i = 0; i < static_cast<int>(kWidthButtonSpecs.size()); ++i) {
    auto* btn =
        curve_width_header_->parentWidget()->findChild<QToolButton*>(QString::fromLatin1(kWidthButtonSpecs[i].first));
    if (btn == nullptr) {
      continue;
    }
    btn->setCheckable(true);
    btn->setChecked(i == initial_width_id);
    width_button_group_->addButton(btn, i);
  }
  // Refresh any open preview panel when the width changes (the per-button slot
  // already applied it to the active plot via applyActivePlotWidth).
  connect(width_button_group_, &QButtonGroup::idClicked, this, [this](int /*width_id*/) {
    syncFilterEditorPreviewDisplay();
    syncPanelPreviewDisplay();
  });

  curve_style_header_ = build_section(
      tr("Curve Style"), u"widgetLabelCurveStyle"_s,
      {
          {"globalStyleLines", ":/resources/svg/style_lines.svg", "Lines",
           on_style(static_cast<int>(PlotWidgetBase::kLines))},
          {"globalStyleDots", ":/resources/svg/style_dots.svg", "Dots",
           on_style(static_cast<int>(PlotWidgetBase::kDots))},
          {"globalStyleLinesAndDots", ":/resources/svg/style_lines_and_dots.svg", "Lines and Dots",
           on_style(static_cast<int>(PlotWidgetBase::kLinesAndDots))},
          {"globalStyleSticks", ":/resources/svg/style_sticks.svg", "Sticks",
           on_style(static_cast<int>(PlotWidgetBase::kSticks))},
          {"globalStyleSteps", ":/resources/svg/style_steps.svg", "Steps (pre)",
           on_style(static_cast<int>(PlotWidgetBase::kSteps))},
          {"globalStyleStepsInverted", ":/resources/svg/style_steps_inverted.svg", "Steps (post)",
           on_style(static_cast<int>(PlotWidgetBase::kStepsInverted))},
      });

  // Curve Style buttons form an exclusive radio-style group: exactly one
  // is checked at any time. Default is "Lines" (kLines); bindEditorToPlot
  // resyncs the checked button to the active plot's defaultCurveStyle().
  // QButtonGroup with exclusive=true uses Qt's button-group semantics —
  // clicking the checked button is a no-op, clicking another checks it and
  // unchecks the previous.
  style_button_group_ = new QButtonGroup(this);
  style_button_group_->setExclusive(true);
  const int initial_style = static_cast<int>(PlotWidgetBase::kLines);
  const std::array<std::pair<const char*, int>, 6> style_button_specs{{
      {"globalStyleLines", static_cast<int>(PlotWidgetBase::kLines)},
      {"globalStyleDots", static_cast<int>(PlotWidgetBase::kDots)},
      {"globalStyleLinesAndDots", static_cast<int>(PlotWidgetBase::kLinesAndDots)},
      {"globalStyleSticks", static_cast<int>(PlotWidgetBase::kSticks)},
      {"globalStyleSteps", static_cast<int>(PlotWidgetBase::kSteps)},
      {"globalStyleStepsInverted", static_cast<int>(PlotWidgetBase::kStepsInverted)},
  }};
  for (const auto& [object_name, style_value] : style_button_specs) {
    auto* btn = curve_style_header_->parentWidget()->findChild<QToolButton*>(QString::fromLatin1(object_name));
    if (btn == nullptr) {
      continue;
    }
    btn->setCheckable(true);
    btn->setChecked(style_value == initial_style);
    style_button_group_->addButton(btn, style_value);
  }
  // Refresh any open preview panel when the style changes (the per-button slot
  // already applied it to the active plot via applyActivePlotStyle).
  connect(style_button_group_, &QButtonGroup::idClicked, this, [this](int /*style_value*/) {
    syncFilterEditorPreviewDisplay();
    syncPanelPreviewDisplay();
  });

  // Local-panel toolbar: each button stays button_extent square, the
  // two "Curve Width" / "Curve Style" header bands grow to band_extent
  // tall, and the section's flow-layout / strip gains layout_padding on
  // every edge so the icon strip doesn't sit flush with the panel edge.
  connect(this, &MainWindow::chromeMetricsChanged, ui_->localToolbarWidget, [this](const ChromeMetrics& metrics) {
    const int button_extent = metrics.icon_size + metrics.icon_padding;
    const int band_extent = button_extent + (2 * metrics.layout_padding);
    for (auto* btn : ui_->localToolbarWidget->findChildren<QToolButton*>()) {
      btn->setIconSize(QSize(metrics.icon_size, metrics.icon_size));
      btn->setFixedSize(button_extent, button_extent);
    }
    const QMargins band_margins(
        metrics.layout_padding, metrics.layout_padding, metrics.layout_padding, metrics.layout_padding);
    if (curve_width_header_ != nullptr) {
      curve_width_header_->setFixedHeight(band_extent);
      if (auto* layout = curve_width_header_->layout()) {
        layout->setContentsMargins(band_margins);
        layout->setSpacing(metrics.layout_spacing);
      }
    }
    if (curve_style_header_ != nullptr) {
      curve_style_header_->setFixedHeight(band_extent);
      if (auto* layout = curve_style_header_->layout()) {
        layout->setContentsMargins(band_margins);
        layout->setSpacing(metrics.layout_spacing);
      }
    }
    // Outer layout stays at 0 margins / 0 spacing (set once in
    // buildLocalToolbar). Each header band already applies layout_pad
    // internally; pushing it onto the outer too would double-pad the
    // bars — the LHS rootLayout follows the same rule.
  });
}

void MainWindow::applyActivePlotWidth(double width) {
  PlotWidget* plot = curve_editor_ != nullptr ? curve_editor_->plot() : nullptr;
  if (plot == nullptr) {
    return;
  }
  // Curve width is a plot-level property: setLineWidth re-pens every curve and
  // becomes the width any curve added later inherits. Map the toolbar's pixel
  // value to the LineWidth enum it mirrors (kWidthButtonSpecs index == enum).
  for (int i = 0; i < static_cast<int>(kWidthButtonSpecs.size()); ++i) {
    if (qFuzzyCompare(kWidthButtonSpecs[i].second, width)) {
      plot->setLineWidth(static_cast<LineWidth>(i));
      onUndoableChange();
      break;
    }
  }
}

void MainWindow::applyActivePlotStyle(int style) {
  PlotWidget* plot = curve_editor_ != nullptr ? curve_editor_->plot() : nullptr;
  if (plot == nullptr) {
    return;
  }
  // Curve style is a plot-level property: setDefaultStyle restyles every curve
  // and becomes the style any curve added later inherits (addCurve applies
  // curveStyle()).
  plot->setDefaultStyle(static_cast<PlotWidgetBase::CurveStyle>(style));
  onUndoableChange();
}

bool MainWindow::presentPanel(QWidget* panel) {
  if (panel == nullptr) {
    return false;
  }
  // A panel is already presented: dismiss it so launching a new toolbox/panel
  // replaces the open one instead of refusing. For a toolbox panel, close its
  // PanelEngine first (same teardown as the reject path) so its host/handle are
  // released; restoreCentralArea() then swaps the chart back and clears the
  // panel state, after which we present the new panel below. Non-toolbox panels
  // (null engine) just restore.
  if (current_panel_ != nullptr) {
    PanelEngine* previous_engine = current_panel_engine_;
    if (previous_engine != nullptr) {
      previous_engine->close();
    }
    restoreCentralArea();
    if (previous_engine != nullptr) {
      previous_engine->deleteLater();
    }
  }

  // The chart area (ui_->tabbedPlotWidget) lives as a direct child of a
  // QSplitter in MainWindow.ui. Swap the panel into the chart's splitter slot
  // and remember the slot so restoreCentralArea() can put the chart back.
  QWidget* chart = ui_->tabbedPlotWidget;
  panel_parent_ = chart->parentWidget();
  auto* splitter = qobject_cast<QSplitter*>(panel_parent_);
  if (splitter == nullptr) {
    qWarning("MainWindow::presentPanel: tabbedPlotWidget is not in a QSplitter");
    panel_parent_ = nullptr;
    return false;
  }
  panel_layout_index_ = splitter->indexOf(chart);
  if (panel_layout_index_ < 0) {
    qWarning("MainWindow::presentPanel: chart not in splitter");
    panel_parent_ = nullptr;
    return false;
  }
  // Swap the panel into the chart's exact splitter slot via replaceWidget so the
  // pane count stays constant and the saved size list still lines up (an
  // insert+hide would leave N+1 panes against an N-entry size list). replaceWidget
  // hands the chart back to us, reparented out of the splitter; keep it hidden so
  // restoreCentralArea can swap it back into the same slot.
  const QList<int> saved_sizes = splitter->sizes();
  QWidget* removed = splitter->replaceWidget(panel_layout_index_, panel);
  if (removed != chart) {
    qWarning("MainWindow::presentPanel: unexpected widget at chart slot; aborting swap");
    if (removed != nullptr) {
      splitter->replaceWidget(panel_layout_index_, removed);
    }
    panel_parent_ = nullptr;
    panel_layout_index_ = -1;
    return false;
  }
  chart->hide();
  panel->show();
  splitter->setSizes(saved_sizes);
  current_panel_ = panel;
  return true;
}

void MainWindow::restoreCentralArea() {
  if (current_panel_ == nullptr) {
    return;
  }
  auto* splitter = qobject_cast<QSplitter*>(panel_parent_);
  if (splitter != nullptr && panel_layout_index_ >= 0) {
    // Swap the chart back into its slot; replaceWidget removes the panel and
    // hands it back reparented out of the splitter (we delete it below).
    const QList<int> saved_sizes = splitter->sizes();
    splitter->replaceWidget(panel_layout_index_, ui_->tabbedPlotWidget);
    splitter->setSizes(saved_sizes);
  } else {
    qWarning("MainWindow::restoreCentralArea: panel_parent_ is no longer a splitter; chart not restored to slot");
  }
  ui_->tabbedPlotWidget->show();
  current_panel_->hide();
  current_panel_->setParent(nullptr);
  current_panel_->deleteLater();
  current_panel_ = nullptr;
  filter_editor_origin_ = nullptr;  // no Filter Editor preview to drive once the panel is gone
  // The engine (when this was a toolbox panel) is deleted by the caller that
  // tore it down (the onCloseRequested handler or presentPanel's replace path);
  // here we only drop our non-owning reference.
  current_panel_engine_ = nullptr;
  panel_layout_index_ = -1;
  panel_parent_ = nullptr;
}

void MainWindow::openEmbeddedConsole() {
  auto* view = new RasterStreamView(this);
  view->setKeyTranslator(&engineKeyForQtKey);
  if (!presentPanel(view)) {
    view->deleteLater();
    return;
  }
  connect(view, &RasterStreamView::sessionEnded, this, [this]() { restoreCentralArea(); });
  const QString dir = QCoreApplication::applicationDirPath() + u"/thirdparty/retro/"_s;
  QString helper = QStandardPaths::findExecutable(u"pj-raster-helper"_s, {dir});
  if (helper.isEmpty()) {
    helper = dir + u"pj-raster-helper"_s;
  }
  view->start(helper, dir + u"base.wad"_s);
}

void MainWindow::launchToolbox(const QString& plugin_id, const QString& initial_config) {
  // Surface every failure on the diagnostic channel (the same sink the toolbox's
  // own on_message uses below), not just stderr, so a user-initiated launch that
  // fails is visible in the UI instead of silently doing nothing.
  auto report_error = [this](const QString& source, const QString& detail) {
    if (diagnostic_history_ != nullptr) {
      diagnostic_history_->record(DiagnosticLevel::kError, source, u"toolbox"_s, detail);
    }
    qWarning("MainWindow::launchToolbox: %s", qPrintable(detail));
  };

  // 1. Find the toolbox in the catalog.
  const auto& toolboxes = session_->extensionCatalog().toolboxes();
  auto it = std::find_if(toolboxes.begin(), toolboxes.end(), [&plugin_id](const RuntimeToolboxPlugin& tb) {
    return QString::fromStdString(tb.id) == plugin_id;
  });
  if (it == toolboxes.end()) {
    report_error(plugin_id, tr("Cloud toolbox '%1' not found in catalog").arg(plugin_id));
    return;
  }

  // 2. Assemble the host services + toolbox handle into one owner whose member
  //    order *guarantees* teardown order. The plugin's destructor persists its
  //    state through the SettingsStoreHost, so the handle (declared last ->
  //    destroyed first) must be torn down while the host + settings backend it
  //    persists through are still alive. Lambda capture-destruction order is
  //    unspecified, so a struct (reverse-declaration destruction) is required.
  struct PanelSession {
    std::unique_ptr<QSettingsBackend> settings;
    std::unique_ptr<ServiceRegistryBuilder> builder;
    std::unique_ptr<ToolboxRuntimeHost> host;
    std::unique_ptr<DataProcessorsRuntimeHost> dp_host;
    std::shared_ptr<ToolboxHandle> handle;

    // Teardown order is load-bearing, so make it explicit here rather than relying
    // on member-declaration order alone: the plugin (handle) persists its state
    // through the settings backend in its destructor, so it must be torn down
    // first, then the service views (builder) into host/dp_host/settings, then the
    // bridge + host. dp_host holds only an external DataProcessorService& (session-
    // owned, outlives this), so it just has to outlive the builder views into it;
    // host holds a SettingsBackend&, so settings goes last. This survives a future
    // member reorder; the implicit reverse-declaration destruction that follows
    // only resets already-null pointers.
    ~PanelSession() {
      handle.reset();
      builder.reset();
      dp_host.reset();
      host.reset();
      settings.reset();
    }
  };
  auto session = std::make_shared<PanelSession>();
  session->settings = std::make_unique<QSettingsBackend>();
  session->builder = std::make_unique<ServiceRegistryBuilder>();

  const QString source = it->name.empty() ? plugin_id : QString::fromStdString(it->name);
  ToolboxRuntimeHost::Callbacks callbacks;
  const std::string plugin_id_std = plugin_id.toStdString();
  callbacks.on_data_changed = [this, plugin_id_std](std::vector<DatasetId> ingested_datasets) {
    session_->catalogModel().rebuildFromDatastore();
    // Bridge ingested kFrameTransforms object topics into the 3D scene's TF
    // buffers — the SAME step the file loader does (FileLoader.cpp ~713-727).
    // Without it a toolbox/cloud dataset registers its /tf object topic in the
    // ObjectStore but the per-dataset TransformBuffer stays empty, so the 3D
    // frame dropdown is blank and pointclouds (which resolve through TF) never
    // render. Runs AFTER the catalog rebuild so the object topics + their
    // render parsers are registered; ingest is idempotent (invalidate first so
    // a re-fetch of the same dataset re-ingests the new transforms).
    if (transform_service_ != nullptr) {
      for (const DatasetId id : ingested_datasets) {
        transform_service_->invalidateDataset(id);
        transform_service_->ingestFrameTransformsForDataset(id);
      }
    }
    // A parser-ingest import (the cloud connector's fetch) gets FOCUS
    // semantics: the timeline snaps to the imported data so a 10s snippet
    // plays back as 10s — the monotonic union would bury it inside whatever
    // range earlier fetches/loads established. Plain write-API toolboxes
    // (empty list) keep the union seeding.
    if (ingested_datasets.empty() || !session_->focusPlaybackOnDatasets(ingested_datasets)) {
      session_->seedPlaybackFromSession();
    }
    // Surface this plugin's transform outputs in the Custom Series panel (flat,
    // not in the main data tree).
    auto& dps = session_->sessionManager().dataProcessorService();
    for (const auto& recipe : dps.transformRecipes()) {
      if (recipe.owner_plugin != plugin_id_std) {
        continue;
      }
      for (const auto& output_name : recipe.outputs) {
        const QString out = QString::fromStdString(output_name);
        for (const auto& item : session_->catalogModel().items()) {
          if (item.topic_name == out) {
            ui_->curveListPanel->addCustomCurve(item.key, out);
            break;
          }
        }
      }
    }
  };
  callbacks.on_message = [this, source](PJ_toolbox_message_level_t level, std::string message) {
    if (diagnostic_history_ == nullptr) {
      return;
    }
    DiagnosticLevel diag = DiagnosticLevel::kInfo;
    if (level == PJ_TOOLBOX_MESSAGE_ERROR) {
      diag = DiagnosticLevel::kError;
    } else if (level == PJ_TOOLBOX_MESSAGE_WARNING) {
      diag = DiagnosticLevel::kWarning;
    }
    diagnostic_history_->record(diag, source, u"toolbox"_s, QString::fromStdString(message));
  };

  // Parser-ingest deps: the plugin catalog for ensureParserBinding lookups and
  // the SessionManager registrar for render-time object parsers. The registrar
  // may fire on the toolbox worker thread mid-download — marshal to the GUI
  // thread (same discipline as the host's own callbacks); the queued
  // registration always lands before the later-queued notify_data_changed
  // catalog rebuild. shared_ptr wrapper: std::function requires copyable.
  ToolboxRuntimeHost::ParserIngestDeps ingest_deps;
  ingest_deps.catalog = &session_->extensionCatalog();
  ingest_deps.register_object_parser = [this](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
    auto shared = std::make_shared<std::unique_ptr<MessageParserHandle>>(std::move(parser));
    QMetaObject::invokeMethod(
        this, [this, id, shared]() { session_->sessionManager().registerObjectTopicParser(id, std::move(*shared)); },
        Qt::AutoConnection);
  };

  session->host = std::make_unique<ToolboxRuntimeHost>(
      session_->sessionManager().dataEngine(), session_->sessionManager().objectStore(), *session->settings,
      std::move(callbacks), std::move(ingest_deps));
  session->host->registerServices(*session->builder);

  session->dp_host = std::make_unique<DataProcessorsRuntimeHost>(
      session_->sessionManager().dataProcessorService(), plugin_id.toStdString());
  session->dp_host->registerServices(*session->builder);

  // 3. Create the toolbox instance and bind it to the assembled services.
  session->handle = std::make_shared<ToolboxHandle>(it->library.createHandle());
  if (auto status = session->handle->bind(session->builder->view()); !status) {
    report_error(source, tr("Failed to bind toolbox '%1': %2").arg(source, QString::fromStdString(status.error())));
    return;
  }
  // Pre-populate for an in-place edit (Transform Editor pencil button): hand the
  // saved editor state to the toolbox before its dialog is built.
  if (!initial_config.isEmpty()) {
    (void)session->handle->loadConfig(initial_config.toStdString());
  }

  // 4. Host the toolbox's dialog in a PanelEngine.
  const PJ_borrowed_dialog_t borrowed = session->handle->getDialog();
  if (borrowed.vtable == nullptr || borrowed.ctx == nullptr) {
    report_error(source, tr("Toolbox '%1' returned no dialog").arg(source));
    return;
  }
  // The curve tree drags opaque catalog keys ("dataset:N/topic:M/column:K"); the
  // toolbox expects human field names ("topic/field"). CatalogModel owns that
  // mapping, so resolve dropped keys to names before they reach onItemsDropped.
  PanelEngineConfig panel_config;
  // Hand the panel a session + catalog so its chart_series previews render with the
  // full PlotWidget (grid/zoom/tracker/legend) — matching the native editor — instead
  // of falling back to the bare ChartPreviewWidget.
  panel_config.session = session_.get();
  panel_config.catalog = &session_->catalogModel();
  panel_config.catalog_key_resolver = [this](const std::string& key) -> std::string {
    auto descriptor = session_->catalogModel().curveDescriptor(QString::fromStdString(key));
    if (!descriptor) {
      return {};
    }
    return (descriptor->topic_name + "/" + descriptor->field_name).toStdString();
  };
  // Give chart containers the session + catalog so they render with the real
  // PJ4 PlotWidget (zoom / tracker / legend / grid), not the bare ChartPreviewWidget.
  panel_config.session = session_.get();
  panel_config.catalog = &session_->catalogModel();
  auto* engine = new PanelEngine(DialogHandle::fromBorrowed(borrowed), panel_config, this);
  QWidget* panel = engine->openPanel();
  if (panel == nullptr) {
    report_error(source, tr("Failed to build the panel UI for '%1'").arg(source));
    delete engine;
    return;
  }

  // QLineEdit (and friends) accept drops by default, so a drop landing on a field
  // gets delivered there and never bubbles to the panel-root DropEventFilter.
  // Clear acceptDrops on every descendant so any drop inside the panel reaches
  // the root filter, which maps it to the declared drop target.
  for (QWidget* child : panel->findChildren<QWidget*>()) {
    child->setAcceptDrops(false);
  }

  // 5. Close -> restore + teardown. The captured session keeps the services +
  //    plugin alive until the panel is gone; deleteLater defers the teardown
  //    (incl. the handle's worker-thread join) past any in-flight signals, and
  //    PanelSession's member order destroys the plugin before the settings host
  //    it persists through.
  engine->onCloseRequested([this, engine, session](const std::string& /*reason*/) {
    (void)session;
    restoreCentralArea();
    engine->deleteLater();
  });

  // 6. Present in the chart area.
  if (!presentPanel(panel)) {
    report_error(source, tr("Cannot show '%1': another panel is already open").arg(source));
    // presentPanel did not parent `panel` on the reject path, and the engine keeps
    // only a non-owning QPointer to it, so it would leak unless we delete it here.
    engine->close();
    panel->deleteLater();
    engine->deleteLater();
    return;
  }
  // presentPanel() succeeded; remember the engine so launching another toolbox
  // (or any panel) tears this one down first instead of being refused.
  current_panel_engine_ = engine;
  // Apply the app's grid/curve-style/width to the panel's embedded PlotWidget once
  // the panel engine has built it (deferred: the plot is created on the first tick).
  QTimer::singleShot(250, this, [this]() { syncPanelPreviewDisplay(); });
}

}  // namespace PJ
