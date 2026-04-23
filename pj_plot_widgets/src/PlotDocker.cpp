#include "pj_plot_widgets/PlotDocker.h"

#include <DockAreaTitleBar.h>
#include <DockAreaWidget.h>
#include <DockComponentsFactory.h>

#include "pj_plot_widgets/DockWidget.h"

namespace PJ {

namespace {

// We own the title via DockToolbar — the built-in ADS title bar is always
// hidden.
class HiddenTitleBar : public ads::CDockAreaTitleBar {
 public:
  using ads::CDockAreaTitleBar::CDockAreaTitleBar;
  void setVisible(bool /*visible*/) override { QWidget::setVisible(false); }
};

class SplittableComponentsFactory : public ads::CDockComponentsFactory {
 public:
  ads::CDockAreaTitleBar* createDockAreaTitleBar(ads::CDockAreaWidget* dock_area) const override {
    auto* title_bar = new HiddenTitleBar(dock_area);
    title_bar->setVisible(false);
    return title_bar;
  }
};

}  // namespace

PlotDocker::PlotDocker(QString name, QWidget* parent)
    : ads::CDockManager(parent), name_(std::move(name)) {
  setStyleSheet("");  // Disable ADS's built-in stylesheet.
  setComponentsFactory(new SplittableComponentsFactory());

  connect(this, &ads::CDockManager::dockWidgetRemoved, this,
          [this](ads::CDockWidget*) { ensureAtLeastOneWidget(); });
  connect(this, &ads::CDockManager::dockAreasAdded, this, &PlotDocker::undoableChange);

  ensureAtLeastOneWidget();
}

PlotDocker::~PlotDocker() = default;

void PlotDocker::ensureAtLeastOneWidget() {
  if (dockAreaCount() != 0) {
    return;
  }
  auto* widget = new DockWidget(this);
  auto* area = addDockWidget(ads::TopDockWidgetArea, widget);
  area->setAllowedAreas(ads::OuterDockAreas);

  connect(widget, &DockWidget::undoableChange, this, &PlotDocker::undoableChange);
  emit dockAdded(widget);
}

int PlotDocker::plotCount() const {
  return dockAreaCount();
}

DockWidget* PlotDocker::plotAt(int index) {
  return dynamic_cast<DockWidget*>(dockArea(index)->currentDockWidget());
}

}  // namespace PJ
