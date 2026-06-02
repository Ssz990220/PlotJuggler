// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/ScrubberBase.h"

#include <QApplication>
#include <QCursor>
#include <QEnterEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QResizeEvent>
#include <QScreen>
#include <QTimer>
#include <QVariantAnimation>
#include <Qt>
#include <cmath>

#include "pj_widgets/SvgUtil.h"
#include "pj_widgets/ThemeColors.h"

namespace PJ {

ScrubberBase::ScrubberBase(QWidget* parent) : QWidget(parent) {
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  // Cursor is updated per-zone in mouseMoveEvent / enterEvent. Default
  // (un-hovered) is the arrow cursor.
  setAutoFillBackground(false);
  // Hug the text vertically so layouts can't stretch us back up.
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

ScrubberBase::~ScrubberBase() {
  if (state_ == State::Dragging && cursor_hidden_during_drag_) {
    QApplication::restoreOverrideCursor();
  }
}

void ScrubberBase::setPixelsPerStep(int px) {
  pixels_per_step_ = std::max(1, px);
}

QSize ScrubberBase::sizeHint() const {
  return {80, fontMetrics().height() + 4};
}

QSize ScrubberBase::minimumSizeHint() const {
  return {50, fontMetrics().height() + 2};
}

void ScrubberBase::valueRepaint() {
  update();
}

QRect ScrubberBase::leftArrowRect() const {
  return {0, 0, kArrowZoneWidth, height()};
}

QRect ScrubberBase::rightArrowRect() const {
  return {width() - kArrowZoneWidth, 0, kArrowZoneWidth, height()};
}

QRect ScrubberBase::centerRect() const {
  return {kArrowZoneWidth, 0, width() - 2 * kArrowZoneWidth, height()};
}

ScrubberBase::Zone ScrubberBase::zoneAt(const QPoint& pos) const {
  if (leftArrowRect().contains(pos)) {
    return Zone::LeftArrow;
  }
  if (rightArrowRect().contains(pos)) {
    return Zone::RightArrow;
  }
  return Zone::Body;
}

void ScrubberBase::updateCursorForPos(const QPoint& pos) {
  if (state_ == State::Dragging || state_ == State::Editing) {
    return;
  }
  switch (zoneAt(pos)) {
    case Zone::LeftArrow:
    case Zone::RightArrow:
      setCursor(Qt::PointingHandCursor);
      break;
    case Zone::Body:
      setCursor(Qt::SizeHorCursor);
      break;
  }
}

QRect ScrubberBase::screenGeometryAt(const QPointF& global_pos) {
  if (auto* s = QGuiApplication::screenAt(global_pos.toPoint())) {
    return s->geometry();
  }
  return {};
}

void ScrubberBase::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);

  const QPalette& pal = palette();
  // Theme-matched separator grey (same #c0c0c0 / #666666 as splitter
  // handles + treeview header borders, defined in stylesheet_*.qss).
  const bool light = currentTheme().contains("light");
  // Match palette token border_default in stylesheet_{light,dark}.qss.
  const QColor border_grey = light ? QColor(0xc0, 0xc0, 0xc0) : QColor(0xb0, 0xb0, 0xbf);
  const QColor active = light ? QColor(0x62, 0xc5, 0xff) : QColor(0x14, 0x8c, 0xd2);

  // Background fill: full rect. Colours come from pj_widgets/ThemeColors.h
  // so they stay in lockstep with the QSS `input_background` token used by
  // PJ::ComboBox — Qt's palette(base) brush varies per platform and landed
  // near-white in dark mode, defeating the dark theme.
  QPainterPath fill_path;
  fill_path.addRoundedRect(rect(), kCornerRadiusPx, kCornerRadiusPx);
  const QBrush fill_brush(light ? theme::kInputBackgroundLight : theme::kInputBackgroundDark);
  p.fillPath(fill_path, fill_brush);

  // Border stroke: inset by 0.5 px so the 1 px line lands cleanly on
  // pixel boundaries (otherwise antialiasing smears the edge across two
  // rows/columns and the rectangle looks uneven).
  qreal active_mix = hover_alpha_;
  if (state_ == State::Dragging || state_ == State::Editing || hasFocus()) {
    active_mix = 1.0;
  }
  const QColor mixed(
      static_cast<int>((border_grey.red() * (1.0 - active_mix)) + (active.red() * active_mix)),
      static_cast<int>((border_grey.green() * (1.0 - active_mix)) + (active.green() * active_mix)),
      static_cast<int>((border_grey.blue() * (1.0 - active_mix)) + (active.blue() * active_mix)), 255);
  QPainterPath stroke_path;
  stroke_path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), kCornerRadiusPx, kCornerRadiusPx);
  p.setPen(QPen(mixed, 1.0));
  p.setBrush(Qt::NoBrush);
  p.drawPath(stroke_path);

  // Centre text (skip while editing — the QLineEdit covers it)
  if (state_ != State::Editing) {
    p.setPen(pal.color(QPalette::Text));
    p.drawText(centerRect(), Qt::AlignCenter, displayText());
  }

  // Arrows: only fade in while hovered
  if (hover_alpha_ > 0.0 && state_ != State::Editing) {
    const QPixmap& left = LoadSvg(":/resources/svg/keyboard_arrow_left_light.svg", currentTheme());
    const QPixmap& right = LoadSvg(":/resources/svg/keyboard_arrow_right_light.svg", currentTheme());
    p.setOpacity(hover_alpha_);
    const QSize icon_size(12, 12);
    auto draw = [&](const QPixmap& pm, const QRect& zone) {
      QRect target(QPoint(0, 0), icon_size);
      target.moveCenter(zone.center());
      p.drawPixmap(target, pm);
    };
    draw(left, leftArrowRect());
    draw(right, rightArrowRect());
    p.setOpacity(1.0);
  }
}

void ScrubberBase::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    QWidget::mousePressEvent(event);
    return;
  }
  switch (zoneAt(event->pos())) {
    case Zone::LeftArrow:
      stepBy(-1);
      startAutoRepeat(-1);
      event->accept();
      return;
    case Zone::RightArrow:
      stepBy(+1);
      startAutoRepeat(+1);
      event->accept();
      return;
    case Zone::Body:
      state_ = State::Armed;
      press_screen_pos_ = event->globalPosition().toPoint();
      last_drag_global_ = event->globalPosition();
      accumulated_pixels_ = 0.0;
      total_move_ = 0.0;
      event->accept();
      return;
  }
}

void ScrubberBase::mouseMoveEvent(QMouseEvent* event) {
  if (state_ == State::Idle) {
    updateCursorForPos(event->pos());
  }
  if (state_ == State::Armed) {
    const qreal dx = event->globalPosition().x() - press_screen_pos_.x();
    const qreal dy = event->globalPosition().y() - press_screen_pos_.y();
    if (std::sqrt(dx * dx + dy * dy) > kClickDragThreshold) {
      startDrag();
    }
  }
  if (state_ == State::Dragging) {
    handleDragMove(event->globalPosition());
    event->accept();
    return;
  }
  QWidget::mouseMoveEvent(event);
}

void ScrubberBase::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    QWidget::mouseReleaseEvent(event);
    return;
  }
  if (autorepeat_direction_ != 0) {
    stopAutoRepeat();
    event->accept();
    return;
  }
  if (state_ == State::Armed) {
    // No drag happened — treat as click ⇒ enter edit.
    state_ = State::Idle;
    enterEditMode();
    event->accept();
    return;
  }
  if (state_ == State::Dragging) {
    endDrag();
    event->accept();
    return;
  }
  QWidget::mouseReleaseEvent(event);
}

void ScrubberBase::mouseDoubleClickEvent(QMouseEvent* event) {
  // Double-click on the body fast-paths into edit. Double-click on an
  // arrow falls through to mousePressEvent so it just steps twice.
  if (event->button() == Qt::LeftButton && state_ != State::Editing && zoneAt(event->pos()) == Zone::Body) {
    enterEditMode();
    event->accept();
    return;
  }
  QWidget::mouseDoubleClickEvent(event);
}

void ScrubberBase::enterEvent(QEnterEvent* event) {
  is_hovered_ = true;
  animateHover(true);
  updateCursorForPos(event->position().toPoint());
  QWidget::enterEvent(event);
}

void ScrubberBase::leaveEvent(QEvent* event) {
  is_hovered_ = false;
  animateHover(false);
  QWidget::leaveEvent(event);
}

void ScrubberBase::resizeEvent(QResizeEvent* event) {
  if (line_edit_ && state_ == State::Editing) {
    line_edit_->setGeometry(centerRect());
  }
  QWidget::resizeEvent(event);
}

void ScrubberBase::keyPressEvent(QKeyEvent* event) {
  // Editor handles its own keys; this only fires when the scrubber itself
  // has focus (not the line edit).
  if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
    enterEditMode();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

void ScrubberBase::startDrag() {
  state_ = State::Dragging;
  if (cursor_hidden_during_drag_) {
    QApplication::setOverrideCursor(Qt::BlankCursor);
  }
  update();
}

void ScrubberBase::endDrag() {
  if (cursor_hidden_during_drag_) {
    QApplication::restoreOverrideCursor();
  }
  QCursor::setPos(press_screen_pos_);
  state_ = State::Idle;
  update();
}

void ScrubberBase::handleDragMove(const QPointF& global_pos) {
  const qreal dx = global_pos.x() - last_drag_global_.x();
  total_move_ += std::abs(dx);
  accumulated_pixels_ += dx;

  while (accumulated_pixels_ >= pixels_per_step_) {
    stepBy(+1);
    accumulated_pixels_ -= pixels_per_step_;
  }
  while (accumulated_pixels_ <= -pixels_per_step_) {
    stepBy(-1);
    accumulated_pixels_ += pixels_per_step_;
  }

  // Wrap at screen edges so the scrub can continue forever.
  const QRect screen = screenGeometryAt(global_pos);
  if (!screen.isNull()) {
    if (global_pos.x() <= screen.left() + 1) {
      const QPoint warped(screen.right() - 2, static_cast<int>(global_pos.y()));
      QCursor::setPos(warped);
      last_drag_global_ = QPointF(warped);
      return;
    }
    if (global_pos.x() >= screen.right() - 1) {
      const QPoint warped(screen.left() + 2, static_cast<int>(global_pos.y()));
      QCursor::setPos(warped);
      last_drag_global_ = QPointF(warped);
      return;
    }
  }
  last_drag_global_ = global_pos;
}

QLineEdit* ScrubberBase::ensureLineEdit() {
  if (line_edit_) {
    return line_edit_;
  }
  line_edit_ = new QLineEdit(this);
  line_edit_->setFrame(false);
  line_edit_->setAlignment(Qt::AlignCenter);
  line_edit_->hide();
  // Enter commits; Escape and focus-out both revert (handled in
  // eventFilter — editingFinished is too coarse, it fires for both).
  connect(line_edit_, &QLineEdit::returnPressed, this, [this]() {
    if (state_ == State::Editing) {
      exitEditMode(/*commit=*/true);
    }
  });
  line_edit_->installEventFilter(this);
  return line_edit_;
}

bool ScrubberBase::eventFilter(QObject* obj, QEvent* event) {
  if (state_ == State::Editing) {
    // Line-edit-targeted: Escape and FocusOut both revert.
    if (obj == line_edit_) {
      if (event->type() == QEvent::FocusOut) {
        exitEditMode(/*commit=*/false);
      } else if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
          exitEditMode(/*commit=*/false);
          return true;
        }
      }
    }
    // App-wide: any mouse press outside the line edit reverts and falls
    // through to the target widget (we don't consume).
    if (event->type() == QEvent::MouseButtonPress) {
      auto* w = qobject_cast<QWidget*>(obj);
      if (w && line_edit_ && w != line_edit_ && !line_edit_->isAncestorOf(w)) {
        exitEditMode(/*commit=*/false);
      }
    }
  }
  return QWidget::eventFilter(obj, event);
}

void ScrubberBase::enterEditMode() {
  if (state_ == State::Editing) {
    return;
  }
  QLineEdit* le = ensureLineEdit();
  state_ = State::Editing;
  le->setText(displayText());
  le->setGeometry(centerRect());
  le->show();
  le->selectAll();
  le->setFocus(Qt::MouseFocusReason);
  // Catch clicks on no-focus widgets (sliders, labels, backdrops) which
  // would otherwise leave focus parked on the line edit and never revert.
  qApp->installEventFilter(this);
  update();
}

void ScrubberBase::exitEditMode(bool commit) {
  if (state_ != State::Editing) {
    return;
  }
  state_ = State::Idle;
  qApp->removeEventFilter(this);
  if (line_edit_) {
    if (commit) {
      const QString t = line_edit_->text();
      // commitText returns false on parse error / out-of-range — the
      // widget silently reverts to the prior value.
      commitText(t);
    }
    line_edit_->hide();
  }
  update();
}

void ScrubberBase::startAutoRepeat(int direction) {
  autorepeat_direction_ = direction;
  if (!autorepeat_timer_) {
    autorepeat_timer_ = new QTimer(this);
    autorepeat_timer_->setSingleShot(true);
    connect(autorepeat_timer_, &QTimer::timeout, this, &ScrubberBase::onAutoRepeatTick);
  }
  autorepeat_timer_->setInterval(kAutorepeatDelayMs);
  autorepeat_timer_->start();
}

void ScrubberBase::stopAutoRepeat() {
  autorepeat_direction_ = 0;
  if (autorepeat_timer_) {
    autorepeat_timer_->stop();
  }
}

void ScrubberBase::onAutoRepeatTick() {
  if (autorepeat_direction_ == 0) {
    return;
  }
  stepBy(autorepeat_direction_);
  autorepeat_timer_->setInterval(kAutorepeatIntervalMs);
  autorepeat_timer_->start();
}

void ScrubberBase::setHoverAlphaInternal(qreal alpha) {
  hover_alpha_ = alpha;
  update();
}

void ScrubberBase::animateHover(bool entering) {
  if (!hover_animation_) {
    hover_animation_ = new QVariantAnimation(this);
    hover_animation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(hover_animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
      setHoverAlphaInternal(v.toReal());
    });
  }
  hover_animation_->stop();
  hover_animation_->setDuration(kHoverFadeMs);
  hover_animation_->setStartValue(hover_alpha_);
  hover_animation_->setEndValue(entering ? 1.0 : 0.0);
  hover_animation_->start();
}

}  // namespace PJ
