#include "ui/TimelineWidget.h"

#include <QDoubleSpinBox>
#include <QPushButton>

#include "pj_app_core/PlaybackEngine.h"
#include "pj_app_core/SvgUtil.h"
#include "ui/RealSlider.h"
#include "ui_TimelineWidget.h"

namespace PJ {

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent), ui_(new Ui::TimelineWidget) {
  ui_->setupUi(this);

  const QString theme = currentTheme();
  ui_->playbackLoop->setIcon(LoadSvg(":/resources/svg/loop.svg", theme));
  ui_->buttonPlay->setIcon(LoadSvg(":/resources/svg/play_arrow.svg", theme));

  ui_->displayTime->setText("0.000");

  connect(ui_->timeSlider, &RealSlider::realValueChanged, this,
          &TimelineWidget::onSliderValueChanged);
  connect(ui_->buttonPlay, &QPushButton::toggled, this, &TimelineWidget::onPlayToggled);
  connect(ui_->playbackLoop, &QPushButton::toggled, this, &TimelineWidget::onLoopToggled);
  connect(ui_->playbackRate,
          static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged), this,
          &TimelineWidget::onRateChanged);
  connect(ui_->playbackStep,
          static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged), this,
          &TimelineWidget::onStepChanged);
}

TimelineWidget::~TimelineWidget() {
  delete ui_;
}

void TimelineWidget::setPlaybackEngine(PlaybackEngine* engine) {
  if (engine_) {
    disconnect(engine_, nullptr, this, nullptr);
  }
  engine_ = engine;
  if (!engine_) {
    return;
  }
  connect(engine_, &PlaybackEngine::currentTimeChanged, this, &TimelineWidget::onEngineTimeChanged);
  connect(engine_, &PlaybackEngine::rangeChanged, this, &TimelineWidget::onEngineRangeChanged);
  connect(engine_, &PlaybackEngine::playingChanged, this, &TimelineWidget::onEnginePlayingChanged);
  connect(engine_, &PlaybackEngine::playbackRateChanged, this, &TimelineWidget::onEngineRateChanged);

  onEngineRangeChanged(engine_->rangeMin(), engine_->rangeMax());
  onEngineTimeChanged(engine_->currentTime());
  onEnginePlayingChanged(engine_->isPlaying());
  onEngineRateChanged(engine_->playbackRate());
}

void TimelineWidget::onEngineTimeChanged(double t) {
  updating_from_engine_ = true;
  ui_->timeSlider->setRealValue(t);
  const QString formatted = QString::number(t, 'f', 3);
  if (formatted != ui_->displayTime->text()) {
    ui_->displayTime->setText(formatted);
  }
  updating_from_engine_ = false;
}

void TimelineWidget::onEngineRangeChanged(double min, double max) {
  const int steps = std::max(1, static_cast<int>((max - min) * 1000.0));
  updating_from_engine_ = true;
  ui_->timeSlider->setLimits(min, max, steps);
  updating_from_engine_ = false;
}

void TimelineWidget::onEnginePlayingChanged(bool playing) {
  updating_from_engine_ = true;
  ui_->buttonPlay->setChecked(playing);
  updating_from_engine_ = false;
}

void TimelineWidget::onEngineRateChanged(double rate) {
  updating_from_engine_ = true;
  ui_->playbackRate->setValue(rate);
  updating_from_engine_ = false;
}

void TimelineWidget::onSliderValueChanged(double value) {
  if (updating_from_engine_ || !engine_) {
    return;
  }
  engine_->setCurrentTime(value);
}

void TimelineWidget::onPlayToggled(bool checked) {
  if (updating_from_engine_ || !engine_) {
    return;
  }
  if (checked) {
    engine_->play();
  } else {
    engine_->pause();
  }
}

void TimelineWidget::onLoopToggled(bool checked) {
  if (updating_from_engine_ || !engine_) {
    return;
  }
  engine_->setLooping(checked);
}

void TimelineWidget::onRateChanged(double value) {
  if (updating_from_engine_ || !engine_) {
    return;
  }
  engine_->setPlaybackRate(value);
}

void TimelineWidget::onStepChanged(double value) {
  if (updating_from_engine_ || !engine_) {
    return;
  }
  engine_->setStep(value);
}

}  // namespace PJ
