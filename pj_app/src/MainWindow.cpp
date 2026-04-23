#include "MainWindow.h"

#include <DockManager.h>

namespace PJ {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), session_(std::make_unique<AppSession>()) {
  setWindowTitle(QStringLiteral("PlotJuggler 4"));
  resize(1280, 800);

  dock_manager_ = new ads::CDockManager(this);
  setCentralWidget(dock_manager_);
}

MainWindow::~MainWindow() = default;

}  // namespace PJ
