#include "MainWindow.h"

#include <qwt_plot_curve.h>
#include <qwt_text.h>

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QByteArray>
#include <QCloseEvent>
#include <QColor>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QLoggingCategory>
#include <QMenu>
#include <QMouseEvent>
#include <QPalette>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSettings>
#include <QShortcut>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QWindow>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "DebugUi.h"
#include "FileLoader.h"
#include "LoadFileDialog.h"
#include "PreferencesDialog.h"
#include "Theme.h"
#include "TitleBar.h"
#include "pj_base/dataset.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_marketplace/marketplace_window.hpp"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_plotting/CurveEditor.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DiagnosticHistory.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_widgets/Media2DDockWidget.h"
#include "pj_scene2d_widgets/media_viewer_widget.h"
#include "pj_widgets/FlowLayout.h"
#include "pj_widgets/MessageBox.h"
#include "pj_widgets/SaveFileDialog.h"
#include "pj_widgets/SvgUtil.h"
#include "ui/CurveListPanel.h"
#include "ui/DiagnosticsDetailDialog.h"
#include "ui/LeftPanel.h"
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
constexpr double kNanosecondsPerSecond = 1e9;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr int kTestSampleCount = 1000;
constexpr double kTestDurationSeconds = 10.0;
constexpr int kResizeMargin = 6;
constexpr int kMaxRecentLayouts = 5;
constexpr auto kRecentLayoutsKey = "Layout/recent";
constexpr auto kLayoutFilter = "PlotJuggler 4 Layout (*.pjl4)";
constexpr auto kLayoutExtension = ".pjl4";
constexpr int kMaxUndoStates = 100;
constexpr qint64 kUndoCoalesceMs = 100;

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
  const char* icon_path;  // full resource path, no per-state suffix.
};

std::array<PanelToggle, 3> panelToggles(Ui::MainWindow* ui) {
  return {{
      {ui->tabbedPlotWidget->leftPanelButton(), ui->leftColumn, "MainWindow.panelLeftVisible",
       ":/resources/svg/panel_left.svg"},
      // Toggle target is timelineStrip, NOT the whole bottomPanel — the
      // playback strip (timelineWidget) sits above the strip in the same
      // panel and must remain visible at all times. Resize of the
      // bottomPanel via the splitter handle grows the strip; the playback
      // keeps its fixed height (sizePolicy Fixed-vertical in MainWindow.ui).
      {ui->tabbedPlotWidget->bottomPanelButton(), ui->timelineStrip, "MainWindow.panelBottomVisible",
       ":/resources/svg/panel_bottom.svg"},
      {ui->tabbedPlotWidget->rightPanelButton(), ui->localToolbarWidget, "MainWindow.panelRightVisible",
       ":/resources/svg/panel_right.svg"},
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
}  // namespace

MainWindow::MainWindow(QWidget* parent) : MainWindow(QString{}, parent) {}

MainWindow::MainWindow(QString extensions_dir, QWidget* parent)
    : QMainWindow(parent),
      ui_(new Ui::MainWindow),
      diagnostic_bridge_(new QtDiagnosticBridge(this)),
      session_(std::make_unique<AppSession>(std::move(extensions_dir), diagnostic_bridge_->sink())),
      theme_(std::make_unique<Theme>()) {
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
  // 6 × 24-px Curve Style icons = 144 px; round up to 150 so the FlowLayout
  // never wraps that strip into two rows.
  ui_->localToolbarWidget->setMinimumWidth(150);
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

  // The TitleBar owns its three popup menus; we just push actions into
  // them. The .ui no longer has a QMenuBar, so there's no reparenting
  // to do and no stale action-association to leak between popups.
  title_bar_ = new TitleBar(this);
  // Marketplace moved out of the App menu. The title-bar Extension
  // button now opens a popup of installed extensions with the
  // marketplace entry at the bottom — populated on aboutToShow so the
  // list reflects whatever the catalog currently has loaded.
  connect(title_bar_->extensionMenu(), &QMenu::aboutToShow, this, &MainWindow::onRebuildExtensionsMenu);
  connect(title_bar_, &TitleBar::diagnosticActivated, this, [this](const DiagnosticRecord& r) {
    auto* dlg = new DiagnosticsDetailDialog(r, this);
    dlg->show();
  });

  // Layout menu: Load... | Save... | Recent ▶ — Recent is rebuilt
  // lazily from QSettings every time it's about to show, so the list
  // stays in sync with whatever the most recent Load/Save did.
  action_load_layout_ = title_bar_->layoutMenu()->addAction(tr("Load..."), this, &MainWindow::onLoadLayout);
  action_save_layout_ = title_bar_->layoutMenu()->addAction(tr("Save..."), this, &MainWindow::onSaveLayout);
  title_bar_->layoutMenu()->addSeparator();
  recent_layouts_menu_ = title_bar_->layoutMenu()->addMenu(tr("Recent"));
  recent_layouts_menu_->setObjectName(QStringLiteral("PJMenu"));
  connect(recent_layouts_menu_, &QMenu::aboutToShow, this, &MainWindow::onRebuildRecentLayoutsMenu);
  // The chevron icon sits on the left of "Recent" (via the menuAction
  // icon slot, like Load/Save above) and the standard right-side
  // submenu indicator is hidden in QSS. The submenu itself is
  // repositioned to open on the LEFT of the parent menu via the
  // QEvent::Show branch in eventFilter().
  recent_layouts_menu_->installEventFilter(this);

  setMenuWidget(title_bar_);

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
      [this](
          ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title,
          QWidget* parent) -> IDataWidget* {
        auto* widget = new Media2DDockWidget(parent);
        widget->setSessionManager(&session_->sessionManager());
        if (widget->setImageTopic(topic_id, object_type, title)) {
          return widget;
        }
        // Tear down the empty widget and tell the user *why* the drop did
        // nothing. setImageTopic already logs the specific reason; this
        // surfaces the failure to the GUI so the operator doesn't sit
        // staring at an unchanged placeholder.
        widget->deleteLater();
        MessageBox::warning(
            this, tr("Cannot display topic"),
            tr("This object topic cannot be displayed in a 2D view (object_type=%1). "
               "Typically this means the source did not register a parser for the topic, "
               "or the type is not yet supported by the built-in viewer.")
                .arg(static_cast<int>(object_type)));
        return nullptr;
      });
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::tabAdded, this, &MainWindow::onPlotTabAdded);
  wireExistingPlots();
  ui_->curveListPanel->setCatalog(&session_->catalogModel());
  connect(ui_->curveListPanel, &CurveListPanel::trashRequested, this, &MainWindow::onCatalogTrashRequested);
  connect(ui_->curveListPanel, &CurveListPanel::clearAllCurvesRequested, this, [this]() {
    session_->catalogModel().clearAll();
  });

  QSettings settings;
  // Right-toolbar global view toggles persisted across sessions. Buttons
  // themselves are created later by buildGlobalToolbar(); we load state
  // first so the buttons can pick up the correct initial check state.
  show_points_ = settings.value(QStringLiteral("MainWindow.buttonShowpoint"), true).toBool();
  activate_grid_ = settings.value(QStringLiteral("MainWindow.buttonActivateGrid"), false).toBool();
  dots_ = settings.value(QStringLiteral("MainWindow.buttonDots"), false).toBool();
  legend_status_ = static_cast<LegendStatus>(
      settings.value(QStringLiteral("MainWindow.legendStatus"), static_cast<int>(LegendStatus::kHidden)).toInt());

  // Push the just-loaded toggle states into every plot already created by
  // wireExistingPlots(). New plots will pick this up via onPlotAdded.
  forEachPlot([this](PlotWidget* plot) { applyGlobalToggles(plot); });

  // Panel toggle buttons in the tab strip drive shell-level visibility
  // for the left column, the timeline strip, and the right toolbar.
  // The buttons are checkable: each carries a fixed "Dock to <side>"
  // glyph and the checked state mirrors the panel's visibility
  // (checked = visible). Persisted across sessions, defaulting to
  // visible. Visibility is restored before applyIcons() so the initial
  // checked state matches the live state.
  for (const PanelToggle& toggle : panelToggles(ui_)) {
    const bool visible = settings.value(QString::fromLatin1(toggle.settings_key), true).toBool();
    toggle.target->setVisible(visible);
    toggle.button->setChecked(visible);
    // Bottom-strip toggle restore: if the strip is hidden, clamp the
    // bottom panel to the playback bar's height so the splitter can't
    // open empty space below the playback when the user drags it.
    if (toggle.target == ui_->timelineStrip && !visible) {
      ui_->bottomPanel->setMaximumHeight(ui_->timelineWidget->minimumHeight());
    }
    const QByteArray key{toggle.settings_key};
    QPushButton* button = toggle.button;
    QWidget* target = toggle.target;
    connect(button, &QPushButton::clicked, this, [this, key, button, target]() {
      const bool now_visible = !target->isVisible();
      target->setVisible(now_visible);
      button->setChecked(now_visible);
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
  apply_theme_chrome();

  // Dev-only widget inspector: Ctrl+Shift+D = pesticide outline overlay,
  // Ctrl+Shift+Q = QSS debug border layer. Drop the include + this call +
  // DebugUi.{cpp,h} (plus the CMakeLists entries) to remove the feature.
  DebugUi::installInto(this, theme_.get());

  auto& playback = session_->playbackEngine();
  playback.setRange(0.0, 10.0);
  ui_->timelineWidget->setPlaybackEngine(&playback);
  connect(&playback, &PlaybackEngine::currentTimeChanged, this, [this](double time) {
    forEachDock([time](DockWidget* dock) { dock->onTrackerTime(time); });
  });

  refreshStreamingCombo();
  connect(
      &session_->extensionCatalog(), &ExtensionCatalogService::catalogChanged, this,
      &MainWindow::refreshStreamingCombo);

  file_loader_ = std::make_unique<FileLoader>(
      session_->sessionManager(), session_->extensionCatalog(), session_->catalogModel(), this);
  connect(ui_->leftPanel, &LeftPanel::loadDataRequested, this, &MainWindow::onLoadDataRequested);
  connect(file_loader_.get(), &FileLoader::fileLoaded, this, &MainWindow::onFileLoaded);
  // Track successful loads so the Recently loaded files popup can show
  // them; the cap-5 record/dedup logic is shared with the layout list.
  connect(file_loader_.get(), &FileLoader::fileLoaded, this, [](const QString& path) {
    QStringList recent = QSettings().value(QStringLiteral("File/recent")).toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > 5) {
      recent.removeLast();
    }
    QSettings().setValue(QStringLiteral("File/recent"), recent);
  });
  // Replay a recent path through the same loader. loadFile will fall
  // through fileLoaded / fileLoadFailed naturally; failures don't have
  // to be handled here.
  connect(ui_->leftPanel, &LeftPanel::recentFileSelected, this, [this](const QString& path) {
    file_loader_->loadFile(path, this);
  });

  connect(ui_->actionMarketplace, &QAction::triggered, this, &MainWindow::onOpenMarketplace);
  connect(ui_->actionExit, &QAction::triggered, this, &QWidget::close);

  // Preferences moved out of the dropdown to a dedicated cog button on
  // the title bar — see TitleBar::preferencesClicked.
  connect(title_bar_, &TitleBar::preferencesClicked, this, &MainWindow::onShowPreferencesDialog);

  // Consolidated PlotJuggler menu. Diagnostics is no longer a menu entry —
  // the title-bar bell popup is the single diagnostics view.
  //
  // Exit is wrapped as a QPushButton+QWidgetAction so the dynamic
  // `special` property can drive its gradient-hover QSS rule. The
  // button forwards its click to the original ui_->actionExit so all
  // shortcut/menu-association wiring keeps working.
  exit_menu_button_ = new QPushButton(ui_->actionExit->icon(), ui_->actionExit->text(), title_bar_->appMenu());
  exit_menu_button_->setFlat(true);
  exit_menu_button_->setProperty("special", true);
  // Carries the source SVG path so the eventFilter Enter/Leave swap
  // (in MainWindow::eventFilter) can re-render this icon dark while the
  // gradient hover background is showing.
  exit_menu_button_->setProperty("iconPath", QStringLiteral(":/resources/svg/logout.svg"));
  connect(exit_menu_button_, &QPushButton::clicked, ui_->actionExit, &QAction::trigger);
  auto* exit_widget_action = new QWidgetAction(title_bar_->appMenu());
  exit_widget_action->setDefaultWidget(exit_menu_button_);
  title_bar_->appMenu()->addAction(exit_widget_action);

  // Undo/Redo apply to plot-layout snapshots. Live in the Layout menu
  // above Recent so the shortcut + menu entry sit alongside the
  // Save/Load actions they undo.
  title_bar_->layoutMenu()->insertSeparator(recent_layouts_menu_->menuAction());
  undo_action_ = new QAction(tr("Undo"), this);
  undo_action_->setShortcuts(QKeySequence::Undo);
  connect(undo_action_, &QAction::triggered, this, &MainWindow::onUndo);
  title_bar_->layoutMenu()->insertAction(recent_layouts_menu_->menuAction(), undo_action_);
  redo_action_ = new QAction(tr("Redo"), this);
  redo_action_->setShortcuts(QKeySequence::Redo);
  connect(redo_action_, &QAction::triggered, this, &MainWindow::onRedo);
  title_bar_->layoutMenu()->insertAction(recent_layouts_menu_->menuAction(), redo_action_);

  // Plot-layout changes from TabbedPlotWidget feed the undo stack.
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::undoableChange, this, &MainWindow::onUndoableChange);

  // Global column on the right of the plot area — Chart + Legend icons,
  // pinned at 24 px wide, never collapses. Always visible regardless of
  // the local panel's toggle state.
  buildGlobalToolbar();

  // Local panel to the right of the global column — Curve Width / Curve
  // Style header bands and their FlowLayout icon strips, followed by the
  // per-curve CurveEditor. Hidden by the "Toggle Right Panel" button;
  // its width snaps/folds as the user drags the splitter handle.
  buildLocalToolbar();

  // CurveEditor lives below the icon strips inside the local panel; same
  // right-panel toggle controls visibility. Rebinds to the active plot
  // on tab changes.
  curve_editor_ = new CurveEditor(ui_->localToolbarWidget);
  auto* toolbar_layout = qobject_cast<QVBoxLayout*>(ui_->localToolbarWidget->layout());
  toolbar_layout->addWidget(curve_editor_, /*stretch=*/1);
  // Trailing stretch keeps the icon strips anchored at the top of the
  // panel when the CurveEditor below is hidden. CurveEditor's own
  // stretch factor (1) outweighs the spacer's default (0), so while
  // the editor is visible it still fills the remaining vertical space.
  toolbar_layout->addStretch(0);
  curve_editor_->onStylesheetChanged(theme_->currentTheme());
  connect(this, &MainWindow::stylesheetChanged, curve_editor_, &CurveEditor::onStylesheetChanged);
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::currentTabChanged, this, [this](PlotDocker* /*docker*/) {
    bindEditorToActivePlot();
  });
  bindEditorToActivePlot();

  pushInitialUndoState();
  updateUndoRedoActions();
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
  session_->playbackEngine().setRange(0.0, kTestDurationSeconds);
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

void MainWindow::onFileLoaded(const QString& /*path*/) {
  // Range computation + first-vs-subsequent-load semantics live in
  // AppSession::seedPlaybackFromSession(). MainWindow is the shell that
  // wires the load completion to the runtime — domain logic belongs in
  // pj_runtime, not here.
  session_->seedPlaybackFromSession();
}

void MainWindow::onCatalogTrashRequested(QStringList keys, bool covers_all) {
  if (covers_all) {
    session_->catalogModel().clearAll();
    return;
  }
  session_->catalogModel().removeItems(std::vector<QString>(keys.begin(), keys.end()));
}

void MainWindow::onShowPreferencesDialog() {
  PreferencesDialog dlg(*theme_, this);
  dlg.exec();
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

void MainWindow::applyIcons(QString theme) {
  // Right-side buttons (Chart + Legend in the global column, Width and
  // Line-style in the local panel) are created programmatically by
  // buildGlobalToolbar() / buildLocalToolbar() and re-tinted by their
  // own stylesheetChanged hooks via each button's "iconPath" property.
  for (const PanelToggle& toggle : panelToggles(ui_)) {
    toggle.button->setIcon(LoadSvg(QString::fromLatin1(toggle.icon_path), theme));
  }
  // Title-bar menus: their QActions persist across theme changes, so
  // re-tint here. The Marketplace entry under the Extensions menu is
  // rebuilt on each aboutToShow (see onRebuildExtensionsMenu) and gets
  // the current theme directly.
  ui_->actionExit->setIcon(QIcon(LoadSvg(":/resources/svg/logout.svg", theme)));
  if (exit_menu_button_ != nullptr) {
    exit_menu_button_->setIcon(ui_->actionExit->icon());
  }
  if (action_load_layout_ != nullptr) {
    action_load_layout_->setIcon(QIcon(LoadSvg(":/resources/svg/dashboard_load.svg", theme)));
  }
  if (action_save_layout_ != nullptr) {
    action_save_layout_->setIcon(QIcon(LoadSvg(":/resources/svg/save_as.svg", theme)));
  }
  if (recent_layouts_menu_ != nullptr) {
    recent_layouts_menu_->menuAction()->setIcon(QIcon(LoadSvg(":/resources/svg/play_arrow_left.svg", theme)));
  }
}

void MainWindow::onPlotTabAdded(PlotDocker* docker) {
  if (docker == nullptr) {
    return;
  }
  connect(docker, &PlotDocker::plotWidgetAdded, this, &MainWindow::onPlotAdded, Qt::UniqueConnection);
  for (int index = 0; index < docker->plotCount(); ++index) {
    if (DockWidget* dock = docker->plotAt(index)) {
      onPlotAdded(dock->plotWidget());
    }
  }
  bindEditorToActivePlot();
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
  plot->setTrackerPosition(session_->playbackEngine().currentTime());
  applyGlobalToggles(plot);
  bindEditorToActivePlot();
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

// Left-click cycle when the legend is already visible. Loops through
// the four corners; never returns kHidden (show/hide is driven by
// right-click).
//   TR → TL → BL → BR → TR
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
      // Defensive: callers should restore from the saved corner before
      // advancing the cycle; pick top-right as a safe fall-through.
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

void MainWindow::applyGlobalToggles(PlotWidget* plot) {
  if (plot == nullptr) {
    return;
  }
  plot->setShowPoints(show_points_);
  plot->setGridVisible(activate_grid_);
  plot->overrideCurvesStyle(
      dots_ ? std::optional<PlotWidgetBase::CurveStyle>{PlotWidgetBase::kLinesAndDots} : std::nullopt);
  applyLegendStatus(plot);
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
  session_->playbackEngine().setCurrentTime(point.x());
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

void MainWindow::closeEvent(QCloseEvent* event) {
  QSettings settings;
  settings.setValue(QStringLiteral("MainWindow.buttonLink"), button_link_->isChecked());
  QMainWindow::closeEvent(event);
}

void MainWindow::onLoadLayout() {
  const QString start_dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  LoadFileDialog dlg(this, start_dir, tr(kLayoutFilter), LoadFileDialog::Options::NoOptions);
  dlg.setDialogTitle(tr("Load Layout"));
  if (dlg.exec() != QDialog::Accepted) {
    return;
  }
  const QString path = dlg.selectedPath();
  if (path.isEmpty()) {
    return;
  }
  loadLayoutFromPath(path);
}

void MainWindow::onSaveLayout() {
  const QString start_dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  SaveFileDialog dlg(this, start_dir, tr(kLayoutFilter), QString::fromLatin1(kLayoutExtension));
  dlg.setDialogTitle(tr("Save Layout"));
  if (dlg.exec() != QDialog::Accepted) {
    return;
  }
  const QString path = dlg.selectedPath();
  if (path.isEmpty()) {
    return;
  }
  saveLayoutToPath(path);
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
  QMenu* menu = title_bar_->extensionMenu();
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

  menu->addSeparator();
  // Marketplace is a "special" item — gradient hover. Wrapped as a
  // QPushButton+QWidgetAction so QSS can target it via the
  // `special` property. Rebuilt fresh on every popup so the icon
  // matches the current theme without needing a member.
  auto* marketplace_btn = new QPushButton(
      QIcon(LoadSvg(":/resources/svg/archive.svg", theme_->currentTheme())), tr("PlotJuggler Marketplace"), menu);
  marketplace_btn->setFlat(true);
  marketplace_btn->setProperty("special", true);
  // Same iconPath protocol as exit_menu_button_ — enables the dark-on-
  // hover icon swap performed in MainWindow::eventFilter.
  marketplace_btn->setProperty("iconPath", QStringLiteral(":/resources/svg/archive.svg"));
  connect(marketplace_btn, &QPushButton::clicked, this, &MainWindow::onOpenMarketplace);
  auto* marketplace_action = new QWidgetAction(menu);
  marketplace_action->setDefaultWidget(marketplace_btn);
  menu->addAction(marketplace_action);
}

void MainWindow::loadLayoutFromPath(const QString& path) {
  // Stub: layout state isn't serialised yet (plot widgets are placeholder
  // in v1). We just verify the file is openable, refresh the recent list,
  // and report success. When real state lands, this is the function that
  // gains a body.
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    MessageBox::warning(this, tr("Load Layout"), tr("Cannot open '%1' for reading.").arg(path));
    return;
  }
  file.close();
  recordRecentLayout(path);
  emitDiagnostic(DiagnosticLevel::kInfo, "Layout", "loaded", tr("Loaded layout: %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::saveLayoutToPath(const QString& path) {
  // Stub: write a placeholder JSON document. Same future-shape note as
  // loadLayoutFromPath — when real state arrives, this gains a body.
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    MessageBox::warning(this, tr("Save Layout"), tr("Cannot open '%1' for writing.").arg(path));
    return;
  }
  file.write("{}\n");
  file.close();
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
  // Reposition the Recent-layouts submenu to open on the LEFT of its
  // parent (the Layout menu) instead of Qt's default rightward popup.
  // Qt computes its preferred position before firing QEvent::Show, so
  // we override here — the move() lands before the platform paints the
  // popup window, avoiding a one-frame flicker.
  if (type == QEvent::Show && watched == recent_layouts_menu_) {
    QMenu* parent_menu = title_bar_ != nullptr ? title_bar_->layoutMenu() : nullptr;
    if (parent_menu != nullptr && parent_menu->isVisible()) {
      const int sub_width = recent_layouts_menu_->sizeHint().width();
      const QPoint parent_top_left = parent_menu->mapToGlobal(QPoint(0, 0));
      recent_layouts_menu_->move(parent_top_left.x() - sub_width, recent_layouts_menu_->y());
    }
    return false;
  }
  // Special-hover icon swap: PJMenu items with the "special" property
  // (Exit, Marketplace, ...) paint a light blue→purple gradient on
  // hover. In dark mode the icon is rendered with white ink and would
  // vanish on that bright bg, so we swap to the light-theme-tinted
  // (dark-ink) variant while the cursor is over the button. The SVG
  // resource path is carried on the button as the "iconPath" dynamic
  // property — only buttons that opted in receive the swap.
  if ((type == QEvent::Enter || type == QEvent::Leave) && theme_ != nullptr &&
      theme_->currentTheme() != QLatin1String("light")) {
    auto* btn = qobject_cast<QPushButton*>(watched);
    if (btn != nullptr && btn->property("special").toBool()) {
      const QString svg_path = btn->property("iconPath").toString();
      if (!svg_path.isEmpty()) {
        const QString theme_for_icon = (type == QEvent::Enter) ? QStringLiteral("light") : theme_->currentTheme();
        btn->setIcon(QIcon(LoadSvg(svg_path, theme_for_icon)));
      }
    }
    // Don't return — let normal hover propagation continue.
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

void MainWindow::onUndo() {
  if (undo_states_.size() <= 1) {
    return;
  }

  redo_states_.push_back(undo_states_.back());
  undo_states_.pop_back();
  QDomDocument doc;
  doc.setContent(undo_states_.back());
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

QDomDocument MainWindow::xmlSaveState() const {
  QDomDocument doc;
  doc.appendChild(
      doc.createProcessingInstruction(QStringLiteral("xml"), QStringLiteral("version='1.0' encoding='UTF-8'")));

  QDomElement root = doc.createElement(QStringLiteral("root"));
  root.setAttribute(QStringLiteral("format"), QStringLiteral("PlotJuggler"));
  root.setAttribute(QStringLiteral("pj4_version"), QStringLiteral("1"));
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
  if (!legend_status.isNull()) {
    const auto new_status = static_cast<LegendStatus>(
        legend_status.attribute(QStringLiteral("value"), QString::number(static_cast<int>(legend_status_))).toInt());
    // Routes through setLegendStatus to refresh button checked / icon
    // state; the forEachPlot call inside is redundant with the
    // applyGlobalToggles loop below but harmless.
    setLegendStatus(new_status);
  }
  forEachPlot([this](PlotWidget* plot) { applyGlobalToggles(plot); });
  return true;
}

void MainWindow::pushInitialUndoState() {
  undo_states_.clear();
  redo_states_.clear();
  undo_states_.push_back(xmlSaveState().toByteArray(2));
  undo_timer_.start();
  updateUndoRedoActions();
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

void MainWindow::bindEditorToActivePlot() {
  if (curve_editor_ == nullptr) {
    return;
  }
  PlotWidget* active = nullptr;
  auto* docker = ui_->tabbedPlotWidget->currentTab();
  auto* dock = (docker != nullptr && docker->plotCount() > 0) ? docker->plotAt(0) : nullptr;
  if (dock != nullptr) {
    active = dock->plotWidget();
  }
  curve_editor_->setPlot(active);
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
    btn->setFixedSize(24, 24);
    btn->setIconSize(QSize(20, 20));
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
  button_show_point_ = add_button("buttonShowpoint", ":/resources/svg/show_point.svg", "Show point in plot");
  button_grid_ = add_button("buttonActivateGrid", ":/resources/svg/grid.svg", "Show/Hide the grid");
  button_dots_ = add_button("buttonDots", ":/resources/svg/scatter_plot.svg", "Show data point markers on curves");
  auto make_checkable = [](QToolButton* btn, bool initial_checked) {
    btn->setCheckable(true);
    btn->setChecked(initial_checked);
  };
  make_checkable(button_link_, QSettings().value(QStringLiteral("MainWindow.buttonLink"), true).toBool());
  make_checkable(button_show_point_, show_points_);
  make_checkable(button_grid_, activate_grid_);
  make_checkable(button_dots_, dots_);
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
    forEachPlot([checked](PlotWidget* plot) {
      plot->overrideCurvesStyle(
          checked ? std::optional<PlotWidgetBase::CurveStyle>{PlotWidgetBase::kLinesAndDots} : std::nullopt);
      plot->replot();
    });
  });

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
      "Legend position — left-click cycles corners (TR → TL → BL → BR), right-click shows / hides");
  button_legend_->setCheckable(true);
  button_legend_->setChecked(legend_status_ != LegendStatus::kHidden);
  connect(button_legend_, &QToolButton::clicked, this, [this](bool /*checked*/) {
    // Qt has already toggled the visual checked state by the time this
    // fires; setLegendStatus() resyncs it to the actual model state.
    if (legend_status_ == LegendStatus::kHidden) {
      // First left-click while hidden enables at the saved corner
      // without advancing the cycle.
      setLegendStatus(previous_legend_corner_);
    } else {
      setLegendStatus(nextLegendCorner(legend_status_));
    }
  });
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
}

void MainWindow::buildLocalToolbar() {
  // localToolbarWidget's QVBoxLayout from the .ui hosts the per-plot
  // settings: a "Curve Width" header + flow-strip, a "Curve Style"
  // header + flow-strip, then the CurveEditor (added by the caller).
  // Sections wrap as the panel narrows; below ~72 px the headers hide
  // and the icon strips stack into a 1- or 2-col snap.
  auto* outer = qobject_cast<QVBoxLayout*>(ui_->localToolbarWidget->layout());
  if (outer == nullptr) {
    return;
  }
  outer->setSpacing(0);
  outer->setContentsMargins(0, 0, 0, 0);

  struct ToolSpec {
    const char* object_name;
    const char* icon_path;
    const char* tooltip;
    std::function<void()> on_click;
  };
  const auto on_width = [this](double w) { return [this, w]() { applyGlobalWidth(w); }; };
  const auto on_style = [this](int s) { return [this, s]() { applyGlobalStyle(s); }; };

  auto build_section = [this, outer](
                           const QString& heading, const QString& header_object_name,
                           const std::vector<ToolSpec>& specs) -> QWidget* {
    // Heading: same 24-px grey band as Datasets / Custom Series / Curves
    // (styled via #widgetLabel* QSS rule that picks up titlebar_background).
    auto* header = new QWidget(ui_->localToolbarWidget);
    header->setObjectName(header_object_name);
    header->setFixedHeight(24);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(0);
    auto* label = new QLabel(heading, header);
    header_layout->addWidget(label);
    header_layout->addStretch(1);
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
    auto* strip = new QWidget(ui_->localToolbarWidget);
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
      btn->setFixedSize(24, 24);
      btn->setIconSize(QSize(20, 20));
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
  // array so the click slot below can call applyGlobalWidth.
  width_button_group_ = new QButtonGroup(this);
  width_button_group_->setExclusive(true);
  const std::array<std::pair<const char*, double>, 4> width_button_specs{{
      {"globalWidth1_0", 1.0},
      {"globalWidth1_5", 1.5},
      {"globalWidth2_0", 2.0},
      {"globalWidth3_0", 3.0},
  }};
  const int initial_width_id = QSettings().value(QStringLiteral("MainWindow.curveWidth"), 0).toInt();
  for (int i = 0; i < static_cast<int>(width_button_specs.size()); ++i) {
    auto* btn =
        curve_width_header_->parentWidget()->findChild<QToolButton*>(QString::fromLatin1(width_button_specs[i].first));
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
  if (initial_width_id >= 0 && initial_width_id < static_cast<int>(width_button_specs.size())) {
    applyGlobalWidth(width_button_specs[initial_width_id].second);
  }

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
  connect(style_button_group_, &QButtonGroup::idClicked, this, [this](int style_value) {
    QSettings().setValue(QStringLiteral("MainWindow.curveStyle"), style_value);
  });
  applyGlobalStyle(initial_style);

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
}

void MainWindow::applyGlobalWidth(double width) {
  forEachPlot([width](PlotWidget* plot) {
    for (const auto& info : plot->curveList()) {
      if (info.curve != nullptr) {
        plot->setCurveLineWidth(info.source_name, width);
      }
    }
    plot->replot();
  });
}

void MainWindow::applyGlobalStyle(int style) {
  const auto curve_style = static_cast<PlotWidgetBase::CurveStyle>(style);
  forEachPlot([curve_style](PlotWidget* plot) {
    for (const auto& info : plot->curveList()) {
      if (info.curve != nullptr) {
        plot->setCurveStyle(info.source_name, curve_style);
      }
    }
    plot->replot();
  });
}

}  // namespace PJ
