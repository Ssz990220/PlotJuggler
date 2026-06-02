#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QSlider>
#include <algorithm>
#include <cmath>

namespace PJ {

// Double-precision slider: wraps QSlider's integer API with a floating-point
// range.
class RealSlider : public QSlider {
  Q_OBJECT
 public:
  explicit RealSlider(QWidget* parent = nullptr) : QSlider(parent) {
    setLimits(0.0, 1.0, 1);
    connect(this, &QSlider::valueChanged, this, &RealSlider::onValueChanged);
  }

  void setLimits(double min, double max, int steps) {
    min_value_ = min;
    max_value_ = max;
    QSlider::setRange(0, steps);
  }

  double getValue() const {
    const int span = maximum() - minimum();
    if (span <= 0) {
      return min_value_;
    }
    const double ratio = static_cast<double>(value() - minimum()) / static_cast<double>(span);
    return (max_value_ - min_value_) * ratio + min_value_;
  }

  void setRealValue(double val) {
    val = std::clamp(val, min_value_, max_value_);
    if (max_value_ == min_value_) {
      QSlider::setValue(minimum());
      return;
    }
    const double ratio = (val - min_value_) / (max_value_ - min_value_);
    const long pos = std::lround(static_cast<double>(maximum() - minimum()) * ratio + minimum());
    QSlider::setValue(static_cast<int>(pos));
  }

  double getMaximum() const {
    return max_value_;
  }
  double getMinimum() const {
    return min_value_;
  }

  void setRealStepValue(double step) {
    const double ratio = (max_value_ - min_value_) / static_cast<double>(maximum() - minimum());
    const int new_step = std::max(1, static_cast<int>(std::round(step / ratio)));
    QSlider::setSingleStep(new_step);
  }

 signals:
  void realValueChanged(double value);

 private slots:
  void onValueChanged(int value) {
    const int min = minimum();
    const int max = maximum();
    const double ratio = static_cast<double>(value) / static_cast<double>(max - min);
    const double pos_x = (max_value_ - min_value_) * ratio + min_value_;
    emit realValueChanged(pos_x);
  }

 private:
  double min_value_ = 0.0;
  double max_value_ = 1.0;
};

}  // namespace PJ
