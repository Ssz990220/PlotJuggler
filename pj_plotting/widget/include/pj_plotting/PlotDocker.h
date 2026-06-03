#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <DockManager.h>

#include <QDomDocument>
#include <QDomElement>
#include <QPointer>
#include <QString>
#include <functional>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"

namespace PJ {

class CatalogModel;
class DockWidget;
class IDataWidget;
struct ObjectDropSeed;
class PlotFocusOverlay;
class PlotWidget;
class SessionManager;

// One tab's worth of plots. Owns an ads::CDockManager and the tree of
// DockWidgets splittable within it. Always keeps at least one DockWidget
// alive so the user never sees an empty tab.
class PlotDocker : public ads::CDockManager {
  Q_OBJECT
 public:
  // One factory for both paths. Layout restore calls it with the saved XML tag
  // as `kind` and a null seed; a catalog drop calls it with an empty kind and a
  // non-null seed. Null return = "not an object widget" (restore falls back to
  // the plot path).
  using ObjectWidgetFactory =
      std::function<IDataWidget*(const QString& kind, const ObjectDropSeed* seed, QWidget* parent)>;

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
  // The dock that currently holds focus in this tab (the one driving the right
  // config panel), or nullptr if none. Tracked from focusedDockWidgetChanged.
  [[nodiscard]] DockWidget* focusedDock() const;
  // Focus `dock` so its settings show. If it is already the focused dock, ADS
  // suppresses its focus-changed signal — so re-announce dockFocused directly,
  // which is what a drop populating an already-focused placeholder needs.
  void focusDock(DockWidget* dock);

 public slots:
  void onStylesheetChanged(QString theme);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 signals:
  void dockAdded(DockWidget* dock);
  void plotWidgetAdded(PlotWidget* plot);
  // Re-emit of ads::CDockManager::focusedDockWidgetChanged narrowed to
  // PJ::DockWidget so listeners don't pull in ADS types. Receivers
  // inspect dock->plotWidget()/dock->objectWidget() to know the kind.
  // nullptr when ADS focus moves to no widget.
  void dockFocused(DockWidget* dock);
  void undoableChange();

 private:
  void ensureAtLeastOneWidget();
  DockWidget* addDockWithPlot(PlotWidget* plot, ads::DockWidgetArea area, ads::CDockAreaWidget* relative_to = nullptr);
  void watchPlotForHover(PlotWidget* plot);
  // Move focus to a surviving dock after the focused one was removed, so its
  // settings stay visible. Prefers the previously focused dock; otherwise the
  // first remaining dock (which, after ensureAtLeastOneWidget, may be a fresh
  // placeholder when the last real widget was closed).
  void refocusAfterRemoval(DockWidget* removed);

  QString state_id_;
  QString name_;
  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  ObjectWidgetFactory object_widget_factory_;
  bool restoring_state_ = false;
  PlotFocusOverlay* focus_overlay_ = nullptr;
  // One-deep focus history, maintained from focusedDockWidgetChanged. Used to
  // restore focus to the previously active dock when the current one closes.
  QPointer<DockWidget> focused_dock_;
  QPointer<DockWidget> previous_dock_;
};

}  // namespace PJ
