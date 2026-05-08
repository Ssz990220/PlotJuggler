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

namespace PJ {
namespace {

QString newStateId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

}  // namespace

DockWidget::DockWidget(SessionManager* session, CatalogModel* catalog, ads::CDockManager* manager, QWidget* parent)
    : DockWidget(nullptr, session, catalog, manager, parent) {}

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
    takeWidget();
    if (plot_widget_) {
      plot_widget_->deleteLater();
      plot_widget_ = nullptr;
    }
    emit undoableChange();
  });

  layout()->setContentsMargins(10, 10, 10, 10);
  if (plot != nullptr || create_plot_when_null) {
    setPlotWidget(plot != nullptr ? plot : new PlotWidget(session_, catalog_, this));
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

PlotWidget* DockWidget::plotWidget() {
  return plot_widget_;
}

PlotWidget* DockWidget::releasePlotWidget() {
  if (plot_widget_ == nullptr) {
    return nullptr;
  }
  disconnect(plot_widget_, nullptr, this, nullptr);
  auto* plot = plot_widget_;
  takeWidget();
  plot_widget_ = nullptr;
  return plot;
}

void DockWidget::setPlotWidget(PlotWidget* plot) {
  if (plot_widget_ == plot) {
    return;
  }
  if (plot_widget_ != nullptr) {
    disconnect(plot_widget_, nullptr, this, nullptr);
    takeWidget();
  }
  plot_widget_ = plot;
  if (plot_widget_ == nullptr) {
    return;
  }
  plot_widget_->setDataServices(session_, catalog_);
  setWidget(plot_widget_);
  connect(plot_widget_, &PlotWidget::splitHorizontal, this, [this]() { splitHorizontal(); });
  connect(plot_widget_, &PlotWidget::splitVertical, this, [this]() { splitVertical(); });
  connect(plot_widget_, &PlotWidget::undoableChange, this, &DockWidget::undoableChange);
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
}

void DockWidget::onStylesheetChanged(QString theme) {
  if (toolbar_ != nullptr) {
    toolbar_->onStylesheetChanged(theme);
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
  auto* new_widget = new DockWidget(plot, session_, catalog_, parent_docker, nullptr, plot == nullptr);
  auto* area = parent_docker->addDockWidget(dock_area, new_widget, dockAreaWidget());
  area->setAllowedAreas(ads::OuterDockAreas);

  connect(new_widget, &DockWidget::undoableChange, parent_docker, &PlotDocker::undoableChange);
  emit undoableChange();
  emit parent_docker->dockAdded(new_widget);
  emit parent_docker->plotWidgetAdded(new_widget->plotWidget());
  return new_widget;
}

}  // namespace PJ
