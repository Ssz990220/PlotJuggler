#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QTimer>
#include <QWidget>

#include "pj_widgets/ChromeMetrics.h"

namespace Ui {
class TimelineWidget;
}

namespace PJ {

class PlaybackEngine;

// Bottom time strip: current-time readout, loop toggle, play button,
// RealSlider, Speed / Step-size spinners. View over PlaybackEngine; user
// interaction calls into the engine and engine signals update the display.
class TimelineWidget : public QWidget {
  Q_OBJECT
 public:
  explicit TimelineWidget(QWidget* parent = nullptr);
  ~TimelineWidget() override;

  void setPlaybackEngine(PlaybackEngine* engine);

 public slots:
  void onStylesheetChanged(QString theme);

  // Rebinds Chrome metrics broadcast from MainWindow. Re-runs
  // applyIcons() so buttons, label pixmaps, layout padding, and the
  // spacing between row items all update atomically.
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 private slots:
  void onEngineTimeChanged(double t);
  void onEngineRangeChanged(double min, double max);
  void onEnginePlayingChanged(bool playing);
  void onEngineRateChanged(double rate);

  void onSliderValueChanged(double value);
  // Slider handle released after a drag: flush any throttled-but-undelivered
  // final position immediately so the released frame appears (and playback
  // resumes) without waiting out the remaining throttle window.
  void onSliderReleased();
  void onPlayToggled(bool checked);
  void onLoopToggled(bool checked);
  void onRateChanged(double value);
  void onStepChanged(double value);

 private:
  void applyIcons(QString theme);
  // Swaps the play button between play_arrow.svg and pause.svg based on
  // its current checked state, re-tinted for the given theme.
  void applyPlayPauseIcon(const QString& theme);

  // Slider-drag throttle: image decode on the GUI thread takes tens of ms per
  // tick; QSlider with 1 ms step resolution can emit thousands of valueChanged
  // events per drag. We rate-limit setCurrentTime to ~30 Hz with a leading-edge
  // + trailing-edge throttle so the final position is always committed without
  // backlog. See PlaybackEngine docs for context.
  void flushPendingSeek();

  Ui::TimelineWidget* ui_;
  PlaybackEngine* engine_ = nullptr;
  bool updating_from_engine_ = false;
  QTimer seek_throttle_timer_;
  double pending_seek_value_ = 0.0;
  bool has_pending_seek_ = false;

  // Chrome metrics from MainWindow::chromeMetricsChanged.
  ChromeMetrics chrome_metrics_;
};

}  // namespace PJ
