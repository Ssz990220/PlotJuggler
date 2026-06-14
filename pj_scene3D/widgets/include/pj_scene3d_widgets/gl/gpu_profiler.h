// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QOpenGLContext>
#include <array>

#include "pj_scene3d_widgets/moving_average.h"

namespace pj::scene3d::gl {

// Non-stalling per-frame GPU timer for the whole scene render, smoothed with a
// moving average (see MovingAverage for the window).
//
// Usage each frame (with a current context):
//   profiler.beginFrame();   // ... draw the whole scene ...   profiler.endFrame();
// then read profiler.averageMillis().
//
// One GL_TIME_ELAPSED query brackets the frame's GPU work; the result is
// harvested from a query issued kRingDepth frames ago, so the CPU never blocks
// waiting for the GPU to finish (reading the current frame's result would
// stall). Per-context: releaseGL() must run under the dying context.
class GpuProfiler {
 public:
  GpuProfiler() = default;
  ~GpuProfiler();

  GpuProfiler(GpuProfiler&&) = delete;
  GpuProfiler& operator=(GpuProfiler&&) = delete;
  GpuProfiler(const GpuProfiler&) = delete;
  GpuProfiler& operator=(const GpuProfiler&) = delete;

  // Harvest an older frame's elapsed time into the moving average, then start
  // timing this frame. No-op without a current context.
  void beginFrame();
  // Stop timing this frame.
  void endFrame();

  // Delete the query objects under the current (dying) context.
  void releaseGL();

  // True once at least one frame time has been harvested.
  [[nodiscard]] bool hasResult() const noexcept {
    return !avg_.empty();
  }
  // Moving average of the recent frame GPU times, in milliseconds (0 until
  // hasResult()).
  [[nodiscard]] double averageMillis() const noexcept {
    return avg_.average();
  }

 private:
  // Query ring depth: >= the GPU's pipeline/buffering depth so the harvested
  // query is reliably complete (3 is the usual minimum).
  static constexpr int kRingDepth = 3;

  std::array<unsigned int, kRingDepth> queries_{};  // GL_TIME_ELAPSED names (0 = uncreated)
  std::array<bool, kRingDepth> pending_{};          // slot has an unharvested result
  int write_ = 0;                                   // ring slot this frame times into
  bool active_ = false;                             // between beginFrame() and endFrame()

  MovingAverage avg_;  // smoothed frame GPU time (ms)
};

}  // namespace pj::scene3d::gl
