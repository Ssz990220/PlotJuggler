#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

namespace PJ {

// Authoritative tracker time + play/pause/loop state. Drives a QTimer while
// playing; every widget family subscribes to currentTimeChanged to stay in
// sync.
class PlaybackEngine : public QObject {
  Q_OBJECT
 public:
  explicit PlaybackEngine(QObject* parent = nullptr);
  ~PlaybackEngine() override;

  PlaybackEngine(const PlaybackEngine&) = delete;
  PlaybackEngine& operator=(const PlaybackEngine&) = delete;

  double currentTime() const { return current_time_; }
  double rangeMin() const { return range_min_; }
  double rangeMax() const { return range_max_; }
  double playbackRate() const { return rate_; }
  double step() const { return step_; }
  bool isPlaying() const { return playing_; }
  bool isLooping() const { return looping_; }

 public slots:
  void setRange(double min, double max);
  void setCurrentTime(double t);
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
