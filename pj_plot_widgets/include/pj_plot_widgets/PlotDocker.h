#pragma once

#include <DockManager.h>

#include <QString>

namespace PJ {

class CatalogModel;
class DockWidget;
class PlotWidget;
class SessionManager;

// One tab's worth of plots. Owns an ads::CDockManager and the tree of
// DockWidgets splittable within it. Always keeps at least one DockWidget
// alive so the user never sees an empty tab.
class PlotDocker : public ads::CDockManager {
  Q_OBJECT
 public:
  explicit PlotDocker(
      QString name, SessionManager* session = nullptr, CatalogModel* catalog = nullptr, QWidget* parent = nullptr);
  ~PlotDocker() override;

  QString name() const {
    return name_;
  }
  void setName(QString name) {
    name_ = std::move(name);
  }
  void setDataServices(SessionManager* session, CatalogModel* catalog);

  int plotCount() const;
  DockWidget* plotAt(int index);

 signals:
  void dockAdded(DockWidget* dock);
  void plotWidgetAdded(PlotWidget* plot);
  void undoableChange();

 private:
  void ensureAtLeastOneWidget();

  QString name_;
  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
};

}  // namespace PJ
