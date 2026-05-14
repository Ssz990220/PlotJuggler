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

#include "pj_runtime/PlaybackEngine.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/RealSlider.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_TimelineWidget.h"

namespace PJ {

namespace {

// Single source of truth for icon size on the timeline strip — applies to
// the loop / play QPushButtons (via setIconSize) and the Speed / Buffer /
// Step QLabels (via renderSvgPixmap). Change here, all icons move together.
constexpr QSize kTimelineIconSize{20, 20};

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

  connect(ui_->timeSlider, &RealSlider::realValueChanged, this, &TimelineWidget::onSliderValueChanged);
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

void TimelineWidget::onStylesheetChanged(QString theme) {
  applyIcons(theme);
}

void TimelineWidget::applyIcons(QString theme) {
  ui_->playbackLoop->setIcon(LoadSvg(":/resources/svg/loop.svg", theme));
  ui_->playbackLoop->setIconSize(kTimelineIconSize);
  applyPlayPauseIcon(theme);
  ui_->buttonPlay->setIconSize(kTimelineIconSize);
  const qreal dpr = devicePixelRatioF();
  ui_->labelSpeed->setPixmap(renderSvgPixmap(":/resources/svg/acute.svg", theme, kTimelineIconSize, dpr));
  ui_->labelStep->setPixmap(renderSvgPixmap(":/resources/svg/move_selection_right.svg", theme, kTimelineIconSize, dpr));
}

void TimelineWidget::applyPlayPauseIcon(const QString& theme) {
  const char* icon = ui_->buttonPlay->isChecked() ? ":/resources/svg/pause.svg" : ":/resources/svg/play_arrow.svg";
  ui_->buttonPlay->setIcon(LoadSvg(QString::fromLatin1(icon), theme));
}

}  // namespace PJ
