#pragma once

#include <DockManager.h>

#include <QDomDocument>
#include <QDomElement>
#include <QString>
#include <functional>

#include "pj_base/builtin/BuiltinObject.hpp"
#include "pj_datastore/object_store.hpp"

namespace PJ {

class CatalogModel;
class DockWidget;
class IDataWidget;
class PlotWidget;
class SessionManager;

// One tab's worth of plots. Owns an ads::CDockManager and the tree of
// DockWidgets splittable within it. Always keeps at least one DockWidget
// alive so the user never sees an empty tab.
class PlotDocker : public ads::CDockManager {
  Q_OBJECT
 public:
  using ObjectWidgetFactory =
      std::function<IDataWidget*(ObjectTopicId, sdk::BuiltinObjectType, const QString&, QWidget*)>;

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
  void setObjectWidgetFactory(ObjectWidgetFactory factory);
  [[nodiscard]] QString stateId() const;
  void setStateId(QString id);
  [[nodiscard]] QDomElement xmlSaveState(QDomDocument& doc) const;
  bool xmlLoadState(const QDomElement& tab_element);

  int plotCount() const;
  DockWidget* plotAt(int index);

 public slots:
  void onStylesheetChanged(QString theme);

 signals:
  void dockAdded(DockWidget* dock);
  void plotWidgetAdded(PlotWidget* plot);
  void undoableChange();

 private:
  void ensureAtLeastOneWidget();
  DockWidget* addDockWithPlot(PlotWidget* plot, ads::DockWidgetArea area, ads::CDockAreaWidget* relative_to = nullptr);

  QString state_id_;
  QString name_;
  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  ObjectWidgetFactory object_widget_factory_;
  bool restoring_state_ = false;
};

}  // namespace PJ
