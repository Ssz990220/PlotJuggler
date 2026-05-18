#include "pj_plotting/DockWidget.h"

#include <DockAreaWidget.h>
#include <DockManager.h>

#include <QBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QUuid>
#include <utility>

#include "pj_plotting/DockToolbar.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_widgets/VisualizationPlaceholderWidget.h"

namespace PJ {
namespace {

QString newStateId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

}  // namespace

DockWidget::DockWidget(SessionManager* session, CatalogModel* catalog, ads::CDockManager* manager, QWidget* parent)
    : DockWidget(nullptr, session, catalog, manager, parent, false) {}

DockWidget::DockWidget(
    PlotWidget* plot, SessionManager* session, CatalogModel* catalog, ads::CDockManager* manager, QWidget* parent,
    bool create_plot_when_null)
    : ads::CDockWidget(manager, "Plot", parent != nullptr ? parent : manager),
      session_(session),
      catalog_(catalog),
      state_id_(newStateId()) {
  setFrameShape(QFrame::NoFrame);

  setFeature(ads::CDockWidget::DockWidgetFloatable, false);
  setFeature(ads::CDockWidget::DockWidgetDeleteOnClose, true);

  toolbar_ = new DockToolbar(this);
  toolbar_->label()->setText("...");
  qobject_cast<QBoxLayout*>(layout())->insertWidget(0, toolbar_);

  connect(toolbar_->buttonSplitHorizontal(), &QPushButton::clicked, this, [this]() { splitHorizontal(); });
  connect(toolbar_->buttonSplitVertical(), &QPushButton::clicked, this, [this]() { splitVertical(); });

  auto fullscreenAction = [this]() {
    auto* parent_docker = qobject_cast<PlotDocker*>(dockManager());
    if (!parent_docker) {
      return;
    }
    toolbar_->toggleFullscreen();
    const bool fullscreen = toolbar_->isFullscreen();
    for (int i = 0; i < parent_docker->dockAreaCount(); ++i) {
      auto* area = parent_docker->dockArea(i);
      if (area != dockAreaWidget()) {
        area->setVisible(!fullscreen);
      }
      toolbar_->buttonClose()->setHidden(fullscreen);
    }
  };
  connect(toolbar_->buttonFullscreen(), &QPushButton::clicked, this, fullscreenAction);

  connect(toolbar_->buttonClose(), &QPushButton::pressed, this, [this]() {
    dockAreaWidget()->closeArea();
    clearCurrentContent(true);
    emit undoableChange();
  });

  layout()->setContentsMargins(10, 10, 10, 10);
  if (plot != nullptr || create_plot_when_null) {
    setPlotWidget(plot != nullptr ? plot : new PlotWidget(session_, catalog_, this));
  } else {
    setPlaceholderWidget();
  }
}

DockWidget::~DockWidget() = default;

void DockWidget::setDataServices(SessionManager* session, CatalogModel* catalog) {
  session_ = session;
  catalog_ = catalog;
  if (plot_widget_ != nullptr) {
    plot_widget_->setDataServices(session_, catalog_);
  }
}

void DockWidget::setObjectWidgetFactory(ObjectWidgetFactory factory) {
  object_widget_factory_ = std::move(factory);
}

PlotWidget* DockWidget::plotWidget() {
  return plot_widget_;
}

IDataWidget* DockWidget::objectWidget() {
  return object_widget_;
}

PlotWidget* DockWidget::releasePlotWidget() {
  if (plot_widget_ == nullptr) {
    return nullptr;
  }
  disconnect(plot_widget_, nullptr, this, nullptr);
  auto* plot = plot_widget_;
  takeWidget();
  content_widget_ = nullptr;
  plot_widget_ = nullptr;
  return plot;
}

void DockWidget::setPlotWidget(PlotWidget* plot) {
  if (plot_widget_ == plot) {
    return;
  }
  clearCurrentContent(true);
  plot_widget_ = plot;
  content_widget_ = plot_widget_;
  if (plot_widget_ == nullptr) {
    return;
  }
  plot_widget_->setDataServices(session_, catalog_);
  setWidget(plot_widget_);
  connect(plot_widget_, &PlotWidget::splitHorizontal, this, [this]() { splitHorizontal(); });
  connect(plot_widget_, &PlotWidget::splitVertical, this, [this]() { splitVertical(); });
  connect(plot_widget_, &PlotWidget::undoableChange, this, &DockWidget::undoableChange);
  emit plotWidgetCreated(plot_widget_);
}

void DockWidget::setPlaceholderWidget() {
  clearCurrentContent(true);
  placeholder_widget_ = new VisualizationPlaceholderWidget(this);
  content_widget_ = placeholder_widget_;
  setWidget(placeholder_widget_);
  setName(QStringLiteral("..."));
  connect(
      placeholder_widget_, &VisualizationPlaceholderWidget::catalogItemsDropped, this,
      &DockWidget::onCatalogItemsDropped);
}

DockToolbar* DockWidget::toolBar() {
  return toolbar_;
}

QString DockWidget::name() const {
  return toolbar_->label()->text();
}

void DockWidget::setName(const QString& name) {
  toolbar_->label()->setText(name);
}

QString DockWidget::stateId() const {
  return state_id_;
}

void DockWidget::setStateId(QString id) {
  if (!id.isEmpty()) {
    state_id_ = std::move(id);
  }
}

void DockWidget::onTrackerTime(double time) {
  if (plot_widget_ != nullptr) {
    plot_widget_->setTrackerPosition(time);
  }
  if (object_widget_ != nullptr) {
    object_widget_->onTrackerTime(time);
  }
}

void DockWidget::onStylesheetChanged(QString theme) {
  if (toolbar_ != nullptr) {
    toolbar_->onStylesheetChanged(theme);
  }
  if (placeholder_widget_ != nullptr) {
    placeholder_widget_->onStylesheetChanged(theme);
  }
}

DockWidget* DockWidget::splitHorizontal() {
  return splitHorizontal(nullptr);
}

DockWidget* DockWidget::splitVertical() {
  return splitVertical(nullptr);
}

DockWidget* DockWidget::splitHorizontal(PlotWidget* plot) {
  return splitInto(ads::RightDockWidgetArea, plot);
}

DockWidget* DockWidget::splitVertical(PlotWidget* plot) {
  return splitInto(ads::BottomDockWidgetArea, plot);
}

DockWidget* DockWidget::splitInto(ads::DockWidgetArea dock_area, PlotWidget* plot) {
  auto* parent_docker = qobject_cast<PlotDocker*>(dockManager());
  if (!parent_docker) {
    return nullptr;
  }
  auto* new_widget = new DockWidget(plot, session_, catalog_, parent_docker, nullptr, false);
  new_widget->setObjectWidgetFactory(object_widget_factory_);
  auto* area = parent_docker->addDockWidget(dock_area, new_widget, dockAreaWidget());
  area->setAllowedAreas(ads::OuterDockAreas);

  connect(new_widget, &DockWidget::undoableChange, parent_docker, &PlotDocker::undoableChange);
  connect(new_widget, &DockWidget::plotWidgetCreated, parent_docker, &PlotDocker::plotWidgetAdded);
  emit undoableChange();
  emit parent_docker->dockAdded(new_widget);
  if (new_widget->plotWidget() != nullptr) {
    emit parent_docker->plotWidgetAdded(new_widget->plotWidget());
  }
  return new_widget;
}

PlotWidget* DockWidget::ensurePlotWidget() {
  if (plot_widget_ == nullptr) {
    setPlotWidget(new PlotWidget(session_, catalog_, this));
    setName(QStringLiteral("..."));
  }
  return plot_widget_;
}

void DockWidget::onCatalogItemsDropped(const QStringList& keys) {
  if (catalog_ == nullptr || keys.empty()) {
    return;
  }

  const auto first_item = catalog_->itemDescriptor(keys.front());
  if (!first_item.has_value()) {
    return;
  }

  if (isScalarField(*first_item)) {
    PlotWidget* plot = ensurePlotWidget();
    bool changed = false;
    for (const QString& key : keys) {
      if (catalog_->curveDescriptor(key).has_value()) {
        changed = plot->addCurve(key) != nullptr || changed;
      }
    }
    if (changed) {
      plot->zoomOut(true);
      emit undoableChange();
    }
    return;
  }

  const auto* object_payload = asObjectTopic(*first_item);
  if (object_payload == nullptr || !object_widget_factory_) {
    return;
  }

  // The factory itself decides which object types it can host — returning
  // nullptr means "I can't render this", which we surface by reverting to
  // the placeholder so the user sees an explicit "not supported" affordance.
  const QString title = first_item->dataset_name.isEmpty()
                            ? first_item->topic_name
                            : QStringLiteral("%1/%2").arg(first_item->dataset_name, first_item->topic_name);
  clearCurrentContent(true);
  object_widget_ = object_widget_factory_(object_payload->object_topic_id, object_payload->object_type, title, this);
  content_widget_ = object_widget_ != nullptr ? object_widget_->widget() : nullptr;
  if (content_widget_ == nullptr) {
    setPlaceholderWidget();
    return;
  }
  setWidget(content_widget_);
  setName(first_item->topic_name);
  emit undoableChange();
}

void DockWidget::clearCurrentContent(bool delete_content) {
  if (plot_widget_ != nullptr) {
    disconnect(plot_widget_, nullptr, this, nullptr);
  }
  if (placeholder_widget_ != nullptr) {
    disconnect(placeholder_widget_, nullptr, this, nullptr);
  }
  if (content_widget_ != nullptr) {
    takeWidget();
    if (delete_content) {
      content_widget_->deleteLater();
    }
  }
  content_widget_ = nullptr;
  placeholder_widget_ = nullptr;
  plot_widget_ = nullptr;
  object_widget_ = nullptr;
}

}  // namespace PJ
