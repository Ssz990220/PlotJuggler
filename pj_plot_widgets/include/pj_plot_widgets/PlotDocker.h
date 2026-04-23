#pragma once

#include <DockManager.h>

#include <QString>

namespace PJ {

class DockWidget;

// One tab's worth of plots. Owns an ads::CDockManager and the tree of
// DockWidgets splittable within it. Always keeps at least one DockWidget
// alive so the user never sees an empty tab.
class PlotDocker : public ads::CDockManager {
  Q_OBJECT
 public:
  explicit PlotDocker(QString name, QWidget* parent = nullptr);
  ~PlotDocker() override;

  QString name() const { return name_; }
  void setName(QString name) { name_ = std::move(name); }

  int plotCount() const;
  DockWidget* plotAt(int index);

 signals:
  void dockAdded(DockWidget* dock);
  void undoableChange();

 private:
  void ensureAtLeastOneWidget();

  QString name_;
};

}  // namespace PJ
