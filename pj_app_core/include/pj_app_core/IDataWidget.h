#pragma once

class QWidget;

namespace PJ {

// Tiny contract shared by the three widget families (plot, media, 3d) with
// pj_app_core. The widget implementations live in sibling modules and never
// depend on each other — they only depend on this header.
class IDataWidget {
 public:
  virtual ~IDataWidget() = default;

  virtual QWidget* widget() = 0;

  virtual void onTrackerTime(double time) = 0;
};

}  // namespace PJ
