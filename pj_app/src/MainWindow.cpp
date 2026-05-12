#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QKeySequence>
#include <QLoggingCategory>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "FileLoader.h"
#include "PreferencesDialog.h"
#include "Theme.h"
#include "pj_base/dataset.hpp"
#include "pj_datastore/engine.hpp"
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
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/SvgUtil.h"
#include "ui/CurveListPanel.h"
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
constexpr double kNanosecondsPerSecond = 1e9;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr int kTestSampleCount = 1000;
constexpr double kTestDurationSeconds = 10.0;
constexpr int kMaxDiagnostics = 200;
constexpr int kMaxUndoStates = 100;
constexpr qint64 kUndoCoalesceMs = 100;
constexpr auto kButtonLinkKey = "MainWindow.buttonLink";

// QSettings keys for the CurveEditor side panel. Position is stored as int
// (cast of PanelPosition); per-position splitter state is stored under
// separate keys so reopening the editor at the same position restores its
// exact width / height.
constexpr auto kPanelPositionKey = "MainWindow.panelPosition";
constexpr auto kPanelStateKeyLeft = "MainWindow.splitterState.left";
constexpr auto kPanelStateKeyRight = "MainWindow.splitterState.right";
constexpr auto kPanelStateKeyBottom = "MainWindow.splitterState.bottom";

[[nodiscard]] const char* panelStateKeyFor(PanelPosition pos) {
  switch (pos) {
    case PanelPosition::kLeft:
      return kPanelStateKeyLeft;
    case PanelPosition::kRight:
      return kPanelStateKeyRight;
    case PanelPosition::kBottom:
      return kPanelStateKeyBottom;
    case PanelPosition::kNone:
      return "";
  }
  return "";
}

QUrl registryUrlFromSettings() {
  QSettings settings;
  const QString raw = settings.value(kRegistryUrlSettingsKey, kDefaultRegistryUrl).toString();
  const QUrl url(raw);
  if (!url.isValid() || url.scheme().isEmpty()) {
    qCWarning(lcMain) << "Invalid" << kRegistryUrlSettingsKey << "in QSettings:" << raw << "— falling back to default.";
    return QUrl(kDefaultRegistryUrl);
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
  connect(diagnostic_bridge_, &QtDiagnosticBridge::diagnosticReported, this, &MainWindow::onDiagnosticReported);

  ui_->tabbedPlotWidget->setDataServices(&session_->sessionManager(), &session_->catalogModel());
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::tabAdded, this, &MainWindow::onPlotTabAdded);
  wireExistingPlots();
  ui_->curveListPanel->setCatalog(&session_->catalogModel());

  QSettings settings;
  applyIcons(theme_->currentTheme());
  ui_->buttonLink->setChecked(settings.value(kButtonLinkKey, true).toBool());
  connect(ui_->buttonLink, &QPushButton::toggled, this, [](bool checked) {
    QSettings settings;
    settings.setValue(kButtonLinkKey, checked);
  });

  connect(theme_.get(), &Theme::themeChanged, this, &MainWindow::onThemeChanged);
  connect(this, &MainWindow::stylesheetChanged, ui_->leftPanel, &LeftPanel::onStylesheetChanged);
  connect(this, &MainWindow::stylesheetChanged, ui_->curveListPanel, &CurveListPanel::onStylesheetChanged);
  connect(this, &MainWindow::stylesheetChanged, ui_->timelineWidget, &TimelineWidget::onStylesheetChanged);
  connect(this, &MainWindow::stylesheetChanged, ui_->tabbedPlotWidget, &TabbedPlotWidget::onStylesheetChanged);
  qApp->setStyleSheet(theme_->expandedQss());

  connect(ui_->buttonPanelLeft, &QToolButton::toggled, this, [this](bool checked) {
    onPanelButtonToggled(PanelPosition::kLeft, checked);
  });
  connect(ui_->buttonPanelBottom, &QToolButton::toggled, this, [this](bool checked) {
    onPanelButtonToggled(PanelPosition::kBottom, checked);
  });
  connect(ui_->buttonPanelRight, &QToolButton::toggled, this, [this](bool checked) {
    onPanelButtonToggled(PanelPosition::kRight, checked);
  });

  curve_editor_ = new CurveEditor(this);
  curve_editor_->setVisible(false);

  diagnostics_action_ = ui_->menuHelp->addAction(tr("Diagnostics..."), this, &MainWindow::onShowDiagnosticsDialog);
  diagnostics_action_->setEnabled(false);
  diagnostics_button_ = new QPushButton(tr("Diagnostics"), this);
  diagnostics_button_->setFlat(true);
  diagnostics_button_->setToolTip(tr("Open the recent diagnostics log"));
  diagnostics_button_->setVisible(false);
  connect(diagnostics_button_, &QPushButton::clicked, this, &MainWindow::onShowDiagnosticsDialog);
  statusBar()->addPermanentWidget(diagnostics_button_);

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

  auto* edit_menu = new QMenu(tr("Edit"), this);
  menuBar()->insertMenu(ui_->menuTools->menuAction(), edit_menu);
  undo_action_ = edit_menu->addAction(tr("Undo"), this, &MainWindow::onUndo);
  undo_action_->setShortcuts(QKeySequence::Undo);
  redo_action_ = edit_menu->addAction(tr("Redo"), this, &MainWindow::onRedo);
  redo_action_->setShortcuts(QKeySequence::Redo);

  load_layout_action_ = new QAction(tr("Load Layout..."), this);
  save_layout_action_ = new QAction(tr("Save Layout..."), this);
  ui_->menuApp->insertAction(ui_->actionMarketplace, load_layout_action_);
  ui_->menuApp->insertAction(ui_->actionMarketplace, save_layout_action_);
  ui_->menuApp->insertSeparator(ui_->actionMarketplace);
  connect(load_layout_action_, &QAction::triggered, this, &MainWindow::onLoadLayout);
  connect(save_layout_action_, &QAction::triggered, this, &MainWindow::onSaveLayout);

  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::undoableChange, this, &MainWindow::onUndoableChange);
  connect(ui_->actionMarketplace, &QAction::triggered, this, &MainWindow::onOpenMarketplace);
  connect(ui_->actionExit, &QAction::triggered, this, &QWidget::close);

  // App menu: [Marketplace] | [Preferences] | [Exit] (two separators).
  // The .ui already provides the divider above Exit; we insert
  // Preferences between, then add a second separator below it.
  auto* preferences_action = new QAction(tr("Preferences..."), this);
  preferences_action->setShortcut(QKeySequence::Preferences);
  connect(preferences_action, &QAction::triggered, this, &MainWindow::onShowPreferencesDialog);
  ui_->menuApp->insertAction(ui_->actionExit, preferences_action);
  ui_->menuApp->insertSeparator(ui_->actionExit);

  pushInitialUndoState();

  bindEditorToActivePlot();
  const auto stored_position =
      static_cast<PanelPosition>(settings.value(kPanelPositionKey, static_cast<int>(PanelPosition::kNone)).toInt());
  switch (stored_position) {
    case PanelPosition::kLeft:
      ui_->buttonPanelLeft->setChecked(true);
      break;
    case PanelPosition::kRight:
      ui_->buttonPanelRight->setChecked(true);
      break;
    case PanelPosition::kBottom:
      ui_->buttonPanelBottom->setChecked(true);
      break;
    case PanelPosition::kNone:
      break;
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

  auto writer = engine.createWriter();
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

  const auto changed_topics = session_->sessionManager().commitChunks(writer.flushAll());
  if (changed_topics.empty()) {
    qCWarning(lcMain) << "test data commit produced no datastore changes";
    return false;
  }
  session_->catalogModel().rebuildFromDatastore();
  session_->playbackEngine().setRange(0.0, kTestDurationSeconds);
  statusBar()->showMessage(tr("Loaded test sin/cos data"), 3000);
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

void MainWindow::onShowPreferencesDialog() {
  PreferencesDialog dlg(*theme_, this);
  dlg.exec();
}

void MainWindow::onThemeChanged(const QString& theme) {
  qApp->setStyleSheet(theme_->expandedQss());
  applyIcons(theme);
  emit stylesheetChanged(theme);
  forEachPlot([](PlotWidget* plot) { plot->replot(); });
}

void MainWindow::applyIcons(QString theme) {
  ui_->buttonLink->setIcon(LoadSvg(":/resources/svg/link.svg", theme));
  ui_->buttonPanelLeft->setIcon(LoadSvg(":/resources/svg/panel_left.svg", theme));
  ui_->buttonPanelBottom->setIcon(LoadSvg(":/resources/svg/panel_bottom.svg", theme));
  ui_->buttonPanelRight->setIcon(LoadSvg(":/resources/svg/panel_right.svg", theme));
}

void MainWindow::onShowDiagnosticsDialog() {
  QDialog dlg(this);
  dlg.setWindowTitle(tr("Diagnostics"));
  dlg.resize(760, 420);

  auto* layout = new QVBoxLayout(&dlg);
  auto* text = new QPlainTextEdit(&dlg);
  text->setReadOnly(true);

  QStringList lines;
  for (const UiDiagnostic& diagnostic : diagnostics_) {
    const char* level = diagnostic.level == DiagnosticLevel::kError     ? "ERROR"
                        : diagnostic.level == DiagnosticLevel::kWarning ? "WARN"
                                                                        : "INFO";
    const QString source = diagnostic.source.isEmpty() ? QStringLiteral("-") : diagnostic.source;
    const QString id = diagnostic.id.isEmpty() ? QStringLiteral("-") : diagnostic.id;
    lines.append(QString("[%1] %2 %3 %4: %5")
                     .arg(
                         diagnostic.timestamp.toLocalTime().toString(Qt::ISODate), QString::fromLatin1(level), source,
                         id, diagnostic.message));
  }
  text->setPlainText(lines.isEmpty() ? tr("No diagnostics.") : lines.join('\n'));
  layout->addWidget(text);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  layout->addWidget(buttons);
  dlg.exec();
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

void MainWindow::onSaveLayout() {
  const QString file_name =
      QFileDialog::getSaveFileName(this, tr("Save Layout"), QString{}, tr("PlotJuggler layout (*.xml);;All files (*)"));
  if (file_name.isEmpty()) {
    return;
  }

  QFile file(file_name);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
    QMessageBox::warning(this, tr("Save Layout"), tr("Unable to write %1").arg(file_name));
    return;
  }

  const QDomDocument state = xmlSaveState();
  file.write(state.toByteArray(2));
  statusBar()->showMessage(tr("Saved layout %1").arg(file_name), 3000);
}

void MainWindow::onLoadLayout() {
  const QString file_name =
      QFileDialog::getOpenFileName(this, tr("Load Layout"), QString{}, tr("PlotJuggler layout (*.xml);;All files (*)"));
  if (file_name.isEmpty()) {
    return;
  }

  QFile file(file_name);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QMessageBox::warning(this, tr("Load Layout"), tr("Unable to read %1").arg(file_name));
    return;
  }

  QDomDocument state;
  const auto parse_result = state.setContent(&file);
  if (!parse_result) {
    QMessageBox::warning(
        this, tr("Load Layout"),
        tr("Invalid XML at line %1, column %2: %3")
            .arg(parse_result.errorLine)
            .arg(parse_result.errorColumn)
            .arg(parse_result.errorMessage));
    return;
  }

  const bool loaded = [&] {
    QScopedValueRollback guard(applying_state_, true);
    return xmlLoadState(state);
  }();
  if (!loaded) {
    QMessageBox::warning(this, tr("Load Layout"), tr("The file does not contain a supported PlotJuggler layout."));
    return;
  }

  pushInitialUndoState();
  statusBar()->showMessage(tr("Loaded layout %1").arg(file_name), 3000);
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
  disconnect(plot, &PlotWidget::statusMessageRequested, this, nullptr);
  connect(plot, &PlotWidget::statusMessageRequested, this, [this](const QString& message) {
    statusBar()->showMessage(message, 3000);
  });
  plot->setTrackerPosition(session_->playbackEngine().currentTime());
  bindEditorToActivePlot();
}

void MainWindow::onPlotZoomChanged(PlotWidget* modified, QRectF rect) {
  if (applying_state_) {
    return;
  }
  if (!ui_->buttonLink->isChecked()) {
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

void MainWindow::onDiagnosticReported(int level, QString source, QString id, QString message) {
  const auto diagnostic_level = static_cast<DiagnosticLevel>(level);
  diagnostics_.append(
      UiDiagnostic{
          diagnostic_level,
          std::move(source),
          std::move(id),
          std::move(message),
          QDateTime::currentDateTimeUtc(),
      });
  while (diagnostics_.size() > kMaxDiagnostics) {
    diagnostics_.removeFirst();
  }
  updateDiagnosticsButton();

  if (diagnostic_level != DiagnosticLevel::kWarning && diagnostic_level != DiagnosticLevel::kError) {
    return;
  }

  const QString prefix = diagnostic_level == DiagnosticLevel::kError ? tr("Error") : tr("Warning");
  const UiDiagnostic& diagnostic = diagnostics_.back();
  const QString display =
      diagnostic.source.isEmpty() ? diagnostic.message : diagnostic.source + QStringLiteral(": ") + diagnostic.message;
  statusBar()->showMessage(prefix + QStringLiteral(": ") + display, 8000);
}

void MainWindow::updateDiagnosticsButton() {
  const bool has_diagnostics = !diagnostics_.isEmpty();
  if (diagnostics_action_ != nullptr) {
    diagnostics_action_->setEnabled(has_diagnostics);
  }
  if (diagnostics_button_ == nullptr) {
    return;
  }
  diagnostics_button_->setVisible(has_diagnostics);
  diagnostics_button_->setText(
      diagnostics_.size() == 1 ? tr("Diagnostics") : tr("Diagnostics (%1)").arg(diagnostics_.size()));
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
  QTabWidget* tabs = ui_->tabbedPlotWidget->tabWidget();
  for (int index = 0; index < tabs->count(); ++index) {
    auto* docker = qobject_cast<PlotDocker*>(tabs->widget(index));
    if (docker != nullptr) {
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
      QStringLiteral("enabled"), ui_->buttonLink->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
  root.appendChild(link_x);

  QDomElement relative_time = doc.createElement(QStringLiteral("use_relative_time_offset"));
  relative_time.setAttribute(QStringLiteral("enabled"), QStringLiteral("false"));
  root.appendChild(relative_time);

  QDomElement streaming_buffer = doc.createElement(QStringLiteral("streaming_buffer_size"));
  streaming_buffer.setAttribute(QStringLiteral("value"), QStringLiteral("5"));
  root.appendChild(streaming_buffer);
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
    ui_->buttonLink->setChecked(
        link_x.attribute(QStringLiteral("enabled"), QStringLiteral("true")) == QStringLiteral("true") ||
        link_x.attribute(QStringLiteral("enabled")) == QStringLiteral("1"));
  }
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

void MainWindow::closeEvent(QCloseEvent* event) {
  QSettings settings;
  if (panel_position_ != PanelPosition::kNone) {
    savePanelSize();
  }
  settings.setValue(kPanelPositionKey, static_cast<int>(panel_position_));
  QMainWindow::closeEvent(event);
}

void MainWindow::onPanelButtonToggled(PanelPosition pos, bool checked) {
  if (curve_editor_ == nullptr) {
    return;
  }

  if (checked) {
    // Untoggle the other two position buttons with their signals blocked so
    // they don't re-enter this slot. Mutual exclusion is implemented manually
    // because QButtonGroup::setExclusive(true) would prevent the click-active-
    // again-to-hide path below.
    const std::pair<PanelPosition, QToolButton*> all_buttons[] = {
        {PanelPosition::kLeft, ui_->buttonPanelLeft},
        {PanelPosition::kRight, ui_->buttonPanelRight},
        {PanelPosition::kBottom, ui_->buttonPanelBottom},
    };
    for (const auto& [other_pos, btn] : all_buttons) {
      if (other_pos != pos && btn->isChecked()) {
        QSignalBlocker block(btn);
        btn->setChecked(false);
      }
    }
    showCurveEditor(pos);
  } else if (pos == panel_position_) {
    hideCurveEditor();
  }
  // (!checked && pos != panel_position_) is the de-toggle that fired when we
  // ourselves untoggled an inactive sibling above; ignore it.
}

void MainWindow::showCurveEditor(PanelPosition pos) {
  if (curve_editor_ == nullptr) {
    return;
  }
  if (panel_position_ != PanelPosition::kNone) {
    savePanelSize();
  }
  // setParent(this) detaches from any current splitter slot without destroying.
  curve_editor_->setParent(this);

  QSplitter* splitter = ui_->plotAreaSplitter;
  splitter->setOrientation(pos == PanelPosition::kBottom ? Qt::Vertical : Qt::Horizontal);

  // kLeft inserts before the plots; kRight and kBottom both append after.
  const int insert_index = pos == PanelPosition::kLeft ? 0 : splitter->count();
  splitter->insertWidget(insert_index, curve_editor_);

  curve_editor_->setVisible(true);
  panel_position_ = pos;

  QSettings settings;
  const QByteArray state = settings.value(panelStateKeyFor(pos)).toByteArray();
  if (!state.isEmpty()) {
    splitter->restoreState(state);
  }
}

void MainWindow::hideCurveEditor() {
  if (curve_editor_ == nullptr) {
    return;
  }
  if (panel_position_ != PanelPosition::kNone) {
    savePanelSize();
  }
  curve_editor_->setVisible(false);
  curve_editor_->setParent(this);
  panel_position_ = PanelPosition::kNone;
  ui_->plotAreaSplitter->setOrientation(Qt::Horizontal);
}

void MainWindow::savePanelSize() {
  if (panel_position_ == PanelPosition::kNone) {
    return;
  }
  QSettings settings;
  settings.setValue(panelStateKeyFor(panel_position_), ui_->plotAreaSplitter->saveState());
}

void MainWindow::bindEditorToActivePlot() {
  if (curve_editor_ == nullptr) {
    return;
  }
  PlotWidget* active = nullptr;
  auto* tabs = ui_->tabbedPlotWidget->tabWidget();
  auto* docker = tabs ? qobject_cast<PlotDocker*>(tabs->currentWidget()) : nullptr;
  auto* dock = (docker && docker->plotCount() > 0) ? docker->plotAt(0) : nullptr;
  if (dock) {
    active = dock->plotWidget();
  }
  curve_editor_->setPlot(active);
}

}  // namespace PJ
