#pragma once

class QWidget;

namespace PJ {

// Contract implemented by every widget family (plot / 2D / 3D) so pj_runtime
// can drive tracker updates without coupling to concrete widget types.
class IDataWidget {
 public:
  virtual ~IDataWidget() = default;

  virtual QWidget* widget() = 0;

  virtual void onTrackerTime(double time) = 0;
};

}  // namespace PJ
