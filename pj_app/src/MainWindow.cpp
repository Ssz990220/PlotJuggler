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
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPalette>
#include <QPushButton>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QSet>
#include <QSettings>
#include <QShortcut>
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

#include "DebugUi.h"
#include "FileLoader.h"
#include "LayoutXml.h"
#include "PreferencesDialog.h"
#include "RasterKeyMap.h"
#include "StreamingSourceManager.h"
#include "Theme.h"
#include "TitleBar.h"
#include "pj_base/dataset.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_marketplace/marketplace_window.hpp"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_plotting/CurveEditor.h"
#include "pj_plotting/CurveTracker.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_plugins/host/dialog_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_plugins/host/toolbox_handle.hpp"
#include "pj_plugins/host_qt/panel_engine.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DiagnosticHistory.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/IObjectViewer.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/QSettingsBackend.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/ToolboxRuntimeHost.h"
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#include "pj_scene2d_widgets/media_viewer_widget.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/transform_service.h"
#include "pj_widgets/FileDialog.h"
#include "pj_widgets/FlowLayout.h"
#include "pj_widgets/MessageBox.h"
#include "pj_widgets/RasterStreamView.h"
#include "pj_widgets/SectionHeaderBand.h"
#include "pj_widgets/SvgUtil.h"
#include "scene_object_classification.h"
#include "ui/AboutDialog.h"
#include "ui/CurveListPanel.h"
#include "ui/DiagnosticsDetailDialog.h"
#include "ui/LeftPanel.h"
#include "ui/Scene2DConfigPanel.h"
#include "ui/Scene3DConfigPanel.h"
#include "ui/TimelineWidget.h"
#include "ui_MainWindow.h"

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcMain, "pj.app.main")

constexpr auto kDefaultRegistryUrl =
    "https://raw.githubusercontent.com/PlotJuggler/pj-plugin-registry/"
    "refs/heads/development/registry.json";
constexpr auto kRegistryUrlSettingsKey = "Marketplace/registryUrl";
constexpr auto kPanelBottomExpandedKey = "MainWindow.panelBottomExpandedHeight";

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
constexpr int kLayoutSchemaVersion = 2;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr int kTestSampleCount = 1000;
constexpr double kTestDurationSeconds = 10.0;
constexpr int kResizeMargin = 6;
constexpr int kMaxRecentLayouts = 5;
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
  const char* settings_key;
  const char* icon_path_on;   // filled glyph — panel visible.
  const char* icon_path_off;  // unfilled glyph — panel hidden.
};

std::array<PanelToggle, 3> panelToggles(Ui::MainWindow* ui) {
  // Material's "Dock to Left" / "Dock to Right" glyphs fill the half
  // of the frame opposite to the side they nominally dock toward, so
  // the left-panel toggle reads correctly with the panel_right.svg
  // asset and vice versa.
  return {{
      {ui->tabbedPlotWidget->leftPanelButton(), ui->leftColumn, "MainWindow.panelLeftVisible",
       ":/resources/svg/panel_right.svg", ":/resources/svg/panel_right_off.svg"},
      // Toggle target is timelineStrip, NOT the whole bottomPanel — the
      // playback strip (timelineWidget) sits above the strip in the same
      // panel and must remain visible at all times. Resize of the
      // bottomPanel via the splitter handle grows the strip; the playback
      // keeps its fixed height (sizePolicy Fixed-vertical in MainWindow.ui).
      {ui->tabbedPlotWidget->bottomPanelButton(), ui->timelineStrip, "MainWindow.panelBottomVisible",
       ":/resources/svg/panel_bottom.svg", ":/resources/svg/panel_bottom_off.svg"},
      {ui->tabbedPlotWidget->rightPanelButton(), ui->localToolbarWidget, "MainWindow.panelRightVisible",
       ":/resources/svg/panel_left.svg", ":/resources/svg/panel_left_off.svg"},
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
  rhi_bootstrap->setObjectName(QStringLiteral("rhi_bootstrap"));
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
  // Recent Layouts is rebuilt lazily from QSettings on every
  // aboutToShow so it stays in sync with the most recent Load/Save.
  QMenu* file_menu = title_bar_->fileMenu();
  action_load_layout_ = file_menu->addAction(tr("Load Layout..."), this, &MainWindow::onLoadLayout);
  action_save_layout_ = file_menu->addAction(tr("Save Layout..."), this, &MainWindow::onSaveLayout);
  recent_layouts_menu_ = file_menu->addMenu(tr("Recent Layouts"));
  recent_layouts_menu_->setObjectName(QStringLiteral("PJMenu"));
  connect(recent_layouts_menu_, &QMenu::aboutToShow, this, &MainWindow::onRebuildRecentLayoutsMenu);
  file_menu->addSeparator();
  file_menu->addAction(ui_->actionMarketplace);
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
  help_menu->addAction(
      tr("Documentation"), this, []() { QDesktopServices::openUrl(QUrl(QStringLiteral("https://plotjuggler.io"))); });
  help_menu->addAction(tr("Report an Issue"), this, []() {
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/PlotJuggler/PJ4/issues")));
  });
  help_menu->addSeparator();
  installed_extensions_menu_ = help_menu->addMenu(tr("Installed Extensions"));
  installed_extensions_menu_->setObjectName(QStringLiteral("PJMenu"));
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
          // Classify the dropped object into a scene kind. kSceneEntities is both
          // 2D and 3D; is3d wins here (markers are primarily 3D), but a 2D dock
          // still accepts markers dropped onto an existing scene. Neither → "".
          resolved_kind = is3dSceneObjectType(seed->object_type)   ? QStringLiteral("scene3d")
                          : is2dSceneObjectType(seed->object_type) ? QStringLiteral("scene2d")
                                                                   : QString();
        }
        IDataWidget* widget = makeSceneDock(resolved_kind, dock_parent);
        if (widget == nullptr) {
          // Unknown kind: on a drop, tell the user why nothing appeared; on
          // restore (no seed) a null just means "not my kind" and is silent.
          if (seed != nullptr) {
            MessageBox::warning(
                this, tr("Cannot display topic"),
                tr("This object topic cannot be displayed (object_type=%1).").arg(static_cast<int>(seed->object_type)));
          }
          return nullptr;
        }
        if (seed == nullptr) {
          // Restore path: hand back the empty dock; PlotDocker then calls
          // xmlLoadState() to repopulate its topics and per-layer config.
          // Seed the playhead FIRST so the dock's last_tracker_ is set before
          // xmlLoadState's registerLayer runs: each restored layer then comes up
          // at the current playhead instead of its first sample. currentTimeChanged
          // only fires on changes, so a freshly-built widget never gets it
          // otherwise — and this same restore path rebuilds docks on undo/redo (M.1).
          widget->onTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
          return widget;
        }

        // Drop path: populate the first topic and apply view side-effects.
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

  // Keep data widgets coherent with catalog removals, whoever triggers them.
  // Each widget prunes its OWN pieces against the live catalog/store. One pass
  // per removal.
  connect(&session_->catalogModel(), &CatalogModel::cleared, this, [this]() { syncWidgetsToCatalog(); });
  connect(&session_->catalogModel(), &CatalogModel::itemsRemoved, this, [this](const QStringList&) {
    syncWidgetsToCatalog();
  });

  connect(ui_->curveListPanel, &CurveListPanel::trashRequested, this, &MainWindow::onCatalogTrashRequested);
  connect(ui_->curveListPanel, &CurveListPanel::removeDatasetRequested, this, &MainWindow::onRemoveDatasetRequested);
  connect(ui_->curveListPanel, &CurveListPanel::clearAllCurvesRequested, this, [this]() {
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
    session_->catalogModel().clearAll();
    resetUndoHistory();
  });

  QSettings settings;
  // Right-toolbar global view toggles persisted across sessions. Buttons
  // themselves are created later by buildGlobalToolbar(); we load state
  // first so the buttons can pick up the correct initial check state.
  show_points_ = settings.value(QStringLiteral("MainWindow.buttonShowpoint"), true).toBool();
  activate_grid_ = settings.value(QStringLiteral("MainWindow.buttonActivateGrid"), false).toBool();
  dots_ = settings.value(QStringLiteral("MainWindow.buttonDots"), false).toBool();
  tracker_info_ = static_cast<CurveTracker::Parameter>(
      settings.value(QStringLiteral("MainWindow.timeTrackerSetting"), static_cast<int>(CurveTracker::kValue)).toInt());
  keep_ratio_ = settings.value(QStringLiteral("MainWindow.buttonRatio"), true).toBool();
  legend_status_ = static_cast<LegendStatus>(
      settings.value(QStringLiteral("MainWindow.legendStatus"), static_cast<int>(LegendStatus::kHidden)).toInt());

  // Push the just-loaded toggle states into every plot already created by
  // wireExistingPlots(). New plots will pick this up via onPlotAdded.
  forEachPlot([this](PlotWidget* plot) { applyGlobalToggles(plot); });
  applyShowPointsTo2DWidgets();

  // Panel toggle buttons in the tab strip drive shell-level visibility
  // for the left column, the timeline strip, and the right toolbar.
  // The buttons are NOT checkable — visibility is communicated through
  // the icon glyph itself (filled = panel visible, outlined = hidden),
  // swapped via the "iconPath" dynamic property + applyIcons() so
  // theme-tinting flows through one codepath. At launch the left panel
  // is forced visible and the right panel forced hidden regardless of
  // the persisted value; the bottom strip restores its QSettings value
  // as before. In-session toggles persist normally — the override
  // applies only on startup.
  for (const PanelToggle& toggle : panelToggles(ui_)) {
    bool visible;
    if (toggle.target == ui_->leftColumn) {
      visible = true;
    } else if (toggle.target == ui_->localToolbarWidget) {
      visible = false;
    } else {
      visible = settings.value(QString::fromLatin1(toggle.settings_key), true).toBool();
    }
    toggle.target->setVisible(visible);
    toggle.button->setCheckable(false);
    toggle.button->setProperty("iconPath", QString::fromLatin1(visible ? toggle.icon_path_on : toggle.icon_path_off));
    // Bottom-strip toggle restore: if the strip is hidden, clamp the
    // bottom panel to the playback bar's height so the splitter can't
    // open empty space below the playback when the user drags it.
    if (toggle.target == ui_->timelineStrip && !visible) {
      ui_->bottomPanel->setMaximumHeight(ui_->timelineWidget->minimumHeight());
    }
    const QByteArray key{toggle.settings_key};
    QPushButton* button = toggle.button;
    QWidget* target = toggle.target;
    const QString icon_on = QString::fromLatin1(toggle.icon_path_on);
    const QString icon_off = QString::fromLatin1(toggle.icon_path_off);
    connect(button, &QPushButton::clicked, this, [this, key, button, target, icon_on, icon_off]() {
      const bool now_visible = !target->isVisible();
      target->setVisible(now_visible);
      const QString icon = now_visible ? icon_on : icon_off;
      button->setProperty("iconPath", icon);
      button->setIcon(LoadSvg(icon, theme_->currentTheme()));
      QSettings().setValue(QString::fromLatin1(key), now_visible);
      // Bottom-panel toggle: also collapse/restore the splitter so the
      // playback stays glued to the top with no empty gap below when
      // folded, and the strip's previous expanded height is preserved
      // across fold cycles. We additionally hard-clamp bottomPanel's
      // maximum height to the playback bar's height when the strip is
      // hidden, so the user can't drag the splitter handle further
      // down and re-introduce an empty gap below the playback.
      if (target == ui_->timelineStrip) {
        const QList<int> sizes = ui_->timelineSplitter->sizes();
        const int total = sizes[0] + sizes[1];
        const int playback_height = ui_->timelineWidget->minimumHeight();
        if (now_visible) {
          ui_->bottomPanel->setMaximumHeight(QWIDGETSIZE_MAX);
          const int expanded = QSettings().value(kPanelBottomExpandedKey, sizes[1]).toInt();
          ui_->timelineSplitter->setSizes({total - expanded, expanded});
        } else {
          QSettings().setValue(kPanelBottomExpandedKey, sizes[1]);
          ui_->bottomPanel->setMaximumHeight(playback_height);
          ui_->timelineSplitter->setSizes({total - playback_height, playback_height});
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

  connect(theme_.get(), &Theme::themeChanged, this, &MainWindow::onThemeChanged);
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

    if (!ui_->timelineStrip->isVisible()) {
      // Strip is hidden — keep the bottomPanel's max-height in sync
      // with the new playback height so the playback bar doesn't get
      // clipped against the stale cap from the toggle handler.
      ui_->bottomPanel->setMaximumHeight(playback_height);
    }
    // Pin the bottomPanel's minimum height to the new playback height
    // so the QSplitter is forced to honor it. Without this the inner
    // QVBoxLayout will shrink the playback bar if the splitter slot is
    // smaller than the new band_extent (Qt's QSplitter doesn't always
    // re-layout when a grandchild's min size changes — it caches the
    // bottomPanel's old minimumSizeHint until something invalidates it).
    ui_->bottomPanel->setMinimumHeight(playback_height);
    // Grow the bottom pane up to at least the playback's new height.
    // Preserve the user's current bottom-pane size if it's already
    // larger (e.g. the strip is open and dragged taller).
    const QList<int> sizes = ui_->timelineSplitter->sizes();
    if (sizes.size() == 2 && sizes[1] < playback_height) {
      const int total = sizes[0] + sizes[1];
      ui_->timelineSplitter->setSizes({total - playback_height, playback_height});
    }
    // Force a re-layout of the bottomPanel now that its child's height
    // pin and its own min height have changed. setMinimumHeight only
    // invalidates lazily; activate() runs the layout immediately so
    // the playback bar is sized correctly before the next paint.
    if (auto* bottom_layout = ui_->bottomPanel->layout()) {
      bottom_layout->invalidate();
      bottom_layout->activate();
    }
  });

  // Dev-only widget inspector: Ctrl+Shift+D = pesticide outline overlay,
  // Ctrl+Shift+Q = QSS debug border layer. Drop the include + this call +
  // DebugUi.{cpp,h} (plus the CMakeLists entries) to remove the feature.
  DebugUi::installInto(this, theme_.get());

  auto& playback = session_->playbackEngine();
  playback.setRange(displayRange(0.0, 10.0));
  ui_->timelineWidget->setPlaybackEngine(&playback);
  connect(&playback, &PlaybackEngine::currentTimeChanged, this, [this](double time) {
    forEachDock([time](DockWidget* dock) { dock->onTrackerTime(time); });
  });

  streaming_manager_ = std::make_unique<StreamingSourceManager>(
      session_->sessionManager(), session_->extensionCatalog(), session_->catalogModel(), this, this);
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

  // Streaming → playback range wiring. The slider range is scoped to the
  // active streaming dataset only — unioning with every catalog-visible
  // dataset (as AppSession::seedPlaybackFromSession does for file loads) can
  // stretch the slider across unrelated historical data, leaving the actual
  // streamed window as a sliver where intermediate scrub positions resolve to
  // "before first entry" or "at the live edge" with nothing in between.
  //
  // The slider stays untouched until a streaming topic is dropped into a view
  // (the object-widget factory above seeds range + playhead and sets
  // streaming_playback_seeded_). Merely subscribing to topics in the source
  // dialog must not move it. Once seeded, the playhead follows the live edge
  // while live — re-pinned to rangeMax on every ingest so the slider tracks
  // "now" instead of drifting backward as the window grows. On LeftPanel pause
  // (live=false) range + playhead freeze, so the user can rewind.
  connect(streaming_manager_.get(), &StreamingSourceManager::streamStarted, this, [this](DatasetId id) {
    active_streaming_dataset_id_ = id;
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
      &session_->sessionManager(), &SessionManager::samplesIngested, this, [this](const QVector<TopicId>&, bool live) {
        if (!streaming_playback_seeded_ || !live) {
          return;
        }
        if (const auto range = computeActiveStreamingRangeSec(); range.has_value()) {
          auto& engine = session_->playbackEngine();
          engine.setRange(displayRange(range->min, range->max));
          engine.setCurrentTime(displaySeconds(range->max));
        }
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
        return FileDialog::getOpenFileName(dialog_parent, caption, dir, filter, metrics_source);
      });
  connect(ui_->leftPanel, &LeftPanel::loadDataRequested, this, &MainWindow::onLoadDataRequested);
  connect(ui_->leftPanel, &LeftPanel::reloadDataRequested, this, &MainWindow::onReloadDataRequested);
  connect(ui_->leftPanel, &LeftPanel::cloudToolboxRequested, this, &MainWindow::launchToolbox);
  connect(file_loader_.get(), &FileLoader::fileLoaded, this, &MainWindow::onFileLoaded);
  // Track successful loads for the recent-files popup.
  connect(
      file_loader_.get(), &FileLoader::fileLoaded, this,
      [this](
          const QString& path, const QString& /*prefix*/, const QString& /*plugin_id*/,
          const QString& /*plugin_config_json*/) {
        QSettings recent_settings;
        QStringList recent = recent_settings.value(QStringLiteral("File/recent")).toStringList();
        recent.removeAll(path);
        recent.prepend(path);
        while (recent.size() > 5) {
          recent.removeLast();
        }
        recent_settings.setValue(QStringLiteral("File/recent"), recent);
        ui_->leftPanel->setRecentEnabled(true);
      });
  // Recent files reopen through the normal load flow, including the dialog.
  connect(ui_->leftPanel, &LeftPanel::recentFileSelected, this, [this](const QString& path) {
    file_loader_->loadFile(path, this);
  });
  // Enable the popup immediately when prior sessions recorded recent files.
  ui_->leftPanel->setRecentEnabled(!settings.value(QStringLiteral("File/recent")).toStringList().isEmpty());

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

  // Plot-layout changes from TabbedPlotWidget feed the undo stack.
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::undoableChange, this, &MainWindow::onUndoableChange);

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
  });
  bindEditorToPlot(firstPlotOfActiveTab());

  pushInitialUndoState();
  updateUndoRedoActions();
}

IDataWidget* MainWindow::makeSceneDock(const QString& kind, QWidget* parent) {
  // Single construct + wire site for object-widget docks, shared by the drop
  // and layout-restore paths. Wiring (session / transform service / theme) is
  // identical regardless of how the dock is later populated.
  if (kind == QStringLiteral("scene3d")) {
    auto* widget = new Scene3DDockWidget(parent);
    widget->setSessionManager(&session_->sessionManager());
    widget->setTransformService(transform_service_.get());
    widget->setSettings(app_settings_.get());
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
  if (kind == QStringLiteral("scene2d")) {
    auto* widget = new Scene2DDockWidget(parent);
    widget->setSessionManager(&session_->sessionManager());
    return widget;
  }
  return nullptr;
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
  ui_->leftPanel->setReloadEnabled(true);
}

void MainWindow::onCatalogTrashRequested(QStringList keys, bool covers_all) {
  CatalogModel& catalog = session_->catalogModel();
  if (covers_all) {
    // Free ObjectStore topics before the catalog wipe so the cleared()
    // subscription sees them gone and resets 2D viewers (symmetric with the
    // "Remove all Datasets" path). Keep lastLoadedSource for reload.
    session_->sessionManager().clearAllObjects();
    // TF buffers derive from the just-evicted objects; drop them with the data.
    if (transform_service_ != nullptr) {
      transform_service_->invalidateAll();
    }
    catalog.clearAll();
    resetUndoHistory();
    return;
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

void MainWindow::onRemoveDatasetRequested(DatasetId dataset_id) {
  // Confirmed removal: evict the dataset's objects, then tombstone its scalars
  // (kept in the engine). The catalog's cleared()/itemsRemoved subscriptions
  // then see the topics already gone and each widget prunes its own pieces
  // (curves / object layers).
  //
  // The TF buffer was built from these object topics; drop it so a later reload
  // re-ingests instead of skipping on the populated guard. Invalidate BEFORE
  // eviction (L.1/L.28): invalidateDataset's primary cleanup walks
  // listTopics(dataset_id), which is empty once eviction runs — so evicting
  // first would leave the per-topic cursor cleanup to the descriptor-empty
  // fallback sweep only.
  if (transform_service_ != nullptr) {
    transform_service_->invalidateDataset(dataset_id);
  }
  session_->sessionManager().evictDatasetObjects(dataset_id);
  session_->catalogModel().removeDataset(dataset_id);
  // Shrink the playback range to the remaining data right away — unless a
  // streaming dataset exists (the slider is scoped to the active stream).
  if (active_streaming_dataset_id_ == 0) {
    session_->seedPlaybackFromSession();
  }
  resetUndoHistory();
}

void MainWindow::onShowPreferencesDialog() {
  PreferencesDialog dlg(*theme_, this);
  dlg.exec();
}

void MainWindow::onShowAboutDialog() {
  AboutDialog dialog(this);
  dialog.exec();
}

void MainWindow::onThemeChanged(const QString& theme) {
  qApp->setStyleSheet(theme_->expandedQss());
  const bool light = theme.contains("light");
  const QColor tip_bg = light ? QColor(0xF5, 0xF5, 0xF5) : QColor(0x44, 0x44, 0x44);
  const QColor tip_fg = light ? QColor(0x11, 0x11, 0x11) : QColor(0xF0, 0xF0, 0xF0);
  QPalette p = qApp->palette();
  p.setColor(QPalette::ToolTipBase, tip_bg);
  p.setColor(QPalette::ToolTipText, tip_fg);
  qApp->setPalette(p);
  QPalette tp = QToolTip::palette();
  tp.setColor(QPalette::ToolTipBase, tip_bg);
  tp.setColor(QPalette::ToolTipText, tip_fg);
  tp.setColor(QPalette::Window, tip_bg);
  tp.setColor(QPalette::WindowText, tip_fg);
  QToolTip::setPalette(tp);
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
    toggle.button->setIcon(LoadSvg(resolved, theme));
  }
  // Title-bar menus: their QActions persist across theme changes, so
  // re-tint here.
  ui_->actionExit->setIcon(QIcon(LoadSvg(":/resources/svg/logout.svg", theme)));
  ui_->actionMarketplace->setIcon(QIcon(LoadSvg(":/resources/svg/archive.svg", theme)));
  if (action_preferences_ != nullptr) {
    action_preferences_->setIcon(QIcon(LoadSvg(":/resources/svg/settings_cog_light.svg", theme)));
  }
  if (action_load_layout_ != nullptr) {
    action_load_layout_->setIcon(QIcon(LoadSvg(":/resources/svg/dashboard_load.svg", theme)));
  }
  if (action_save_layout_ != nullptr) {
    action_save_layout_->setIcon(QIcon(LoadSvg(":/resources/svg/save_as.svg", theme)));
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
  connect(plot, &PlotWidget::statusMessageRequested, this, [this](const QString& message) {
    emitDiagnostic(DiagnosticLevel::kInfo, "Plot", "status", message);
  });
  plot->setTrackerPosition(toAxisDouble(session_->playbackEngine().currentTime()));
  applyGlobalToggles(plot);
  if (curve_editor_ != nullptr && curve_editor_->plot() == nullptr) {
    bindEditorToPlot(plot);
  }
}

namespace {
// SVG path for the legend button when the legend is at the given corner.
// kHidden is never a valid argument — callers must remap to the saved
// corner (previous_legend_corner_) before looking up the icon.
[[nodiscard]] QString legendCornerIcon(LegendStatus corner) {
  switch (corner) {
    case LegendStatus::kBottomRight:
      return QStringLiteral(":/resources/svg/position_bottom_right.svg");
    case LegendStatus::kBottomLeft:
      return QStringLiteral(":/resources/svg/position_bottom_left.svg");
    case LegendStatus::kTopRight:
      return QStringLiteral(":/resources/svg/position_top_right.svg");
    case LegendStatus::kTopLeft:
      return QStringLiteral(":/resources/svg/position_top_left.svg");
    case LegendStatus::kHidden:
      return QStringLiteral(":/resources/svg/position_top_right.svg");
  }
  return QStringLiteral(":/resources/svg/position_top_right.svg");
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
  QSettings().setValue(QStringLiteral("MainWindow.legendStatus"), static_cast<int>(legend_status_));
  if (button_legend_ != nullptr) {
    const bool visible = (position != LegendStatus::kHidden);
    button_legend_->setChecked(visible);
    const QString icon = legendCornerIcon(visible ? position : previous_legend_corner_);
    button_legend_->setProperty("iconPath", icon);
    button_legend_->setIcon(LoadSvg(icon, theme_->currentTheme()));
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
  plot->setReferenceLine(reference_time_);
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
      button_time_tracker_->setIcon(QIcon(QStringLiteral(":/style_light/line_tracker.png")));
      break;
    case CurveTracker::kValue:
      button_time_tracker_->setIcon(QIcon(QStringLiteral(":/style_light/line_tracker_1.png")));
      break;
    case CurveTracker::kValueName:
      button_time_tracker_->setIcon(QIcon(QStringLiteral(":/style_light/line_tracker_a.png")));
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
  QSettings().setValue(QStringLiteral("MainWindow.timeTrackerSetting"), static_cast<int>(tracker_info_));
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
  const auto from = dots_ ? PlotWidgetBase::kLines : PlotWidgetBase::kLinesAndDots;
  const auto to = dots_ ? PlotWidgetBase::kLinesAndDots : PlotWidgetBase::kLines;
  for (const auto& info : plot->curveList()) {
    if (info.curve != nullptr && PlotWidget::qwtStyleToCurveStyle(info.curve) == from) {
      plot->setCurveStyle(info.source_name, to);
    }
  }
}

void MainWindow::onPlotZoomChanged(PlotWidget* modified, QRectF rect) {
  if (!button_link_->isChecked()) {
    return;
  }

  forEachPlot([modified, rect](PlotWidget* plot) {
    if (plot == modified || plot->isEmpty() || plot->isXYPlot() || !plot->isZoomLinkEnabled()) {
      return;
    }
    QRectF peer_rect = plot->currentBoundingRect();
    peer_rect.setLeft(rect.left());
    peer_rect.setRight(rect.right());
    plot->setZoomRectangle(peer_rect, false);
    plot->onZoomOutVerticalTriggered(false);
    plot->replot();
  });
}

void MainWindow::onTrackerMovedFromWidget(QPointF point) {
  session_->playbackEngine().setCurrentTime(displaySeconds(point.x()));
}

std::optional<Range<double>> MainWindow::computeActiveStreamingRangeSec() const {
  if (active_streaming_dataset_id_ == 0) {
    return std::nullopt;
  }
  const auto reader = session_->sessionManager().createReader();
  const auto& object_store = session_->sessionManager().objectStore();

  Timestamp t_min = std::numeric_limits<Timestamp>::max();
  Timestamp t_max = std::numeric_limits<Timestamp>::min();
  bool found = false;

  for (const TopicId topic_id : reader.listTopics(active_streaming_dataset_id_)) {
    const auto metadata = reader.getMetadata(topic_id);
    if (metadata.has_value() && metadata->total_row_count > 0) {
      t_min = std::min(t_min, metadata->time_range_min);
      t_max = std::max(t_max, metadata->time_range_max);
      found = true;
    }
  }
  for (const ObjectTopicId object_topic_id : object_store.listTopics(active_streaming_dataset_id_)) {
    if (object_store.entryCount(object_topic_id) > 0) {
      const auto [obj_min, obj_max] = object_store.timeRange(object_topic_id);
      t_min = std::min(t_min, obj_min);
      t_max = std::max(t_max, obj_max);
      found = true;
    }
  }
  if (!found) {
    return std::nullopt;
  }

  return Range<double>{
      .min = static_cast<double>(t_min) / kNanosecondsPerSecond,
      .max = static_cast<double>(t_max) / kNanosecondsPerSecond};
}

void MainWindow::seedStreamingPlaybackFromDrop() {
  if (active_streaming_dataset_id_ == 0) {
    return;
  }
  streaming_playback_seeded_ = true;
  if (const auto range = computeActiveStreamingRangeSec(); range.has_value()) {
    auto& engine = session_->playbackEngine();
    engine.setRange(displayRange(range->min, range->max));
    engine.setCurrentTime(displaySeconds(range->max));
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
  QSettings settings;
  settings.setValue(QStringLiteral("MainWindow.buttonLink"), button_link_->isChecked());
  QMainWindow::closeEvent(event);
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
  const QString default_suffix = QString::fromLatin1(LayoutXml::kLayoutExtension).mid(1);

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
  const QString save_path = LayoutXml::ensureLayoutExtension(result.path);
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

void MainWindow::onRebuildRecentLayoutsMenu() {
  if (recent_layouts_menu_ == nullptr) {
    return;
  }
  recent_layouts_menu_->clear();
  const QStringList recent = recentLayouts();
  if (recent.isEmpty()) {
    QAction* placeholder = recent_layouts_menu_->addAction(tr("(no recent layouts)"));
    placeholder->setEnabled(false);
    return;
  }
  for (const QString& path : recent) {
    const QString shown = QFileInfo(path).fileName();
    QAction* action = recent_layouts_menu_->addAction(shown);
    action->setToolTip(path);
    connect(action, &QAction::triggered, this, [this, path]() { onLoadRecentLayout(path); });
  }
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
      const QString label = version.isEmpty() ? name : QStringLiteral("%1 (%2)").arg(name, version);
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
    bool is_cloud = false;
    if (manifest.is_object()) {
      if (auto it = manifest.find("tags"); it != manifest.end() && it->is_array()) {
        for (const auto& tag : *it) {
          if (tag.is_string() && tag.get<std::string>() == "cloud") {
            is_cloud = true;
            break;
          }
        }
      }
    }
    if (is_cloud) {
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
  const int version = root.attribute(QStringLiteral("pj4_version"), QStringLiteral("0")).toInt(&version_ok);
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
  const QString binding = root.attribute(QStringLiteral("binding"), QStringLiteral("source"));
  const QDir layout_dir(QFileInfo(path).absoluteDir());
  const LayoutXml::DataSourceRef replay = LayoutXml::extractDataSource(doc, layout_dir);
  if (binding != QStringLiteral("generic") && !replay.resolved_path.isEmpty()) {
    const auto current_source = session_->sessionManager().lastLoadedSource();
    // A remembered source counts as loaded only while the catalog has data.
    const bool same_source_loaded = current_source.has_value() &&
                                    LayoutXml::isSamePath(current_source->path, replay.resolved_path) &&
                                    !session_->catalogModel().isEmpty();
    if (same_source_loaded) {
      // The referenced file is already loaded; nothing to reload.
    } else if (!QFileInfo::exists(replay.resolved_path)) {
      emitDiagnostic(
          DiagnosticLevel::kWarning, "Layout", "data-source-missing",
          tr("Layout's data source '%1' does not exist on disk; applying to current data.").arg(replay.resolved_path));
    } else {
      QMessageBox box(this);
      box.setIcon(QMessageBox::Question);
      box.setWindowTitle(tr("Load Layout"));
      box.setText(tr("This layout was saved with data source:\n  %1\n\nReload it, or apply the layout to the "
                     "currently loaded data?")
                      .arg(replay.resolved_path));
      QPushButton* reload_btn = box.addButton(tr("Reload original"), QMessageBox::AcceptRole);
      // "Use current data" is the fall-through: neither cancel nor reload.
      box.addButton(tr("Use current data"), QMessageBox::AcceptRole);
      QPushButton* cancel_btn = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
      box.setDefaultButton(reload_btn);
      box.exec();
      if (box.clickedButton() == cancel_btn) {
        return;
      }
      if (box.clickedButton() == reload_btn) {
        LoadHints hints{
            .expected_plugin_id = replay.plugin_id,
            .preset_config_json = replay.plugin_config_json,
            .skip_dialog = !replay.plugin_id.isEmpty() && !replay.plugin_config_json.isEmpty(),
            .prefer_reuse = true,
        };
        // FileLoader shows its own error dialog on failure; fall through and
        // let the unresolved-curve handling below catch an empty load.
        file_loader_->loadFile(replay.resolved_path, this, hints);
      }
    }
  }

  // 3. Pick the target dataset and rebind every curve's stable topic+field
  // path to that dataset's concrete keys. A layout built on one recording
  // thus reuses on a similar one (same topics/fields). Paths the dataset
  // can't provide are surfaced via the missing-curve prompt.
  const auto datasets = session_->catalogModel().datasets();
  if (datasets.empty()) {
    MessageBox::warning(
        this, tr("Load Layout"), tr("No data is loaded. Open a data source before applying this layout."));
    return;
  }
  const std::optional<DatasetId> target = chooseActiveDataset(datasets);
  if (!target.has_value()) {
    return;  // user cancelled the dataset chooser
  }
  const DatasetId target_id = *target;
  const QList<LayoutXml::SeriesPath> unresolved =
      LayoutXml::rebindCurveKeys(doc, [this, target_id](const LayoutXml::SeriesPath& p) -> std::optional<QString> {
        const auto descriptor = session_->catalogModel().descriptorForPath(target_id, p.topic, p.field);
        return descriptor.has_value() ? std::optional<QString>(descriptor->name) : std::nullopt;
      });
  if (!unresolved.isEmpty()) {
    QStringList shown;
    shown.reserve(unresolved.size());
    for (const LayoutXml::SeriesPath& sp : unresolved) {
      shown.push_back(sp.display());
    }
    switch (promptMissingCurves(shown)) {
      case MissingCurveChoice::kCancel:
        return;
      case MissingCurveChoice::kRemove:
        LayoutXml::stripUnresolvedCurves(doc);
        break;
    }
  }

  // 4. Apply. If step 2 already reloaded a data source AND this fails,
  // we leave the world half-mutated: the new data is loaded but the
  // user's plots/panels never came back. Rolling back a synchronous
  // ingest is not currently feasible (FileLoader has no "unload" API
  // and the DataEngine doesn't support transactional commits). The
  // warning is the best signal we can offer.
  if (!xmlLoadState(doc)) {
    MessageBox::warning(
        this, tr("Load Layout"),
        tr("Layout was parsed but could not be applied. If a data source was reloaded, it is still loaded."));
    return;
  }

  // 4a. Restore curve-list content state (filters + show_topics/show_values toggles).
  ui_->curveListPanel->restoreListState(doc.documentElement().firstChildElement(QStringLiteral("curve_list_state")));

  // 4b. Restore right-panel state.
  restoreRightPanelState(doc.documentElement().firstChildElement(QStringLiteral("right_panel_state")));

  // 4c. Restore LeftPanel Sources tab + streaming controls.
  ui_->leftPanel->restoreSourcesState(doc.documentElement().firstChildElement(QStringLiteral("left_panel_state")));

  // 4d. Restore chrome state (panel visibilities + splitter sizes).
  restoreChromeState(doc.documentElement().firstChildElement(QStringLiteral("chrome_state")));

  // 5. Recent files + diagnostic
  recordRecentLayout(path);
  emitDiagnostic(DiagnosticLevel::kInfo, "Layout", "loaded", tr("Loaded layout: %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::saveLayoutToPath(const QString& path, bool include_data_source) {
  QDomDocument doc = xmlSaveState();
  // Record the binding intent so load knows whether to reload the original
  // file (source-bound) or adopt the currently-loaded dataset (generic). The
  // data-source element below is only embedded for source-bound layouts.
  doc.documentElement().setAttribute(
      QStringLiteral("binding"), include_data_source ? QStringLiteral("source") : QStringLiteral("generic"));
  if (include_data_source) {
    const QDir layout_dir(QFileInfo(path).absoluteDir());
    QDomElement ds = appendDataSourceElement(doc, layout_dir);
    if (!ds.isNull()) {
      doc.documentElement().appendChild(ds);
    }
  }
  // Always save right-panel state — pure UI chrome, no privacy cost,
  // not gated by Save Data Source.
  doc.documentElement().appendChild(saveRightPanelState(doc));
  // Always save the remaining widget state — pure UI chrome, not gated
  // by Save Data Source.
  doc.documentElement().appendChild(ui_->leftPanel->saveSourcesState(doc));
  doc.documentElement().appendChild(ui_->curveListPanel->saveListState(doc));
  doc.documentElement().appendChild(saveChromeState(doc));
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

void MainWindow::recordRecentLayout(const QString& path) {
  QStringList recent = recentLayouts();
  recent.removeAll(path);  // dedupe — most-recent-first
  recent.prepend(path);
  while (recent.size() > kMaxRecentLayouts) {
    recent.removeLast();
  }
  QSettings().setValue(kRecentLayoutsKey, recent);
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

void MainWindow::onUndoableChange() {
  if (applying_state_) {
    return;
  }
  pushUndoState();
}

void MainWindow::rebindToCurrentSession(QDomDocument& doc) {
  const auto datasets = session_->catalogModel().datasets();
  // Unresolved paths are intentionally ignored here: undo/redo restores
  // silently (no missing-curve prompt), and a curve whose data is gone is
  // simply dropped on restore.
  (void)LayoutXml::rebindCurveKeys(doc, [this, &datasets](const LayoutXml::SeriesPath& p) -> std::optional<QString> {
    for (const auto& [id, name] : datasets) {
      (void)name;
      if (const auto descriptor = session_->catalogModel().descriptorForPath(id, p.topic, p.field)) {
        return descriptor->name;
      }
    }
    return std::nullopt;
  });
}

void MainWindow::onUndo() {
  if (undo_states_.size() <= 1) {
    return;
  }

  redo_states_.push_back(undo_states_.back());
  undo_states_.pop_back();
  QDomDocument doc;
  doc.setContent(undo_states_.back());
  rebindToCurrentSession(doc);
  const bool loaded = [&] {
    QScopedValueRollback guard(applying_state_, true);
    return xmlLoadState(doc);
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
  rebindToCurrentSession(doc);
  const bool loaded = [&] {
    QScopedValueRollback guard(applying_state_, true);
    return xmlLoadState(doc);
  }();

  if (!loaded) {
    statusBar()->showMessage(tr("Unable to restore redo state"), 3000);
  }
  undo_timer_.restart();
  updateUndoRedoActions();
}

QDomElement MainWindow::appendDataSourceElement(QDomDocument& doc, const QDir& layout_dir) const {
  const auto src = session_->sessionManager().lastLoadedSource();
  // Do not save a data-source reference after the catalog was cleared.
  if (!src.has_value() || session_->catalogModel().isEmpty()) {
    return QDomElement();
  }
  QDomElement wrapper = doc.createElement(QStringLiteral("previouslyLoaded_Datafiles"));
  QDomElement file_info = doc.createElement(QStringLiteral("fileInfo"));

  const QFileInfo info(src->path);
  const QString abs = info.absoluteFilePath();
  const QString rel = layout_dir.relativeFilePath(abs);
  // Prefer the relative form when the data lives at or beneath the layout
  // dir; fall back to absolute when it escapes. This diverges from PJ3,
  // which always stores relative — PJ4 avoids brittle ../.. paths so that
  // moving a layout file doesn't silently break the data reference.
  // A relative path is a "subpath" only when Qt's relativeFilePath did
  // NOT emit a "../" prefix or the literal ".." path. The earlier check
  // (`!rel.startsWith("..")`) would misclassify legitimate filenames
  // like "..foo" or "..bar/data.csv" as escaping the dir.
  const bool is_subpath = rel != QStringLiteral("..") && !rel.startsWith(QStringLiteral("../"));
  file_info.setAttribute(QStringLiteral("filename"), is_subpath ? rel : abs);
  file_info.setAttribute(QStringLiteral("prefix"), src->prefix);

  // Emit the plugin sub-element whenever the plugin id is known. An
  // empty saveConfig payload is legitimate (some plugins have no
  // user-tunable state) and must NOT cause us to skip — otherwise
  // those plugins would re-prompt on every layout reload. Empty
  // plugin_id means the loader didn't capture a plugin (legacy path
  // or saveConfig failure); only that case skips the child.
  if (!src->plugin_id.isEmpty()) {
    QDomElement plugin = doc.createElement(QStringLiteral("plugin"));
    plugin.setAttribute(QStringLiteral("ID"), src->plugin_id);
    // CDATA so the JSON survives round-tripping without XML escape mangling.
    // appendJsonAsCdata splits across multiple CDATA sections when the JSON
    // contains a literal "]]>" sequence (otherwise it'd terminate the
    // CDATA early and corrupt the layout file).
    LayoutXml::appendJsonAsCdata(doc, plugin, src->plugin_config_json);
    file_info.appendChild(plugin);
  }

  wrapper.appendChild(file_info);
  return wrapper;
}

QDomElement MainWindow::saveRightPanelState(QDomDocument& doc) const {
  QDomElement element = doc.createElement(QStringLiteral("right_panel_state"));

  if (ui_->localToolbarWidget != nullptr) {
    element.setAttribute(
        QStringLiteral("visible"),
        ui_->localToolbarWidget->isVisible() ? QStringLiteral("true") : QStringLiteral("false"));
  }

  // Curve Width: the radio's checkedId is a loop index (0..3); map it
  // back to the canonical double via kWidthButtonSpecs so we encode the
  // value (rebuild-stable) rather than the index (depends on button
  // declaration order).
  if (width_button_group_ != nullptr) {
    const int id = width_button_group_->checkedId();
    if (id >= 0 && id < static_cast<int>(kWidthButtonSpecs.size())) {
      element.setAttribute(QStringLiteral("width"), QString::number(kWidthButtonSpecs[id].second, 'g'));
    }
  }

  // Curve Style: the radio's checkedId IS the CurveStyle enum value
  // (assigned at button-group construction). Stable across rebuilds.
  if (style_button_group_ != nullptr) {
    const int id = style_button_group_->checkedId();
    if (id >= 0) {
      element.setAttribute(QStringLiteral("style"), QString::number(id));
    }
  }

  if (ui_->rightToolbarSplitter != nullptr) {
    QStringList parts;
    const QList<int> sizes = ui_->rightToolbarSplitter->sizes();
    parts.reserve(sizes.size());
    for (int s : sizes) {
      parts.push_back(QString::number(s));
    }
    element.setAttribute(QStringLiteral("splitter_sizes"), parts.join(QLatin1Char(',')));
  }

  return element;
}

void MainWindow::applyPanelVisibility(QWidget* target, bool wanted) {
  if (target == nullptr || target->isVisible() == wanted) {
    return;
  }
  const auto toggles = panelToggles(ui_);
  for (const PanelToggle& t : toggles) {
    if (t.target != target) {
      continue;
    }
    t.target->setVisible(wanted);
    const QString icon = QString::fromLatin1(wanted ? t.icon_path_on : t.icon_path_off);
    t.button->setProperty("iconPath", icon);
    t.button->setIcon(LoadSvg(icon, theme_->currentTheme()));
    return;
  }
}

void MainWindow::restoreRightPanelState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != QStringLiteral("right_panel_state")) {
    return;
  }

  // Visibility: re-create the side-effects of a button click (toggle
  // target + swap icon) without writing to QSettings. Diff-against-
  // current avoids needless flips.
  if (element.hasAttribute(QStringLiteral("visible"))) {
    const bool wanted = element.attribute(QStringLiteral("visible")) == QStringLiteral("true");
    applyPanelVisibility(ui_->localToolbarWidget, wanted);
  }

  // Curve Width: look up the button whose canonical value fuzzy-matches
  // the layout's stored value. Block group signals so the idClicked
  // lambda doesn't rewrite QSettings.
  if (element.hasAttribute(QStringLiteral("width")) && width_button_group_ != nullptr) {
    bool ok = false;
    const double wanted = element.attribute(QStringLiteral("width")).toDouble(&ok);
    if (ok) {
      for (int i = 0; i < static_cast<int>(kWidthButtonSpecs.size()); ++i) {
        if (qFuzzyCompare(kWidthButtonSpecs[i].second, wanted)) {
          if (auto* btn = width_button_group_->button(i)) {
            const QSignalBlocker blocker(width_button_group_);
            btn->setChecked(true);
          }
          break;
        }
      }
    }
  }

  // Curve Style: checkedId is the CurveStyle enum value; pass through.
  // Same QSettings-suppression via QSignalBlocker.
  if (element.hasAttribute(QStringLiteral("style")) && style_button_group_ != nullptr) {
    bool ok = false;
    const int wanted = element.attribute(QStringLiteral("style")).toInt(&ok);
    if (ok) {
      if (auto* btn = style_button_group_->button(wanted)) {
        const QSignalBlocker blocker(style_button_group_);
        btn->setChecked(true);
      }
    }
  }

  // Splitter sizes: only apply when the parsed list length matches the
  // splitter's current widget count. A mismatch means the splitter shape
  // changed across PJ4 versions; layout silently skips this piece.
  if (element.hasAttribute(QStringLiteral("splitter_sizes")) && ui_->rightToolbarSplitter != nullptr) {
    const QStringList parts =
        element.attribute(QStringLiteral("splitter_sizes")).split(QLatin1Char(','), Qt::SkipEmptyParts);
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
  QDomElement element = doc.createElement(QStringLiteral("chrome_state"));

  if (ui_->leftColumn != nullptr) {
    element.setAttribute(
        QStringLiteral("left_visible"),
        ui_->leftColumn->isVisible() ? QStringLiteral("true") : QStringLiteral("false"));
  }
  if (ui_->timelineStrip != nullptr) {
    element.setAttribute(
        QStringLiteral("bottom_visible"),
        ui_->timelineStrip->isVisible() ? QStringLiteral("true") : QStringLiteral("false"));
  }

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
    element.setAttribute(QStringLiteral("main_splitter_sizes"), join_sizes(ui_->mainSplitter));
  }
  if (ui_->timelineSplitter != nullptr) {
    element.setAttribute(QStringLiteral("timeline_splitter_sizes"), join_sizes(ui_->timelineSplitter));
  }

  return element;
}

void MainWindow::restoreChromeState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != QStringLiteral("chrome_state")) {
    return;
  }

  if (element.hasAttribute(QStringLiteral("left_visible"))) {
    const bool wanted = element.attribute(QStringLiteral("left_visible")) == QStringLiteral("true");
    applyPanelVisibility(ui_->leftColumn, wanted);
  }
  if (element.hasAttribute(QStringLiteral("bottom_visible"))) {
    const bool wanted = element.attribute(QStringLiteral("bottom_visible")) == QStringLiteral("true");
    applyPanelVisibility(ui_->timelineStrip, wanted);
    // Replay the bottom-panel splitter clamp the click handler does so
    // the user can't drag the splitter handle to re-introduce empty
    // space below the playback bar when the strip is hidden. timeline_
    // splitter_sizes (if present) is applied below and supersedes this.
    if (!wanted && ui_->bottomPanel != nullptr && ui_->timelineWidget != nullptr) {
      ui_->bottomPanel->setMaximumHeight(ui_->timelineWidget->minimumHeight());
    } else if (wanted && ui_->bottomPanel != nullptr) {
      ui_->bottomPanel->setMaximumHeight(QWIDGETSIZE_MAX);
    }
  }

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

  if (element.hasAttribute(QStringLiteral("main_splitter_sizes"))) {
    apply_splitter(ui_->mainSplitter, element.attribute(QStringLiteral("main_splitter_sizes")));
  }
  if (element.hasAttribute(QStringLiteral("timeline_splitter_sizes"))) {
    apply_splitter(ui_->timelineSplitter, element.attribute(QStringLiteral("timeline_splitter_sizes")));
  }
}

std::optional<DatasetId> MainWindow::chooseActiveDataset(const std::vector<std::pair<DatasetId, QString>>& datasets) {
  if (datasets.size() == 1) {
    return datasets.front().first;
  }
  QStringList names;
  names.reserve(static_cast<int>(datasets.size()));
  for (const auto& [id, name] : datasets) {
    (void)id;
    names.push_back(name);
  }
  // Default to the most-recently-loaded dataset (datasets are load-ordered).
  const int default_index = static_cast<int>(datasets.size()) - 1;
  bool ok = false;
  const QString chosen = QInputDialog::getItem(
      this, tr("Apply Layout"), tr("Apply this layout to which dataset?"), names, default_index,
      /*editable=*/false, &ok);
  if (!ok) {
    return std::nullopt;
  }
  for (const auto& [id, name] : datasets) {
    if (name == chosen) {
      return id;
    }
  }
  return std::nullopt;
}

MainWindow::MissingCurveChoice MainWindow::promptMissingCurves(const QStringList& names) {
  static constexpr int kMaxShown = 10;
  const int name_count = static_cast<int>(names.size());
  QString body = tr("The layout references %n curve(s) not present in the current data:", "", name_count);
  body += QStringLiteral("\n\n");
  const int shown = std::min(name_count, kMaxShown);
  for (int i = 0; i < shown; ++i) {
    body += QStringLiteral("  • ") + names[i] + QStringLiteral("\n");
  }
  if (name_count > kMaxShown) {
    body += tr("  … and %n more\n", "", name_count - kMaxShown);
  }
  body += QStringLiteral("\n");
  body += tr("Choose how to handle them:");

  QMessageBox box(this);
  box.setIcon(QMessageBox::Question);
  box.setWindowTitle(tr("Missing curves"));
  box.setText(body);
  QPushButton* remove_btn = box.addButton(tr("Remove from plots"), QMessageBox::AcceptRole);
  QPushButton* cancel_btn = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
  // Default to Cancel — Remove is destructive (drops all missing curves
  // from every plot in the layout). Don't let an accidental Enter wipe
  // state on a layout the user just opened.
  box.setDefaultButton(cancel_btn);
  box.exec();

  if (box.clickedButton() == remove_btn) {
    return MissingCurveChoice::kRemove;
  }
  return MissingCurveChoice::kCancel;
}

QDomDocument MainWindow::xmlSaveState() const {
  QDomDocument doc;
  doc.appendChild(
      doc.createProcessingInstruction(QStringLiteral("xml"), QStringLiteral("version='1.0' encoding='UTF-8'")));

  QDomElement root = doc.createElement(QStringLiteral("root"));
  root.setAttribute(QStringLiteral("format"), QStringLiteral("PlotJuggler"));
  root.setAttribute(QStringLiteral("pj4_version"), QString::number(kLayoutSchemaVersion));
  doc.appendChild(root);

  root.appendChild(ui_->tabbedPlotWidget->xmlSaveState(doc));

  QDomElement link_x = doc.createElement(QStringLiteral("link_x"));
  link_x.setAttribute(
      QStringLiteral("enabled"), button_link_->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
  root.appendChild(link_x);

  const auto bool_attr = [](bool v) { return v ? QStringLiteral("true") : QStringLiteral("false"); };
  QDomElement show_points = doc.createElement(QStringLiteral("show_points"));
  show_points.setAttribute(QStringLiteral("enabled"), bool_attr(button_show_point_->isChecked()));
  root.appendChild(show_points);
  QDomElement legend_status = doc.createElement(QStringLiteral("legend_status"));
  legend_status.setAttribute(QStringLiteral("value"), QString::number(static_cast<int>(legend_status_)));
  root.appendChild(legend_status);
  QDomElement activate_grid = doc.createElement(QStringLiteral("activate_grid"));
  activate_grid.setAttribute(QStringLiteral("enabled"), bool_attr(button_grid_->isChecked()));
  root.appendChild(activate_grid);
  QDomElement dots = doc.createElement(QStringLiteral("dots"));
  dots.setAttribute(QStringLiteral("enabled"), bool_attr(button_dots_->isChecked()));
  root.appendChild(dots);
  QDomElement tracker_info = doc.createElement(QStringLiteral("tracker_info"));
  tracker_info.setAttribute(QStringLiteral("value"), QString::number(static_cast<int>(tracker_info_)));
  root.appendChild(tracker_info);
  QDomElement ratio = doc.createElement(QStringLiteral("ratio"));
  ratio.setAttribute(QStringLiteral("enabled"), bool_attr(button_ratio_->isChecked()));
  root.appendChild(ratio);
  return doc;
}

bool MainWindow::xmlLoadState(const QDomDocument& state_document) {
  const QDomElement root = state_document.documentElement();
  if (root.isNull() || root.tagName() != QStringLiteral("root")) {
    qCWarning(lcMain) << "No <root> element found at the top-level of the XML document";
    return false;
  }

  QDomElement main_tabbed_widget;
  for (auto tabbed = root.firstChildElement(QStringLiteral("tabbed_widget")); !tabbed.isNull();
       tabbed = tabbed.nextSiblingElement(QStringLiteral("tabbed_widget"))) {
    if (main_tabbed_widget.isNull()) {
      main_tabbed_widget = tabbed;
    }
    if (tabbed.attribute(QStringLiteral("parent")) == QStringLiteral("main_window")) {
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

  const QDomElement link_x = root.firstChildElement(QStringLiteral("link_x"));
  if (!link_x.isNull()) {
    button_link_->setChecked(
        link_x.attribute(QStringLiteral("enabled"), QStringLiteral("true")) == QStringLiteral("true") ||
        link_x.attribute(QStringLiteral("enabled")) == QStringLiteral("1"));
  }

  // Toggle states: read attributes, write QSettings, set button check
  // state. applying_state_ is true around this whole call, so the
  // toggled lambdas no-op — we apply once at the end via forEachPlot.
  const auto read_bool = [](const QDomElement& e, bool fallback) {
    if (e.isNull()) {
      return fallback;
    }
    const QString v = e.attribute(QStringLiteral("enabled"));
    return v == QStringLiteral("true") || v == QStringLiteral("1");
  };
  const QDomElement show_points = root.firstChildElement(QStringLiteral("show_points"));
  const QDomElement activate_grid = root.firstChildElement(QStringLiteral("activate_grid"));
  const QDomElement dots = root.firstChildElement(QStringLiteral("dots"));
  const QDomElement legend_status = root.firstChildElement(QStringLiteral("legend_status"));
  if (!show_points.isNull()) {
    show_points_ = read_bool(show_points, show_points_);
    button_show_point_->setChecked(show_points_);
    QSettings().setValue(QStringLiteral("MainWindow.buttonShowpoint"), show_points_);
  }
  if (!activate_grid.isNull()) {
    activate_grid_ = read_bool(activate_grid, activate_grid_);
    button_grid_->setChecked(activate_grid_);
    QSettings().setValue(QStringLiteral("MainWindow.buttonActivateGrid"), activate_grid_);
  }
  if (!dots.isNull()) {
    dots_ = read_bool(dots, dots_);
    button_dots_->setChecked(dots_);
    QSettings().setValue(QStringLiteral("MainWindow.buttonDots"), dots_);
  }
  const QDomElement tracker_info_el = root.firstChildElement(QStringLiteral("tracker_info"));
  if (!tracker_info_el.isNull()) {
    tracker_info_ = static_cast<CurveTracker::Parameter>(
        tracker_info_el.attribute(QStringLiteral("value"), QString::number(static_cast<int>(tracker_info_))).toInt());
    QSettings().setValue(QStringLiteral("MainWindow.timeTrackerSetting"), static_cast<int>(tracker_info_));
    updateTimeTrackerIcon();
  }
  const QDomElement ratio = root.firstChildElement(QStringLiteral("ratio"));
  if (!ratio.isNull()) {
    keep_ratio_ = read_bool(ratio, keep_ratio_);
    button_ratio_->setChecked(keep_ratio_);
    QSettings().setValue(QStringLiteral("MainWindow.buttonRatio"), keep_ratio_);
  }
  if (!legend_status.isNull()) {
    const auto new_status = static_cast<LegendStatus>(
        legend_status.attribute(QStringLiteral("value"), QString::number(static_cast<int>(legend_status_))).toInt());
    // Routes through setLegendStatus to refresh button checked / icon
    // state; the forEachPlot call inside is redundant with the
    // applyGlobalToggles loop below but harmless.
    setLegendStatus(new_status);
  }
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
  if (width_button_group_ != nullptr) {
    for (auto* btn : width_button_group_->buttons()) {
      btn->setEnabled(enable);
    }
  }
  if (style_button_group_ != nullptr) {
    for (auto* btn : style_button_group_->buttons()) {
      btn->setEnabled(enable);
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

  auto add_button = [this, outer](const char* object_name, const char* icon_path, const char* tooltip) -> QToolButton* {
    auto* btn = new QToolButton(ui_->globalToolbarWidget);
    btn->setObjectName(QString::fromLatin1(object_name));
    btn->setProperty("iconPath", QString::fromLatin1(icon_path));
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setAutoRaise(true);
    const int button_extent = chrome_metrics_.icon_size + chrome_metrics_.icon_padding;
    btn->setFixedSize(button_extent, button_extent);
    btn->setIconSize(QSize(chrome_metrics_.icon_size, chrome_metrics_.icon_size));
    btn->setIcon(LoadSvg(QString::fromLatin1(icon_path), theme_->currentTheme()));
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
  button_time_tracker_->setObjectName(QStringLiteral("buttonTimeTracker"));
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
  auto make_checkable = [](QToolButton* btn, bool initial_checked) {
    btn->setCheckable(true);
    btn->setChecked(initial_checked);
  };
  make_checkable(button_link_, QSettings().value(QStringLiteral("MainWindow.buttonLink"), true).toBool());
  make_checkable(button_show_point_, show_points_);
  make_checkable(button_grid_, activate_grid_);
  make_checkable(button_ratio_, keep_ratio_);
  make_checkable(button_dots_, dots_);
  make_checkable(button_reference_point_, false);
  // buttonZoomOut is a one-shot action, not a toggle — no make_checkable.
  connect(button_zoom_out_, &QToolButton::clicked, this, [this]() {
    linkedZoomOut();
    onUndoableChange();
  });
  connect(button_link_, &QToolButton::toggled, this, [](bool checked) {
    QSettings().setValue(QStringLiteral("MainWindow.buttonLink"), checked);
  });
  connect(button_show_point_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    show_points_ = checked;
    QSettings().setValue(QStringLiteral("MainWindow.buttonShowpoint"), checked);
    forEachPlot([checked](PlotWidget* plot) { plot->setShowPoints(checked); });
    applyShowPointsTo2DWidgets();
  });
  connect(button_grid_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    activate_grid_ = checked;
    QSettings().setValue(QStringLiteral("MainWindow.buttonActivateGrid"), checked);
    forEachPlot([checked](PlotWidget* plot) { plot->setGridVisible(checked); });
  });
  connect(button_dots_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    dots_ = checked;
    QSettings().setValue(QStringLiteral("MainWindow.buttonDots"), checked);
    forEachPlot([this](PlotWidget* plot) {
      applyDots(plot);
      plot->replot();
    });
  });
  connect(button_ratio_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    keep_ratio_ = checked;
    QSettings().setValue(QStringLiteral("MainWindow.buttonRatio"), checked);
    forEachPlot([checked](PlotWidget* plot) { plot->setKeepRatioXY(checked); });
  });
  // Session-only state — not persisted to QSettings, not in xmlSaveState.
  // Captures the playback time at the moment of click; subsequent scrubbing
  // does not move the reference.
  connect(button_reference_point_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    reference_time_ =
        checked ? std::optional<double>{toAxisDouble(session_->playbackEngine().currentTime())} : std::nullopt;
    forEachPlot([this](PlotWidget* plot) { plot->setReferenceLine(reference_time_); });
  });

  // Trailing stretch pins the icon stack at the top of the column.
  outer->addStretch(1);

  // Re-tint all global tool buttons when the theme rolls. Each button
  // tagged with an "iconPath" property is re-rendered against the new theme.
  connect(this, &MainWindow::stylesheetChanged, ui_->globalToolbarWidget, [this](const QString& theme) {
    for (auto* btn : ui_->globalToolbarWidget->findChildren<QToolButton*>()) {
      const QString path = btn->property("iconPath").toString();
      if (!path.isEmpty()) {
        btn->setIcon(LoadSvg(path, theme));
      }
    }
  });

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
      auto* btn = new QToolButton(strip);
      btn->setObjectName(QString::fromLatin1(spec.object_name));
      btn->setProperty("iconPath", QString::fromLatin1(spec.icon_path));
      btn->setFocusPolicy(Qt::NoFocus);
      btn->setAutoRaise(true);
      const int button_extent = chrome_metrics_.icon_size + chrome_metrics_.icon_padding;
      btn->setFixedSize(button_extent, button_extent);
      btn->setIconSize(QSize(chrome_metrics_.icon_size, chrome_metrics_.icon_size));
      btn->setIcon(LoadSvg(QString::fromLatin1(spec.icon_path), theme_->currentTheme()));
      btn->setToolTip(tr(spec.tooltip));
      connect(btn, &QToolButton::clicked, this, spec.on_click);
      flow->addWidget(btn);
    }
    outer->addWidget(strip);
    return header;
  };

  curve_width_header_ = build_section(
      tr("Curve Width"), QStringLiteral("widgetLabelCurveWidth"),
      {
          {"globalWidth1_0", ":/resources/svg/line_width_1_0.svg", "Line width 1.0", on_width(1.0)},
          {"globalWidth1_5", ":/resources/svg/line_width_1_5.svg", "Line width 1.5", on_width(1.5)},
          {"globalWidth2_0", ":/resources/svg/line_width_2_0.svg", "Line width 2.0", on_width(2.0)},
          {"globalWidth3_0", ":/resources/svg/line_width_3_0.svg", "Line width 3.0", on_width(3.0)},
      });

  // Curve Width: same exclusive radio-group pattern as Curve Style.
  // Default is 1.0 (kPoints1_0). The group's id is the LineWidth enum
  // index (0..3); the matching double is looked up from a parallel
  // array so the click slot below can call applyActivePlotWidth.
  width_button_group_ = new QButtonGroup(this);
  width_button_group_->setExclusive(true);
  const int initial_width_id = QSettings().value(QStringLiteral("MainWindow.curveWidth"), 0).toInt();
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
  connect(width_button_group_, &QButtonGroup::idClicked, this, [](int width_id) {
    QSettings().setValue(QStringLiteral("MainWindow.curveWidth"), width_id);
  });

  curve_style_header_ = build_section(
      tr("Curve Style"), QStringLiteral("widgetLabelCurveStyle"),
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
  // is checked at any time. Default is "Lines" (kLines is 0, the QSettings
  // fallback). QButtonGroup with exclusive=true uses Qt's button-group
  // semantics — clicking the checked button is a no-op, clicking another
  // checks it and unchecks the previous.
  style_button_group_ = new QButtonGroup(this);
  style_button_group_->setExclusive(true);
  const int initial_style =
      QSettings().value(QStringLiteral("MainWindow.curveStyle"), static_cast<int>(PlotWidgetBase::kLines)).toInt();
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
  // Persist the selection so the same style sticks across sessions, and
  // apply it once to existing plots so curves match the checked button.
  connect(style_button_group_, &QButtonGroup::idClicked, this, [](int style_value) {
    QSettings().setValue(QStringLiteral("MainWindow.curveStyle"), style_value);
  });

  // Re-tint all local-panel tool buttons when the theme rolls. Each
  // button tagged with an "iconPath" property is re-rendered against
  // the new theme.
  connect(this, &MainWindow::stylesheetChanged, ui_->localToolbarWidget, [this](const QString& theme) {
    for (auto* btn : ui_->localToolbarWidget->findChildren<QToolButton*>()) {
      const QString path = btn->property("iconPath").toString();
      if (!path.isEmpty()) {
        btn->setIcon(LoadSvg(path, theme));
      }
    }
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
  for (const auto& info : plot->curveList()) {
    if (info.curve != nullptr) {
      plot->setCurveLineWidth(info.source_name, width);
    }
  }
  plot->replot();
}

void MainWindow::applyActivePlotStyle(int style) {
  PlotWidget* plot = curve_editor_ != nullptr ? curve_editor_->plot() : nullptr;
  if (plot == nullptr) {
    return;
  }
  const auto curve_style = static_cast<PlotWidgetBase::CurveStyle>(style);
  for (const auto& info : plot->curveList()) {
    if (info.curve != nullptr) {
      plot->setCurveStyle(info.source_name, curve_style);
    }
  }
  plot->replot();
}

bool MainWindow::presentPanel(QWidget* panel) {
  if (panel == nullptr) {
    return false;
  }
  if (current_panel_ != nullptr) {
    qWarning("MainWindow::presentPanel: another panel is already presented");
    return false;
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
  const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/thirdparty/retro/");
  QString helper = QStandardPaths::findExecutable(QStringLiteral("pj-raster-helper"), {dir});
  if (helper.isEmpty()) {
    helper = dir + QStringLiteral("pj-raster-helper");
  }
  view->start(helper, dir + QStringLiteral("base.wad"));
}

void MainWindow::launchToolbox(const QString& plugin_id) {
  // Surface every failure on the diagnostic channel (the same sink the toolbox's
  // own on_message uses below), not just stderr, so a user-initiated launch that
  // fails is visible in the UI instead of silently doing nothing.
  auto report_error = [this](const QString& source, const QString& detail) {
    if (diagnostic_history_ != nullptr) {
      diagnostic_history_->record(DiagnosticLevel::kError, source, QStringLiteral("toolbox"), detail);
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
    std::shared_ptr<ToolboxHandle> handle;

    // Teardown order is load-bearing, so make it explicit here rather than relying
    // on member-declaration order alone: the plugin (handle) persists its state
    // through the settings backend in its destructor, so it must be torn down
    // first, then the service views (builder) into host/settings, then the host
    // (which holds a SettingsBackend&), then settings last. This survives a future
    // member reorder; the implicit reverse-declaration destruction that follows
    // only resets already-null pointers.
    ~PanelSession() {
      handle.reset();
      builder.reset();
      host.reset();
      settings.reset();
    }
  };
  auto session = std::make_shared<PanelSession>();
  session->settings = std::make_unique<QSettingsBackend>();
  session->builder = std::make_unique<ServiceRegistryBuilder>();

  const QString source = it->name.empty() ? plugin_id : QString::fromStdString(it->name);
  ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_data_changed = [this]() {
    session_->catalogModel().rebuildFromDatastore();
    session_->seedPlaybackFromSession();
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
    diagnostic_history_->record(diag, source, QStringLiteral("toolbox"), QString::fromStdString(message));
  };

  session->host = std::make_unique<ToolboxRuntimeHost>(
      session_->sessionManager().dataEngine(), session_->sessionManager().objectStore(), *session->settings,
      std::move(callbacks));
  session->host->registerServices(*session->builder);

  // 3. Create the toolbox instance and bind it to the assembled services.
  session->handle = std::make_shared<ToolboxHandle>(it->library.createHandle());
  if (auto status = session->handle->bind(session->builder->view()); !status) {
    report_error(source, tr("Failed to bind toolbox '%1': %2").arg(source, QString::fromStdString(status.error())));
    return;
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
  panel_config.catalog_key_resolver = [this](const std::string& key) -> std::string {
    auto descriptor = session_->catalogModel().curveDescriptor(QString::fromStdString(key));
    if (!descriptor) {
      return {};
    }
    return (descriptor->topic_name + "/" + descriptor->field_name).toStdString();
  };
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
  }
}

}  // namespace PJ
