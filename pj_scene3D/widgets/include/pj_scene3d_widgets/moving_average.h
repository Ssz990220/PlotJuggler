// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <deque>

namespace pj::scene3d {

// Fixed-window moving average over the most recent N samples, backed by a
// std::deque used as a circular buffer. Smooths jittery per-frame timing
// numbers (GPU / CPU ms) for display. Single source for the window size, so the
// GPU and CPU readouts can't drift apart. Not thread-safe (render thread only).
class MovingAverage {
 public:
  explicit MovingAverage(std::size_t window = 30) : window_(window) {}

  void add(double value) {
    samples_.push_back(value);
    if (samples_.size() > window_) {
      samples_.pop_front();
    }
  }

  [[nodiscard]] bool empty() const noexcept {
    return samples_.empty();
  }

  // Mean of the buffered samples; 0 when empty.
  [[nodiscard]] double average() const noexcept {
    if (samples_.empty()) {
      return 0.0;
    }
    double sum = 0.0;
    for (const double value : samples_) {
      sum += value;
    }
    return sum / static_cast<double>(samples_.size());
  }

 private:
  std::size_t window_;
  std::deque<double> samples_;
};

}  // namespace pj::scene3d
