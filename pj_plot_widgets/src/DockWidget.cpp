#include "pj_plot_widgets/DockWidget.h"

#include <DockAreaWidget.h>
#include <DockManager.h>

#include <QBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QPushButton>

#include "pj_plot_widgets/DockToolbar.h"
#include "pj_plot_widgets/PlotDocker.h"

namespace PJ {

namespace {
QWidget* makePlaceholder(QWidget* parent) {
  auto* frame = new QFrame(parent);
  frame->setFrameShape(QFrame::StyledPanel);
  frame->setFrameShadow(QFrame::Sunken);
  frame->setStyleSheet("QFrame { background-color: palette(base); }");

  auto* layout = new QVBoxLayout(frame);
  auto* label = new QLabel(QObject::tr("plot placeholder"), frame);
  label->setAlignment(Qt::AlignCenter);
  label->setStyleSheet("QLabel { color: palette(mid); font-size: 14px; }");
  layout->addWidget(label);
  return frame;
}
}  // namespace

DockWidget::DockWidget(QWidget* parent) : ads::CDockWidget("Plot", parent) {
  setFrameShape(QFrame::NoFrame);

  placeholder_ = makePlaceholder(this);
  setWidget(placeholder_);
  setFeature(ads::CDockWidget::DockWidgetFloatable, false);
  setFeature(ads::CDockWidget::DockWidgetDeleteOnClose, true);

  toolbar_ = new DockToolbar(this);
  toolbar_->label()->setText("...");
  qobject_cast<QBoxLayout*>(layout())->insertWidget(0, toolbar_);

  connect(toolbar_->buttonSplitHorizontal(), &QPushButton::clicked, this,
          &DockWidget::splitHorizontal);
  connect(toolbar_->buttonSplitVertical(), &QPushButton::clicked, this,
          &DockWidget::splitVertical);

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
    if (placeholder_) {
      placeholder_->deleteLater();
      placeholder_ = nullptr;
    }
    emit undoableChange();
  });

  layout()->setContentsMargins(10, 10, 10, 10);
}

DockWidget::~DockWidget() = default;

QWidget* DockWidget::plotPlaceholder() { return placeholder_; }

DockToolbar* DockWidget::toolBar() { return toolbar_; }

QString DockWidget::name() const { return toolbar_->label()->text(); }

void DockWidget::onTrackerTime(double /*time*/) {
  // No-op in the prototype (no PlotWidget yet).
}

DockWidget* DockWidget::splitHorizontal() {
  auto* new_widget = new DockWidget(qobject_cast<QWidget*>(parent()));
  auto* parent_docker = qobject_cast<PlotDocker*>(dockManager());
  auto* area =
      parent_docker->addDockWidget(ads::RightDockWidgetArea, new_widget, dockAreaWidget());
  area->setAllowedAreas(ads::OuterDockAreas);

  connect(this, &DockWidget::undoableChange, parent_docker, &PlotDocker::undoableChange);
  emit undoableChange();
  emit parent_docker->dockAdded(new_widget);
  return new_widget;
}

DockWidget* DockWidget::splitVertical() {
  auto* new_widget = new DockWidget(qobject_cast<QWidget*>(parent()));
  auto* parent_docker = qobject_cast<PlotDocker*>(dockManager());
  auto* area =
      parent_docker->addDockWidget(ads::BottomDockWidgetArea, new_widget, dockAreaWidget());
  area->setAllowedAreas(ads::OuterDockAreas);

  connect(this, &DockWidget::undoableChange, parent_docker, &PlotDocker::undoableChange);
  emit undoableChange();
  emit parent_docker->dockAdded(new_widget);
  return new_widget;
}

}  // namespace PJ
