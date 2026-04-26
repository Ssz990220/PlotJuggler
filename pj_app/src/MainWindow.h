#pragma once

#include <QMainWindow>
#include <QPointF>
#include <QRectF>
#include <functional>
#include <memory>

class QCloseEvent;

namespace Ui {
class MainWindow;
}

namespace PJ {

class AppSession;
class DockWidget;
class FileLoader;
class PlotDocker;
class PlotWidget;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;
  [[nodiscard]] bool populateTestData();

 private slots:
  void onOpenMarketplace();
  void onLoadDataRequested();
  void onPlotTabAdded(PlotDocker* docker);
  void onPlotAdded(PlotWidget* plot);
  void onPlotZoomChanged(PlotWidget* modified, QRectF rect);
  void onTrackerMovedFromWidget(QPointF point);

 private:
  void refreshStreamingCombo();
  void wireExistingPlots();
  void forEachDocker(const std::function<void(PlotDocker*)>& operation);
  void forEachDock(const std::function<void(DockWidget*)>& operation);
  void forEachPlot(const std::function<void(PlotWidget*)>& operation);

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  Ui::MainWindow* ui_;
  std::unique_ptr<AppSession> session_;
  std::unique_ptr<FileLoader> file_loader_;
};

}  // namespace PJ
