#pragma once

#include <DockWidget.h>

#include <QEvent>
#include <QPoint>
#include <QStringList>
#include <functional>

#include "pj_base/builtin/BuiltinObject.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/IDataWidget.h"

namespace PJ {

class CatalogModel;
class DockToolbar;
class PlotWidget;
class SessionManager;
class VisualizationPlaceholderWidget;

// ADS-backed dock hosting a single plot area. splitHorizontal /
// splitVertical create sibling DockWidgets inside the parent PlotDocker.
class DockWidget : public ads::CDockWidget, public IDataWidget {
  Q_OBJECT
 public:
  using ObjectWidgetFactory =
      std::function<IDataWidget*(ObjectTopicId, sdk::BuiltinObjectType, const QString&, QWidget*)>;

  explicit DockWidget(
      SessionManager* session = nullptr, CatalogModel* catalog = nullptr, ads::CDockManager* manager = nullptr,
      QWidget* parent = nullptr);
  explicit DockWidget(
      PlotWidget* plot, SessionManager* session = nullptr, CatalogModel* catalog = nullptr,
      ads::CDockManager* manager = nullptr, QWidget* parent = nullptr, bool create_plot_when_null = false);
  ~DockWidget() override;

  void setDataServices(SessionManager* session, CatalogModel* catalog);
  void setObjectWidgetFactory(ObjectWidgetFactory factory);
  PlotWidget* plotWidget();
  IDataWidget* objectWidget();
  PlotWidget* releasePlotWidget();
  void setPlotWidget(PlotWidget* plot);
  void setPlaceholderWidget();
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
  void plotWidgetCreated(PlotWidget* plot);

 private slots:
  void clearToPlaceholder();
  void onCatalogItemsDropped(const QStringList& keys);

 private:
  bool eventFilter(QObject* watched, QEvent* event) override;
  DockWidget* splitInto(ads::DockWidgetArea area, PlotWidget* plot);
  PlotWidget* ensurePlotWidget();
  void clearCurrentContent(bool delete_content);
  void installObjectContextMenuFilter(QWidget* root);
  void removeObjectContextMenuFilter(QWidget* root);
  void showObjectContextMenu(const QPoint& global_pos);

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  ObjectWidgetFactory object_widget_factory_;
  QWidget* content_widget_ = nullptr;
  VisualizationPlaceholderWidget* placeholder_widget_ = nullptr;
  PlotWidget* plot_widget_ = nullptr;
  IDataWidget* object_widget_ = nullptr;
  DockToolbar* toolbar_ = nullptr;
  QString state_id_;
};

}  // namespace PJ
