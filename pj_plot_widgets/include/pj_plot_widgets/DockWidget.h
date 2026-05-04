#pragma once

#include <DockWidget.h>

#include "pj_app_core/IDataWidget.h"

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
  ~DockWidget() override;

  void setDataServices(SessionManager* session, CatalogModel* catalog);
  PlotWidget* plotWidget();
  DockToolbar* toolBar();
  QString name() const;

  // IDataWidget
  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double time) override;

 public slots:
  void onStylesheetChanged(QString theme);
  DockWidget* splitHorizontal();
  DockWidget* splitVertical();

 signals:
  void undoableChange();

 private:
  DockWidget* splitInto(ads::DockWidgetArea area);

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  PlotWidget* plot_widget_ = nullptr;
  DockToolbar* toolbar_ = nullptr;
};

}  // namespace PJ
