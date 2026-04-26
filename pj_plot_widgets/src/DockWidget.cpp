#include "pj_plot_widgets/DockWidget.h"

#include <DockAreaWidget.h>
#include <DockManager.h>

#include <QBoxLayout>
#include <QLabel>
#include <QPushButton>

#include "pj_plot_widgets/DockToolbar.h"
#include "pj_plot_widgets/PlotDocker.h"
#include "pj_plot_widgets/PlotWidget.h"

namespace PJ {

DockWidget::DockWidget(SessionManager* session, CatalogModel* catalog, ads::CDockManager* manager, QWidget* parent)
    : ads::CDockWidget(manager, "Plot", parent != nullptr ? parent : manager), session_(session), catalog_(catalog) {
  setFrameShape(QFrame::NoFrame);

  plot_widget_ = new PlotWidget(session_, catalog_, this);
  setWidget(plot_widget_);
  setFeature(ads::CDockWidget::DockWidgetMovable, false);
  setFeature(ads::CDockWidget::DockWidgetFloatable, false);
  setFeature(ads::CDockWidget::DockWidgetDeleteOnClose, true);
  connect(plot_widget_, &PlotWidget::splitHorizontal, this, &DockWidget::splitHorizontal);
  connect(plot_widget_, &PlotWidget::splitVertical, this, &DockWidget::splitVertical);

  toolbar_ = new DockToolbar(this);
  toolbar_->label()->setText("...");
  qobject_cast<QBoxLayout*>(layout())->insertWidget(0, toolbar_);

  connect(toolbar_->buttonSplitHorizontal(), &QPushButton::clicked, this, &DockWidget::splitHorizontal);
  connect(toolbar_->buttonSplitVertical(), &QPushButton::clicked, this, &DockWidget::splitVertical);

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

DockToolbar* DockWidget::toolBar() {
  return toolbar_;
}

QString DockWidget::name() const {
  return toolbar_->label()->text();
}

void DockWidget::onTrackerTime(double time) {
  if (plot_widget_ != nullptr) {
    plot_widget_->setTrackerPosition(time);
  }
}

DockWidget* DockWidget::splitHorizontal() {
  return splitInto(ads::RightDockWidgetArea);
}

DockWidget* DockWidget::splitVertical() {
  return splitInto(ads::BottomDockWidgetArea);
}

DockWidget* DockWidget::splitInto(ads::DockWidgetArea dock_area) {
  auto* parent_docker = qobject_cast<PlotDocker*>(dockManager());
  if (!parent_docker) {
    return nullptr;
  }
  auto* new_widget = new DockWidget(session_, catalog_, parent_docker);
  auto* area = parent_docker->addDockWidget(dock_area, new_widget, dockAreaWidget());
  area->setAllowedAreas(ads::OuterDockAreas);

  connect(new_widget, &DockWidget::undoableChange, parent_docker, &PlotDocker::undoableChange);
  emit undoableChange();
  emit parent_docker->dockAdded(new_widget);
  emit parent_docker->plotWidgetAdded(new_widget->plotWidget());
  return new_widget;
}

}  // namespace PJ
