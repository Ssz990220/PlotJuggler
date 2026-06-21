// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/PlaybackEngine.h"

#include <algorithm>
#include <cmath>

namespace PJ {

namespace {
// ~30 Hz. Nothing the cursor drives needs 60 Hz: video tops out ~30 Hz, ROS
// messages 10-20 Hz, the curve-list Value column is capped at 10 Hz, and a 30 Hz
// tracker line is already smooth (film is 24). Halving the tick halves the
// downstream replot/decode/composite work for every consumer at once — capping
// once at the origin instead of throttling each widget separately. Playback speed
// is unaffected: onTick derives the cursor from elapsed wall-clock, not tick count.
constexpr int kTickIntervalMs = 33;  // ~30 Hz
}  // namespace

PlaybackEngine::PlaybackEngine(QObject* parent) : QObject(parent) {
  timer_.setInterval(kTickIntervalMs);
  connect(&timer_, &QTimer::timeout, this, &PlaybackEngine::onTick);
}

PlaybackEngine::~PlaybackEngine() = default;

void PlaybackEngine::setRange(DisplayRange range) {
  double min = range.min.value;
  double max = range.max.value;
  if (max < min) {
    std::swap(min, max);
  }
  if (range_min_ == min && range_max_ == max) {
    return;
  }
  range_min_ = min;
  range_max_ = max;
  emit rangeChanged(range_min_, range_max_);

  const double clamped = clampedTime(current_time_);
  if (clamped != current_time_) {
    current_time_ = clamped;
    emit currentTimeChanged(current_time_);
  }
}

void PlaybackEngine::setCurrentTime(DisplaySeconds t) {
  const double clamped = clampedTime(t.value);
  if (clamped == current_time_) {
    return;
  }
  current_time_ = clamped;
  emit currentTimeChanged(current_time_);
}

void PlaybackEngine::setPlaybackRate(double rate) {
  if (rate_ == rate) {
    return;
  }
  rate_ = rate;
  emit playbackRateChanged(rate_);
}

void PlaybackEngine::setStep(double step) {
  step_ = step;
}

void PlaybackEngine::setLooping(bool looping) {
  if (looping_ == looping) {
    return;
  }
  looping_ = looping;
  emit loopingChanged(looping_);
}

void PlaybackEngine::play() {
  if (playing_) {
    return;
  }
  playing_ = true;
  elapsed_.restart();
  timer_.start();
  emit playingChanged(playing_);
}

void PlaybackEngine::pause() {
  if (!playing_) {
    return;
  }
  playing_ = false;
  timer_.stop();
  emit playingChanged(playing_);
}

void PlaybackEngine::togglePlay() {
  if (playing_) {
    pause();
  } else {
    play();
  }
}

void PlaybackEngine::onTick() {
  const qint64 dt_ms = elapsed_.restart();
  const double dt = static_cast<double>(dt_ms) * 0.001;
  double next = current_time_ + dt * rate_;

  if (next > range_max_) {
    if (looping_) {
      const double span = range_max_ - range_min_;
      if (span > 0.0) {
        next = range_min_ + std::fmod(next - range_min_, span);
      } else {
        next = range_min_;
      }
    } else {
      next = range_max_;
      pause();
    }
  } else if (next < range_min_) {
    next = range_min_;
  }

  if (next != current_time_) {
    current_time_ = next;
    emit currentTimeChanged(current_time_);
  }
}

double PlaybackEngine::clampedTime(double t) const {
  return std::clamp(t, range_min_, range_max_);
}

}  // namespace PJ
