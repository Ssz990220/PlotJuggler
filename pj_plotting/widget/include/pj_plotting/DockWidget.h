#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <DockWidget.h>

#include <QEvent>
#include <QPoint>
#include <QStringList>
#include <functional>

#include "pj_base/builtin/builtin_object.hpp"
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
  // One factory for both paths: layout restore calls it with a `kind` tag and a
  // null seed (then xmlLoadState repopulates); a catalog drop calls it with an
  // empty kind and a non-null seed (the factory classifies + populates the first
  // topic). A null return means "not an object widget for this input".
  using ObjectWidgetFactory =
      std::function<IDataWidget*(const QString& kind, const ObjectDropSeed* seed, QWidget* parent)>;

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
  // Restore-path counterpart of setPlotWidget for object widgets
  // (Scene3DDockWidget, …). The drop flow keeps installing its own object
  // widgets via the factory; this hook lets layout restore install one
  // it just constructed and then call xmlLoadState on it.
  void setObjectWidget(IDataWidget* widget);
  IDataWidget* releaseObjectWidget();
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
  // Resets the dock to the empty placeholder (same as the toolbar's "Clear").
  // A slot so the shell (via QMetaObject::invokeMethod) can reset a dock whose
  // bound data was evicted.
  void clearToPlaceholder();
  DockWidget* splitHorizontal();
  DockWidget* splitVertical();
  DockWidget* splitHorizontal(PlotWidget* plot);
  DockWidget* splitVertical(PlotWidget* plot);

 signals:
  void undoableChange();
  void plotWidgetCreated(PlotWidget* plot);

 private slots:
  void onCatalogItemsDropped(const QStringList& keys);

 private:
  bool eventFilter(QObject* watched, QEvent* event) override;
  DockWidget* splitInto(ads::DockWidgetArea area, PlotWidget* plot);
  PlotWidget* ensurePlotWidget();
  void clearCurrentContent(bool delete_content);
  void installObjectContextMenuFilter(QWidget* root);
  void removeObjectContextMenuFilter(QWidget* root);
  void showObjectContextMenu(const QPoint& global_pos);
  // Make this dock the focused one after it receives a drop, so its settings
  // become visible immediately (no-op if the manager has no focus controller).
  void focusSelf();

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
