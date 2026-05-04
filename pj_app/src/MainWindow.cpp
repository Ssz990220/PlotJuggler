#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLoggingCategory>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "FileLoader.h"
#include "PreferencesDialog.h"
#include "Theme.h"
#include "pj_app_core/AppSession.h"
#include "pj_app_core/CatalogModel.h"
#include "pj_app_core/ExtensionCatalogService.h"
#include "pj_app_core/PlaybackEngine.h"
#include "pj_app_core/SessionManager.h"
#include "pj_app_core/SvgUtil.h"
#include "pj_base/dataset.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_marketplace/marketplace_window.hpp"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_plot_widgets/DockWidget.h"
#include "pj_plot_widgets/PlotDocker.h"
#include "pj_plot_widgets/PlotWidget.h"
#include "pj_plot_widgets/TabbedPlotWidget.h"
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
  connect(diagnostic_bridge_, &QtDiagnosticBridge::diagnosticReported, this, &MainWindow::onDiagnosticReported);

  ui_->tabbedPlotWidget->setDataServices(&session_->sessionManager(), &session_->catalogModel());
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::tabAdded, this, &MainWindow::onPlotTabAdded);
  wireExistingPlots();
  ui_->curveListPanel->setCatalog(&session_->catalogModel());

  QSettings settings;
  applyIcons(theme_->currentTheme());
  ui_->buttonLink->setChecked(settings.value(QStringLiteral("MainWindow.buttonLink"), true).toBool());
  connect(ui_->buttonLink, &QPushButton::toggled, this, [](bool checked) {
    QSettings().setValue(QStringLiteral("MainWindow.buttonLink"), checked);
  });

  connect(theme_.get(), &Theme::themeChanged, this, &MainWindow::onThemeChanged);
  connect(this, &MainWindow::stylesheetChanged, ui_->leftPanel, &LeftPanel::onStylesheetChanged);
  connect(this, &MainWindow::stylesheetChanged, ui_->curveListPanel, &CurveListPanel::onStylesheetChanged);
  connect(this, &MainWindow::stylesheetChanged, ui_->timelineWidget, &TimelineWidget::onStylesheetChanged);
  connect(this, &MainWindow::stylesheetChanged, ui_->tabbedPlotWidget, &TabbedPlotWidget::onStylesheetChanged);
  qApp->setStyleSheet(theme_->expandedQss());

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
}

void MainWindow::onPlotAdded(PlotWidget* plot) {
  if (plot == nullptr) {
    return;
  }
  connect(plot, &PlotWidget::rectChanged, this, &MainWindow::onPlotZoomChanged, Qt::UniqueConnection);
  connect(plot, &PlotWidget::trackerMoved, this, &MainWindow::onTrackerMovedFromWidget, Qt::UniqueConnection);
  connect(plot, &PlotWidget::statusMessageRequested, this, [this](const QString& message) {
    statusBar()->showMessage(message, 3000);
  });
  plot->setTrackerPosition(session_->playbackEngine().currentTime());
}

void MainWindow::onPlotZoomChanged(PlotWidget* modified, QRectF rect) {
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

void MainWindow::closeEvent(QCloseEvent* event) {
  QSettings().setValue(QStringLiteral("MainWindow.buttonLink"), ui_->buttonLink->isChecked());
  QMainWindow::closeEvent(event);
}

}  // namespace PJ
