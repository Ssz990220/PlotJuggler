// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/TimelineWidget.h"

#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSvgRenderer>
#include <Qt>
#include <algorithm>
#include <cstdint>

#include "pj_runtime/PlaybackEngine.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/RealSlider.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_TimelineWidget.h"

namespace PJ {

namespace {

// Rasterise a monochrome Material SVG at a given logical size, honouring the
// caller widget's devicePixelRatio so QLabel::setPixmap stays crisp on HiDPI
// screens. Applies the same #000000 / #ffffff recolour as LoadSvg.
QPixmap renderSvgPixmap(const QString& path, const QString& theme, const QSize& logical_size, qreal dpr) {
  QFile file(path);
  if (!file.open(QFile::ReadOnly | QFile::Text)) {
    return {};
  }
  QByteArray svg_data = file.readAll();
  file.close();
  RecolorSvgInk(svg_data, theme.contains("light"));
  QSvgRenderer renderer(svg_data);
  const QSize physical = logical_size * dpr;
  QImage image(physical, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  renderer.render(&painter);
  painter.end();
  QPixmap pm = QPixmap::fromImage(image);
  pm.setDevicePixelRatio(dpr);
  return pm;
}
}  // namespace

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent), ui_(new Ui::TimelineWidget) {
  ui_->setupUi(this);

  applyIcons(currentTheme());

  ui_->displayTime->setText("0.000");

  // 30 Hz throttle window for slider-driven seeks. Synchronous image decode on
  // the GUI thread takes ~30-120 ms per topic; without this window each slider
  // tick during a drag would chain a fresh decode and the visible image lags
  // arbitrarily far behind the slider.
  seek_throttle_timer_.setSingleShot(true);
  seek_throttle_timer_.setInterval(33);
  connect(&seek_throttle_timer_, &QTimer::timeout, this, &TimelineWidget::flushPendingSeek);

  connect(ui_->timeSlider, &RealSlider::realValueChanged, this, &TimelineWidget::onSliderValueChanged);
  connect(ui_->timeSlider, &QSlider::sliderReleased, this, &TimelineWidget::onSliderReleased);
  connect(ui_->buttonPlay, &QPushButton::toggled, this, &TimelineWidget::onPlayToggled);
  // Icon-only sync: swap play_arrow ↔ pause whenever the toggle flips,
  // regardless of whether the change came from the user or from
  // onEnginePlayingChanged below. Kept separate from onPlayToggled so
  // the visual swap runs even while updating_from_engine_ is set.
  connect(ui_->buttonPlay, &QPushButton::toggled, this, [this]() { applyPlayPauseIcon(currentTheme()); });
  connect(ui_->playbackLoop, &QPushButton::toggled, this, &TimelineWidget::onLoopToggled);
  connect(ui_->playbackRate, &DoubleScrubber::valueChanged, this, &TimelineWidget::onRateChanged);
  connect(ui_->playbackStep, &DoubleScrubber::valueChanged, this, &TimelineWidget::onStepChanged);
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

  onEngineRangeChanged(toAxisDouble(engine_->rangeMin()), toAxisDouble(engine_->rangeMax()));
  onEngineTimeChanged(toAxisDouble(engine_->currentTime()));
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
  // Step count = ~millisecond resolution over the range, computed in 64-bit and
  // clamped. Streaming seeds the engine range in absolute epoch seconds, so a
  // large (or t≈0-polluted) span makes `(max-min)*1000` overflow a 32-bit int,
  // wrapping to ≤0 and collapsing the RealSlider to two positions (start/end).
  // kMaxSliderSteps caps the count at ms resolution over any realistic span —
  // past sub-pixel granularity on any display — so a polluted span cannot
  // produce an absurd count.
  static constexpr int64_t kMaxSliderSteps = 10'000'000;
  const auto raw_steps = static_cast<int64_t>((max - min) * 1000.0);
  const int steps = static_cast<int>(std::clamp<int64_t>(raw_steps, 1, kMaxSliderSteps));
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
  if (seek_throttle_timer_.isActive()) {
    // Inside the throttle window — buffer the latest value. The trailing-edge
    // flush below will deliver whatever sits here once the window closes.
    pending_seek_value_ = value;
    has_pending_seek_ = true;
    return;
  }
  // Leading edge: open the window first so a long synchronous decode inside
  // setCurrentTime keeps the timer alive (it cannot tick on a blocked event
  // loop, but it will fire as soon as control returns).
  has_pending_seek_ = false;
  seek_throttle_timer_.start();
  engine_->setCurrentTime(displaySeconds(value));
}

void TimelineWidget::onSliderReleased() {
  if (updating_from_engine_ || !engine_) {
    return;
  }
  // Deliver the final drag position immediately rather than waiting out the
  // throttle window — the frame the user released on should appear at once, and
  // forward playback resumes from there with no extra latency.
  if (has_pending_seek_) {
    has_pending_seek_ = false;
    seek_throttle_timer_.stop();
    engine_->setCurrentTime(displaySeconds(pending_seek_value_));
  }
}

void TimelineWidget::flushPendingSeek() {
  if (!has_pending_seek_) {
    return;
  }
  if (!engine_) {
    has_pending_seek_ = false;
    return;
  }
  const double v = pending_seek_value_;
  has_pending_seek_ = false;
  seek_throttle_timer_.start();  // Re-arm so a long drag stays rate-limited.
  engine_->setCurrentTime(displaySeconds(v));
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

void TimelineWidget::onStylesheetChanged(QString theme) {
  applyIcons(theme);
}

void TimelineWidget::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  chrome_metrics_ = metrics;
  applyIcons(currentTheme());
}

void TimelineWidget::applyIcons(QString theme) {
  const QSize icon_sz(chrome_metrics_.icon_size, chrome_metrics_.icon_size);
  const int button_extent = chrome_metrics_.icon_size + chrome_metrics_.icon_padding;
  const int band_extent = button_extent + (2 * chrome_metrics_.layout_padding);
  ui_->playbackLoop->setIcon(LoadSvg(":/resources/svg/loop.svg", theme));
  ui_->playbackLoop->setIconSize(icon_sz);
  ui_->playbackLoop->setMinimumSize(button_extent, button_extent);
  ui_->playbackLoop->setMaximumSize(button_extent, button_extent);
  applyPlayPauseIcon(theme);
  ui_->buttonPlay->setIconSize(icon_sz);
  ui_->buttonPlay->setMinimumSize(button_extent, button_extent);
  ui_->buttonPlay->setMaximumSize(button_extent, button_extent);
  // Override .ui-baked 24-px height caps on the strip's non-button
  // controls so the row grows together when icons scale.
  ui_->displayTime->setMinimumHeight(button_extent);
  ui_->displayTime->setMaximumHeight(button_extent);
  ui_->timeSlider->setMinimumHeight(button_extent);
  ui_->timeSlider->setMaximumHeight(button_extent);
  ui_->frame->setMaximumHeight(button_extent);
  // Outer strip layout: padding goes here, so the playback widget
  // itself grows by 2 * layout_padding via sizeHint (the inner content
  // remains button_extent tall plus the margins).
  if (auto* layout = ui_->layoutTimescale) {
    layout->setContentsMargins(
        chrome_metrics_.layout_padding, chrome_metrics_.layout_padding, chrome_metrics_.layout_padding,
        chrome_metrics_.layout_padding);
    layout->setSpacing(chrome_metrics_.layout_spacing);
  }
  // Grow the TimelineWidget itself to band_extent so the .ui-baked
  // 24-px clamp on the host slot in MainWindow.ui doesn't clip the
  // taller buttons / scrubbers when Chrome metrics scale up. Without
  // this the outer slot stays at 24 and the inner row overflows.
  setMinimumHeight(band_extent);
  setMaximumHeight(band_extent);
  const qreal dpr = devicePixelRatioF();
  ui_->labelSpeed->setPixmap(renderSvgPixmap(":/resources/svg/acute.svg", theme, icon_sz, dpr));
  ui_->labelStep->setPixmap(renderSvgPixmap(":/resources/svg/move_selection_right.svg", theme, icon_sz, dpr));
}

void TimelineWidget::applyPlayPauseIcon(const QString& theme) {
  const char* icon = ui_->buttonPlay->isChecked() ? ":/resources/svg/pause.svg" : ":/resources/svg/play_arrow.svg";
  ui_->buttonPlay->setIcon(LoadSvg(QString::fromLatin1(icon), theme));
}

}  // namespace PJ
