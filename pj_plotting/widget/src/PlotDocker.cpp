// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotDocker.h"

#include <DockAreaTitleBar.h>
#include <DockAreaWidget.h>
#include <DockComponentsFactory.h>
#include <DockContainerWidget.h>
#include <DockSplitter.h>
#include <ads_globals.h>

#include <QDomDocument>
#include <QEvent>
#include <QHash>
#include <QSet>
#include <QSplitter>
#include <QStringList>
#include <QUuid>
#include <QVector>
#include <algorithm>
#include <utility>

#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotFocusOverlay.h"
#include "pj_plotting/PlotWidget.h"

namespace PJ {

namespace {

// We own the title via DockToolbar — the built-in ADS title bar is always
// hidden.
class HiddenTitleBar : public ads::CDockAreaTitleBar {
 public:
  using ads::CDockAreaTitleBar::CDockAreaTitleBar;
  void setVisible(bool /*visible*/) override {
    QWidget::setVisible(false);
  }
};

class SplittableComponentsFactory : public ads::CDockComponentsFactory {
 public:
  ads::CDockAreaTitleBar* createDockAreaTitleBar(ads::CDockAreaWidget* dock_area) const override {
    auto* title_bar = new HiddenTitleBar(dock_area);
    title_bar->setVisible(false);
    return title_bar;
  }
};

QString newStateId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QDomElement saveChildNodesState(QDomDocument& doc, QWidget* widget) {
  if (widget == nullptr) {
    return {};
  }

  if (auto* splitter = qobject_cast<QSplitter*>(widget)) {
    QDomElement splitter_element = doc.createElement(QStringLiteral("DockSplitter"));
    splitter_element.setAttribute(
        QStringLiteral("orientation"),
        splitter->orientation() == Qt::Horizontal ? QStringLiteral("|") : QStringLiteral("-"));
    splitter_element.setAttribute(QStringLiteral("count"), QString::number(splitter->count()));

    QStringList normalized_sizes;
    int total_size = 0;
    for (int size : splitter->sizes()) {
      total_size += size;
    }
    for (int index = 0; index < splitter->count(); ++index) {
      const int size = splitter->sizes().value(index, 0);
      const double normalized = total_size > 0 ? static_cast<double>(size) / static_cast<double>(total_size)
                                               : 1.0 / static_cast<double>(std::max(1, splitter->count()));
      normalized_sizes.push_back(QString::number(normalized, 'f', 8));
    }
    splitter_element.setAttribute(QStringLiteral("sizes"), normalized_sizes.join(QStringLiteral(";")));

    for (int index = 0; index < splitter->count(); ++index) {
      QDomElement child = saveChildNodesState(doc, splitter->widget(index));
      if (!child.isNull()) {
        splitter_element.appendChild(child);
      }
    }
    return splitter_element;
  }

  auto* dock_area = qobject_cast<ads::CDockAreaWidget*>(widget);
  if (dock_area == nullptr) {
    return {};
  }

  QDomElement area_element = doc.createElement(QStringLiteral("DockArea"));
  for (int index = 0; index < dock_area->dockWidgetsCount(); ++index) {
    auto* dock_widget = dynamic_cast<DockWidget*>(dock_area->dockWidget(index));
    if (dock_widget == nullptr) {
      continue;
    }
    // Plot widgets emit <plot>; object widgets (Scene3DDockWidget, …)
    // emit whatever their xmlSaveState returns (e.g. <scene3d>). A null
    // QDomElement from an object widget means "I don't persist state yet",
    // so we skip it honestly rather than writing an empty stub.
    QDomElement payload;
    if (auto* plot = dock_widget->plotWidget()) {
      payload = plot->xmlSaveState(doc);
    } else if (auto* obj = dock_widget->objectWidget()) {
      payload = obj->xmlSaveState(doc);
      if (payload.isNull()) {
        continue;
      }
    } else {
      continue;
    }
    area_element.setAttribute(QStringLiteral("id"), dock_widget->stateId());
    area_element.setAttribute(QStringLiteral("name"), dock_widget->name());
    area_element.appendChild(payload);
  }
  return area_element;
}

struct LayoutNode {
  enum class Type { Area, Splitter };

  Type type = Type::Area;
  Qt::Orientation orientation = Qt::Horizontal;
  QVector<double> size_ratios;
  QString area_id;
  QString area_name;
  QVector<QDomElement> plots;
  QVector<LayoutNode> children;
  bool valid = false;
};

LayoutNode parseLayoutNode(const QDomElement& element) {
  LayoutNode node;
  if (element.isNull()) {
    return node;
  }

  if (element.tagName() == QStringLiteral("DockSplitter")) {
    node.type = LayoutNode::Type::Splitter;
    node.valid = true;
    node.orientation = element.attribute(QStringLiteral("orientation")).startsWith(QStringLiteral("|")) ? Qt::Horizontal
                                                                                                        : Qt::Vertical;
    for (const QString& size : element.attribute(QStringLiteral("sizes")).split(';', Qt::SkipEmptyParts)) {
      bool ok = false;
      const double value = size.toDouble(&ok);
      if (ok) {
        node.size_ratios.push_back(value);
      }
    }
    for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
      LayoutNode child_node = parseLayoutNode(child);
      if (child_node.valid) {
        node.children.push_back(std::move(child_node));
      }
    }
    node.valid = !node.children.isEmpty();
    return node;
  }

  if (element.tagName() == QStringLiteral("DockArea")) {
    node.type = LayoutNode::Type::Area;
    node.valid = true;
    node.area_id = element.attribute(QStringLiteral("id"));
    node.area_name = element.attribute(QStringLiteral("name"));
    // node.plots holds the per-dock content element regardless of kind — a
    // <plot> (PlotWidget) or an object widget's own tag (<scene3d>, <scene2d>,
    // …). Each <DockArea> writes exactly one payload per dock, so collect every
    // child element and let restore discriminate by tagName().
    for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
      node.plots.push_back(child);
    }
    return node;
  }

  return node;
}

// First leaf-area dock element reachable from `node`, descending into the
// first child of each splitter. Used to decide between plot-pool and
// empty-object-widget construction at the top of the restore tree.
QDomElement firstLeafElement(const LayoutNode& node) {
  const LayoutNode* cur = &node;
  while (cur->type == LayoutNode::Type::Splitter && !cur->children.isEmpty()) {
    cur = &cur->children.front();
  }
  if (cur->type == LayoutNode::Type::Area && !cur->plots.isEmpty()) {
    return cur->plots.front();
  }
  return {};
}

// A dock's content element is an object widget (Scene3DDockWidget,
// Scene2DDockWidget, …) when it is present and is not the PlotWidget's own
// <plot> element. Restore hands its tagName() to the object-widget factory as
// the "kind", so pj_plotting stays agnostic to specific scene families.
bool isObjectWidgetElement(const QDomElement& element) {
  return !element.isNull() && element.tagName() != QStringLiteral("plot");
}

class RestorePlotPool {
 public:
  RestorePlotPool(QVector<DockWidget*> docks, SessionManager* session, CatalogModel* catalog, QWidget* restored_parent)
      : session_(session), catalog_(catalog), restored_parent_(restored_parent) {
    for (DockWidget* dock : docks) {
      if (dock == nullptr) {
        continue;
      }
      PlotWidget* plot = dock->releasePlotWidget();
      if (plot == nullptr) {
        continue;
      }
      plot->setDataServices(session_, catalog_);
      plots_by_position_.push_back(plot);
      const QString id = plot->stateId();
      if (!id.isEmpty() && !plots_by_id_.contains(id)) {
        plots_by_id_.insert(id, plot);
      }
    }
  }

  PlotWidget* takeFirstForNode(const LayoutNode& node) {
    if (node.type == LayoutNode::Type::Area) {
      return takeForArea(node);
    }
    for (const LayoutNode& child : node.children) {
      return takeFirstForNode(child);
    }
    return createPlot({});
  }

  PlotWidget* takeForArea(const LayoutNode& node) {
    const QDomElement plot_element = node.plots.isEmpty() ? QDomElement{} : node.plots.front();
    const QString plot_id = plot_element.attribute(QStringLiteral("id"));
    if (!plot_id.isEmpty()) {
      auto it = plots_by_id_.find(plot_id);
      if (it != plots_by_id_.end() && !used_.contains(it.value())) {
        PlotWidget* plot = it.value();
        used_.insert(plot);
        return plot;
      }
    }

    while (next_position_ < plots_by_position_.size()) {
      PlotWidget* plot = plots_by_position_.at(next_position_++);
      if (plot != nullptr && !used_.contains(plot)) {
        used_.insert(plot);
        return plot;
      }
    }

    return createPlot(plot_id);
  }

  void deleteUnused() {
    for (PlotWidget* plot : plots_by_position_) {
      if (plot != nullptr && !used_.contains(plot)) {
        plot->deleteLater();
      }
    }
  }

 private:
  PlotWidget* createPlot(const QString& state_id) {
    auto* plot = new PlotWidget(session_, catalog_, restored_parent_);
    plot->setStateId(state_id);
    used_.insert(plot);
    plots_by_position_.push_back(plot);
    if (!plot->stateId().isEmpty() && !plots_by_id_.contains(plot->stateId())) {
      plots_by_id_.insert(plot->stateId(), plot);
    }
    return plot;
  }

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  QWidget* restored_parent_ = nullptr;
  QVector<PlotWidget*> plots_by_position_;
  QHash<QString, PlotWidget*> plots_by_id_;
  QSet<PlotWidget*> used_;
  qsizetype next_position_ = 0;
};

void applySplitterSizes(const LayoutNode& node, const QVector<DockWidget*>& widgets) {
  if (node.size_ratios.size() != widgets.size() || widgets.isEmpty()) {
    return;
  }
  auto* splitter = ads::internal::findParent<ads::CDockSplitter*>(widgets.back());
  if (splitter == nullptr) {
    return;
  }

  int total_size = 0;
  for (DockWidget* widget : widgets) {
    total_size += node.orientation == Qt::Horizontal ? widget->width() : widget->height();
  }
  if (total_size <= 0) {
    total_size = static_cast<int>(widgets.size()) * 100;
  }

  QList<int> sizes;
  for (double ratio : node.size_ratios) {
    sizes.push_back(std::max(1, static_cast<int>(ratio * static_cast<double>(total_size))));
  }
  splitter->setSizes(sizes);
}

void restoreNode(
    const LayoutNode& node, DockWidget* widget, RestorePlotPool& pool,
    const PlotDocker::ObjectWidgetFactory& object_factory) {
  if (widget == nullptr) {
    return;
  }

  if (node.type == LayoutNode::Type::Area) {
    widget->setStateId(node.area_id);
    widget->setName(node.area_name.isEmpty() ? QStringLiteral("...") : node.area_name);

    const QDomElement dock_element = node.plots.isEmpty() ? QDomElement{} : node.plots.front();

    if (isObjectWidgetElement(dock_element)) {
      // Object-widget path: build an empty dock by kind (the XML tag), then ask
      // it to load its own payload. Unknown kind (no factory / null result) →
      // leave the dock as-is rather than mis-parsing a scene element as a plot.
      IDataWidget* obj = object_factory ? object_factory(dock_element.tagName(), nullptr, widget) : nullptr;
      if (obj != nullptr) {
        widget->setObjectWidget(obj);
        obj->xmlLoadState(dock_element);
      }
      return;
    }

    // Plot path
    PlotWidget* plot = widget->plotWidget();
    if (plot == nullptr) {
      plot = pool.takeForArea(node);
      widget->setPlotWidget(plot);
    }
    if (!dock_element.isNull()) {
      plot->xmlLoadState(dock_element);
    } else {
      plot->removeAllCurves();
    }
    return;
  }

  if (node.children.isEmpty()) {
    return;
  }

  QVector<DockWidget*> widgets;
  widgets.push_back(widget);
  DockWidget* split_anchor = widget;
  for (qsizetype index = 1; index < node.children.size(); ++index) {
    const LayoutNode& child = node.children.at(index);
    const bool child_is_object = isObjectWidgetElement(firstLeafElement(child));
    if (child_is_object) {
      // No plot needed — the leaf will install an object widget itself.
      split_anchor =
          node.orientation == Qt::Horizontal ? split_anchor->splitHorizontal() : split_anchor->splitVertical();
    } else {
      PlotWidget* child_plot = pool.takeFirstForNode(child);
      split_anchor = node.orientation == Qt::Horizontal ? split_anchor->splitHorizontal(child_plot)
                                                        : split_anchor->splitVertical(child_plot);
    }
    if (split_anchor == nullptr) {
      return;
    }
    widgets.push_back(split_anchor);
  }
  applySplitterSizes(node, widgets);

  for (qsizetype index = 0; index < node.children.size() && index < widgets.size(); ++index) {
    restoreNode(node.children.at(index), widgets.at(index), pool, object_factory);
  }
}

}  // namespace

PlotDocker::PlotDocker(QString name, SessionManager* session, CatalogModel* catalog, QWidget* parent)
    : ads::CDockManager(parent), state_id_(newStateId()), name_(std::move(name)), session_(session), catalog_(catalog) {
  setStyleSheet("");  // Disable ADS's built-in stylesheet.
  setComponentsFactory(new SplittableComponentsFactory());

  connect(this, &ads::CDockManager::dockWidgetRemoved, this, [this](ads::CDockWidget* removed) {
    auto* removed_dock = qobject_cast<DockWidget*>(removed);
    const bool removed_was_focused = (removed_dock != nullptr && removed_dock == focused_dock_);
    ensureAtLeastOneWidget();
    // ADS doesn't move focus to a survivor when the focused dock is closed, so
    // do it here: the right panel mirrors the focused dock, and leaving focus
    // on a deleted dock would strand stale settings on screen.
    if (removed_was_focused && !restoring_state_) {
      refocusAfterRemoval(removed_dock);
    }
  });
  connect(this, &ads::CDockManager::dockAreasAdded, this, [this]() {
    if (!restoring_state_) {
      emit undoableChange();
    }
  });

  focus_overlay_ = new PlotFocusOverlay(*this);
  connect(
      this, &ads::CDockManager::focusedDockWidgetChanged, this,
      [this](ads::CDockWidget* /*old*/, ads::CDockWidget* now) {
        auto* now_dock = qobject_cast<DockWidget*>(now);
        if (now_dock != focused_dock_) {
          previous_dock_ = focused_dock_;
          focused_dock_ = now_dock;
        }
        focus_overlay_->setFocusedArea(now != nullptr ? now->dockAreaWidget() : nullptr);
        emit dockFocused(now_dock);
      });
  connect(this, &PlotDocker::plotWidgetAdded, this, &PlotDocker::watchPlotForHover);

  ensureAtLeastOneWidget();
}

PlotDocker::~PlotDocker() = default;

void PlotDocker::watchPlotForHover(PlotWidget* plot) {
  if (plot != nullptr) {
    plot->installHoverFilter(this);
  }
}

bool PlotDocker::eventFilter(QObject* watched, QEvent* event) {
  const QEvent::Type type = event->type();
  if (focus_overlay_ != nullptr && (type == QEvent::Enter || type == QEvent::Leave)) {
    // Walk up from the watched child (canvas or axis widget) to its
    // containing DockWidget so we can hand the overlay its CDockAreaWidget.
    for (QObject* node = watched; node != nullptr; node = node->parent()) {
      auto* dock_widget = qobject_cast<ads::CDockWidget*>(node);
      if (dock_widget == nullptr) {
        continue;
      }
      focus_overlay_->setHoveredArea(type == QEvent::Enter ? dock_widget->dockAreaWidget() : nullptr);
      break;
    }
  }
  return ads::CDockManager::eventFilter(watched, event);
}

void PlotDocker::setDataServices(SessionManager* session, CatalogModel* catalog) {
  session_ = session;
  catalog_ = catalog;
  for (int index = 0; index < plotCount(); ++index) {
    if (auto* dock = plotAt(index)) {
      dock->setDataServices(session_, catalog_);
    }
  }
}

void PlotDocker::setObjectWidgetFactory(ObjectWidgetFactory factory) {
  object_widget_factory_ = std::move(factory);
  for (int index = 0; index < plotCount(); ++index) {
    if (auto* dock = plotAt(index)) {
      dock->setObjectWidgetFactory(object_widget_factory_);
    }
  }
}

QString PlotDocker::stateId() const {
  return state_id_;
}

void PlotDocker::setStateId(QString id) {
  if (!id.isEmpty()) {
    state_id_ = std::move(id);
  }
}

void PlotDocker::refocusAfterRemoval(DockWidget* removed) {
  DockWidget* target = nullptr;
  // Prefer the previously focused dock so the user lands back where they were.
  // QPointer keeps it null if it was deleted in the meantime.
  if (previous_dock_ != nullptr && previous_dock_ != removed) {
    target = previous_dock_;
  } else {
    // Otherwise focus the first surviving dock. ensureAtLeastOneWidget() has
    // already guaranteed at least one exists, so closing the last real widget
    // lands focus on the freshly created placeholder (an empty config panel).
    for (int i = 0; i < plotCount(); ++i) {
      if (DockWidget* dock = plotAt(i); dock != nullptr && dock != removed) {
        target = dock;
        break;
      }
    }
  }
  if (target != nullptr) {
    setDockWidgetFocused(target);
  }
}

void PlotDocker::ensureAtLeastOneWidget() {
  if (restoring_state_) {
    return;
  }
  if (dockAreaCount() != 0) {
    return;
  }
  addDockWithPlot(nullptr, ads::TopDockWidgetArea);
}

DockWidget* PlotDocker::addDockWithPlot(
    PlotWidget* plot, ads::DockWidgetArea dock_area, ads::CDockAreaWidget* relative_to) {
  auto* widget = new DockWidget(plot, session_, catalog_, this, nullptr, plot != nullptr);
  widget->setObjectWidgetFactory(object_widget_factory_);
  auto* area_widget = addDockWidget(dock_area, widget, relative_to);
  area_widget->setAllowedAreas(ads::OuterDockAreas);

  connect(widget, &DockWidget::undoableChange, this, &PlotDocker::undoableChange);
  connect(widget, &DockWidget::plotWidgetCreated, this, &PlotDocker::plotWidgetAdded);
  emit dockAdded(widget);
  if (widget->plotWidget() != nullptr) {
    emit plotWidgetAdded(widget->plotWidget());
  }
  return widget;
}

QDomElement PlotDocker::xmlSaveState(QDomDocument& doc) const {
  QDomElement tab_element = doc.createElement(QStringLiteral("Tab"));
  tab_element.setAttribute(QStringLiteral("id"), state_id_);
  tab_element.setAttribute(QStringLiteral("containers"), dockContainers().count());

  for (ads::CDockContainerWidget* container : dockContainers()) {
    QDomElement container_element = doc.createElement(QStringLiteral("Container"));
    QDomElement child =
        saveChildNodesState(doc, container->findChild<QSplitter*>(QString(), Qt::FindDirectChildrenOnly));
    if (!child.isNull()) {
      container_element.appendChild(child);
    }
    tab_element.appendChild(container_element);
  }
  return tab_element;
}

bool PlotDocker::xmlLoadState(const QDomElement& tab_element) {
  if (tab_element.isNull() || tab_element.tagName() != QStringLiteral("Tab")) {
    return false;
  }

  setStateId(tab_element.attribute(QStringLiteral("id")));
  if (tab_element.hasAttribute(QStringLiteral("tab_name"))) {
    setName(tab_element.attribute(QStringLiteral("tab_name")));
  }

  QVector<LayoutNode> container_nodes;
  for (QDomElement container = tab_element.firstChildElement(QStringLiteral("Container")); !container.isNull();
       container = container.nextSiblingElement(QStringLiteral("Container"))) {
    QDomElement child = container.firstChildElement(QStringLiteral("DockSplitter"));
    if (child.isNull()) {
      child = container.firstChildElement(QStringLiteral("DockArea"));
    }
    LayoutNode node = parseLayoutNode(child);
    if (node.valid) {
      container_nodes.push_back(std::move(node));
    }
  }

  if (container_nodes.isEmpty()) {
    return false;
  }

  const bool was_hidden = isHidden();
  if (!was_hidden) {
    hide();
  }

  restoring_state_ = true;
  QVector<DockWidget*> old_docks;
  for (int index = 0; index < plotCount(); ++index) {
    if (DockWidget* dock = plotAt(index)) {
      old_docks.push_back(dock);
    }
  }

  RestorePlotPool pool(old_docks, session_, catalog_, this);
  for (DockWidget* dock : old_docks) {
    removeDockWidget(dock);
    dock->deleteLater();
  }

  const LayoutNode& root_node = container_nodes.front();
  // Decide top-level dock kind by peeking the first leaf's content tag. An
  // object-widget root (<scene3d>, <scene2d>, …) needs a plot-less placeholder
  // DockWidget, into which restoreNode installs the dock via the factory.
  const bool root_is_object = isObjectWidgetElement(firstLeafElement(root_node));
  DockWidget* root_widget = nullptr;
  if (root_is_object) {
    root_widget = addDockWithPlot(nullptr, ads::TopDockWidgetArea);
  } else {
    PlotWidget* root_plot = pool.takeFirstForNode(root_node);
    root_widget = addDockWithPlot(root_plot, ads::TopDockWidgetArea);
  }
  restoreNode(root_node, root_widget, pool, object_widget_factory_);
  pool.deleteUnused();

  restoring_state_ = false;
  ensureAtLeastOneWidget();
  if (!was_hidden) {
    show();
  }
  return true;
}

int PlotDocker::plotCount() const {
  return dockAreaCount();
}

DockWidget* PlotDocker::plotAt(int index) {
  return dynamic_cast<DockWidget*>(dockArea(index)->currentDockWidget());
}

DockWidget* PlotDocker::focusedDock() const {
  return focused_dock_.data();
}

void PlotDocker::focusDock(DockWidget* dock) {
  if (dock == nullptr) {
    return;
  }
  if (dock == focused_dock_) {
    // Already focused: ADS won't re-emit focusedDockWidgetChanged, but the
    // dock's content just changed (e.g. a drop populated a focused placeholder
    // after every widget was closed), so refresh listeners directly.
    emit dockFocused(dock);
  } else {
    setDockWidgetFocused(dock);
  }
}

void PlotDocker::onStylesheetChanged(QString theme) {
  for (int index = 0; index < plotCount(); ++index) {
    if (auto* dock = plotAt(index)) {
      dock->onStylesheetChanged(theme);
    }
  }
}

}  // namespace PJ
