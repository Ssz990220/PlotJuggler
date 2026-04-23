#include "MainWindow.h"

#include "pj_app_core/AppSession.h"
#include "pj_app_core/PlaybackEngine.h"
#include "ui/CurveListPanel.h"
#include "ui/LeftPanel.h"
#include "ui/TimelineWidget.h"
#include "ui_MainWindow.h"

namespace PJ {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui_(new Ui::MainWindow), session_(std::make_unique<AppSession>()) {
  ui_->setupUi(this);

  ui_->curveListPanel->setCatalog(session_->catalogModel());

  auto* playback = session_->playbackEngine();
  playback->setRange(0.0, 10.0);
  ui_->timelineWidget->setPlaybackEngine(playback);
}

MainWindow::~MainWindow() {
  delete ui_;
}

}  // namespace PJ
