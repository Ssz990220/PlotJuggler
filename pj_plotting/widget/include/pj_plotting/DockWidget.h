#pragma once

#include <DockWidget.h>

#include "pj_runtime/IDataWidget.h"

namespace PJ {

class CatalogModel;
class DockToolbar;
class PlotWidget;
class SessionManager;

// ADS-backed dock hosting a single plot area. splitHorizontal /
// splitVertical create sibling DockWidgets inside the parent PlotDocker.
class DockWidget : public ads::CDockWidget, public IDataWidget {
  Q_OBJECT
 public:
  explicit DockWidget(
      SessionManager* session = nullptr, CatalogModel* catalog = nullptr, ads::CDockManager* manager = nullptr,
      QWidget* parent = nullptr);
  explicit DockWidget(
      PlotWidget* plot, SessionManager* session = nullptr, CatalogModel* catalog = nullptr,
      ads::CDockManager* manager = nullptr, QWidget* parent = nullptr, bool create_plot_when_null = true);
  ~DockWidget() override;

  void setDataServices(SessionManager* session, CatalogModel* catalog);
  PlotWidget* plotWidget();
  PlotWidget* releasePlotWidget();
  void setPlotWidget(PlotWidget* plot);
  DockToolbar* toolBar();
  QString name() const;
  void setName(const QString& name);
  [[nodiscard]] QString stateId() const;
  void setStateId(QString id);

  // IDataWidget
  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double time) override;

 public slots:
  void onStylesheetChanged(QString theme);
  DockWidget* splitHorizontal();
  DockWidget* splitVertical();
  DockWidget* splitHorizontal(PlotWidget* plot);
  DockWidget* splitVertical(PlotWidget* plot);

 signals:
  void undoableChange();

 private:
  DockWidget* splitInto(ads::DockWidgetArea area, PlotWidget* plot);

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  PlotWidget* plot_widget_ = nullptr;
  DockToolbar* toolbar_ = nullptr;
  QString state_id_;
};

}  // namespace PJ
