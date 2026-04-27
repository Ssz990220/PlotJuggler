#include "pj_plot_widgets/PlotDocker.h"

#include <DockAreaTitleBar.h>
#include <DockAreaWidget.h>
#include <DockComponentsFactory.h>

#include "pj_plot_widgets/DockWidget.h"
#include "pj_plot_widgets/PlotWidget.h"

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

}  // namespace

PlotDocker::PlotDocker(QString name, SessionManager* session, CatalogModel* catalog, QWidget* parent)
    : ads::CDockManager(parent), name_(std::move(name)), session_(session), catalog_(catalog) {
  setStyleSheet("");  // Disable ADS's built-in stylesheet.
  setComponentsFactory(new SplittableComponentsFactory());

  connect(this, &ads::CDockManager::dockWidgetRemoved, this, [this](ads::CDockWidget*) { ensureAtLeastOneWidget(); });
  connect(this, &ads::CDockManager::dockAreasAdded, this, &PlotDocker::undoableChange);

  ensureAtLeastOneWidget();
}

PlotDocker::~PlotDocker() = default;

void PlotDocker::setDataServices(SessionManager* session, CatalogModel* catalog) {
  session_ = session;
  catalog_ = catalog;
  for (int index = 0; index < plotCount(); ++index) {
    if (auto* dock = plotAt(index)) {
      dock->setDataServices(session_, catalog_);
    }
  }
}

void PlotDocker::ensureAtLeastOneWidget() {
  if (dockAreaCount() != 0) {
    return;
  }
  auto* widget = new DockWidget(session_, catalog_, this);
  auto* area = addDockWidget(ads::TopDockWidgetArea, widget);
  area->setAllowedAreas(ads::OuterDockAreas);

  connect(widget, &DockWidget::undoableChange, this, &PlotDocker::undoableChange);
  emit dockAdded(widget);
  emit plotWidgetAdded(widget->plotWidget());
}

int PlotDocker::plotCount() const {
  return dockAreaCount();
}

DockWidget* PlotDocker::plotAt(int index) {
  return dynamic_cast<DockWidget*>(dockArea(index)->currentDockWidget());
}

}  // namespace PJ
