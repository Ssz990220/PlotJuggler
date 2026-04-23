#pragma once

#include <DockManager.h>

#include <QString>

namespace PJ {

class DockWidget;

// Tabs of the RED region. Each PlotDocker owns one `ads::CDockManager` and
// manages a tree of DockWidgets that can be split horizontally / vertically.
// Ported from PJ3 plot_docker.{h,cpp}, minus the PlotDataMapRef argument and
// the xml save/load (workspace persistence is Phase 2).
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
