#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <memory>

#include "pj_runtime/Time.h"

namespace PJ {

// Authoritative tracker time + play/pause/loop state. Drives a QTimer while
// playing; every widget family subscribes to currentTimeChanged to stay in
// sync. Time is display-relative seconds (DisplaySeconds); the change SIGNALS
// stay bare double — they are a moc/vtable contract shared by every widget
// family's onTrackerTime(double), so they are deliberately not typed.
class PlaybackEngine : public QObject {
  Q_OBJECT
 public:
  using Ptr = std::shared_ptr<PlaybackEngine>;

  explicit PlaybackEngine(QObject* parent = nullptr);
  ~PlaybackEngine() override;

  PlaybackEngine(const PlaybackEngine&) = delete;
  PlaybackEngine& operator=(const PlaybackEngine&) = delete;

  [[nodiscard]] DisplaySeconds currentTime() const {
    return DisplaySeconds{current_time_};
  }
  [[nodiscard]] DisplaySeconds rangeMin() const {
    return DisplaySeconds{range_min_};
  }
  [[nodiscard]] DisplaySeconds rangeMax() const {
    return DisplaySeconds{range_max_};
  }
  double playbackRate() const {
    return rate_;
  }
  double step() const {
    return step_;
  }
  bool isPlaying() const {
    return playing_;
  }
  bool isLooping() const {
    return looping_;
  }

 public slots:
  void setRange(DisplayRange range);
  void setCurrentTime(DisplaySeconds t);
  void setPlaybackRate(double rate);
  void setStep(double step);
  void setLooping(bool looping);
  void play();
  void pause();
  void togglePlay();

 signals:
  void currentTimeChanged(double t);
  void rangeChanged(double min, double max);
  void playbackRateChanged(double rate);
  void playingChanged(bool playing);
  void loopingChanged(bool looping);

 private slots:
  void onTick();

 private:
  double clampedTime(double t) const;

  double current_time_ = 0.0;
  double range_min_ = 0.0;
  double range_max_ = 1.0;
  double rate_ = 1.0;
  double step_ = 0.0;
  bool playing_ = false;
  bool looping_ = false;

  QTimer timer_;
  QElapsedTimer elapsed_;
};

}  // namespace PJ
