#pragma once

#include <DockWidget.h>

#include "pj_app_core/IDataWidget.h"

namespace PJ {

class DockToolbar;

// ADS-backed dock hosting a single plot area. splitHorizontal /
// splitVertical create sibling DockWidgets inside the parent PlotDocker.
// The embedded placeholder QWidget is replaced by a real PlotWidget when
// the Qwt lift lands.
class DockWidget : public ads::CDockWidget, public IDataWidget {
  Q_OBJECT
 public:
  explicit DockWidget(QWidget* parent = nullptr);
  ~DockWidget() override;

  DockToolbar* toolBar();
  QString name() const;

  // IDataWidget
  QWidget* widget() override { return this; }
  void onTrackerTime(double time) override;

 public slots:
  DockWidget* splitHorizontal();
  DockWidget* splitVertical();

 signals:
  void undoableChange();

 private:
  DockWidget* splitInto(ads::DockWidgetArea area);

  QWidget* placeholder_ = nullptr;
  DockToolbar* toolbar_ = nullptr;
};

}  // namespace PJ
