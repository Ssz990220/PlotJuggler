#include "MainWindow.h"

#include <QCloseEvent>
#include <QLoggingCategory>
#include <QSettings>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QToolButton>
#include <QUrl>
#include <cmath>
#include <cstdint>
#include <vector>

#include "FileLoader.h"
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

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui_(new Ui::MainWindow), session_(std::make_unique<AppSession>()) {
  ui_->setupUi(this);

  ui_->tabbedPlotWidget->setDataServices(&session_->sessionManager(), &session_->catalogModel());
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::tabAdded, this, &MainWindow::onPlotTabAdded);
  wireExistingPlots();
  ui_->curveListPanel->setCatalog(&session_->catalogModel());

  QSettings settings;
  ui_->buttonLink->setIcon(LoadSvg(":/resources/svg/link.svg", currentTheme()));
  ui_->buttonLink->setChecked(settings.value(QStringLiteral("MainWindow.buttonLink"), true).toBool());
  connect(ui_->buttonLink, &QToolButton::toggled, this, [](bool checked) {
    QSettings().setValue(QStringLiteral("MainWindow.buttonLink"), checked);
  });

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

void MainWindow::refreshStreamingCombo() {
  QStringList names;
  for (const auto* ds : session_->extensionCatalog().streamSources()) {
    names << ds->name;
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
