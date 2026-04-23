#pragma once

#include <DockWidget.h>

#include "pj_app_core/IDataWidget.h"

namespace PJ {

class DockToolbar;

// ADS-backed dock that hosts a single plot area. Ported from PJ3
// plot_docker's DockWidget, with two stripped pieces:
//   - the embedded PlotWidget is replaced by a neutral placeholder QWidget
//     (Qwt lift is a later milestone)
//   - the background-colour drag-drop pathway is dropped (needs PlotWidget)
//
// splitHorizontal / splitVertical create sibling DockWidgets inside the
// parent PlotDocker, matching the PJ3 behaviour that drives the RED region.
class DockWidget : public ads::CDockWidget, public IDataWidget {
  Q_OBJECT
 public:
  explicit DockWidget(QWidget* parent = nullptr);
  ~DockWidget() override;

  QWidget* plotPlaceholder();
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
  QWidget* placeholder_ = nullptr;
  DockToolbar* toolbar_ = nullptr;
};

}  // namespace PJ
