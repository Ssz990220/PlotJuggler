#pragma once

#include <QWidget>

namespace Ui {
class TimelineWidget;
}

namespace PJ {

class PlaybackEngine;

// MAGENTA region at the bottom of the main window: current-time readout,
// loop toggle, play button, RealSlider, Speed spinner, Step-size spinner.
// Extracted from PJ3 mainwindow.ui `widgetTimescale` subtree.
//
// The widget is a view over a PlaybackEngine and forwards user interaction
// into the engine. When the engine emits change notifications the widget
// updates its display.
class TimelineWidget : public QWidget {
  Q_OBJECT
 public:
  explicit TimelineWidget(QWidget* parent = nullptr);
  ~TimelineWidget() override;

  void setPlaybackEngine(PlaybackEngine* engine);

 private slots:
  void onEngineTimeChanged(double t);
  void onEngineRangeChanged(double min, double max);
  void onEnginePlayingChanged(bool playing);
  void onEngineRateChanged(double rate);

  void onSliderValueChanged(double value);
  void onPlayToggled(bool checked);
  void onLoopToggled(bool checked);
  void onRateChanged(double value);
  void onStepChanged(double value);

 private:
  Ui::TimelineWidget* ui_;
  PlaybackEngine* engine_ = nullptr;
  bool updating_from_engine_ = false;
};

}  // namespace PJ
