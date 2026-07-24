// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/DualOptionsWidget.h"

#include <QEasingCurve>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QVariantAnimation>
#include <algorithm>

#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
constexpr auto kHPadding = theme::Space::Comfortable;
constexpr int kAnimationDurationMs = 140;

theme::Theme frameworkTheme() {
  const QColor window = QGuiApplication::palette().color(QPalette::Window);
  return theme::themeFor(window.lightness() >= 128);
}
}  // namespace

DualOptionsWidget::DualOptionsWidget(QWidget* parent) : DualOptionsWidget(u"Option A"_s, u"Option B"_s, parent) {}

DualOptionsWidget::DualOptionsWidget(const QString& opt0, const QString& opt1, QWidget* parent)
    : DualOptionsWidget(QStringList{opt0, opt1}, parent) {}

DualOptionsWidget::DualOptionsWidget(const QStringList& options, QWidget* parent)
    : QWidget(parent),
      // Defaults mirror the framework roles so the widget looks correct before
      // a stylesheet injects the qproperties.
      accent_color_(theme::surface(PJ::theme::Surface::Separation, frameworkTheme())),
      border_color_(theme::surface(PJ::theme::Surface::Separation, frameworkTheme())),
      base_fill_color_(theme::surface(theme::Surface::Input, frameworkTheme())),
      selected_fill_color_(theme::interaction(theme::Variant::Accent, theme::State::Checked, frameworkTheme())),
      text_color_(theme::text(frameworkTheme())) {
  selection_animation_ = new QVariantAnimation(this);
  selection_animation_->setDuration(kAnimationDurationMs);
  selection_animation_->setEasingCurve(QEasingCurve::OutCubic);
  connect(selection_animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    visual_selection_ = value.toReal();
    update();
  });

  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::TabFocus);
  setAttribute(Qt::WA_Hover, true);
  setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  setOptions(options);
}

void DualOptionsWidget::setOptions(const QStringList& options) {
  if (options.size() < 2) {
    return;
  }
  options_ = options;
  if (selected_ >= optionCount()) {
    setSelectedIndex(optionCount() - 1);
  }
  updateGeometry();
  update();
}

void DualOptionsWidget::setOptions(const QString& opt0, const QString& opt1) {
  setOptions(QStringList{opt0, opt1});
}

void DualOptionsWidget::setSelectedIndex(int index) {
  if (index == selected_ || index < 0 || index >= optionCount()) {
    return;
  }
  selected_ = index;
  animateSelectedIndex(index);
  emit selectionChanged(selected_);
}

void DualOptionsWidget::setAccentColor(const QColor& color) {
  if (accent_color_ == color) {
    return;
  }
  accent_color_ = color;
  update();
}

void DualOptionsWidget::setBorderColor(const QColor& color) {
  if (border_color_ == color) {
    return;
  }
  border_color_ = color;
  update();
}

void DualOptionsWidget::setBaseFillColor(const QColor& color) {
  if (base_fill_color_ == color) {
    return;
  }
  base_fill_color_ = color;
  update();
}

void DualOptionsWidget::setSelectedFillColor(const QColor& color) {
  if (selected_fill_color_ == color) {
    return;
  }
  selected_fill_color_ = color;
  update();
}

void DualOptionsWidget::setTextColor(const QColor& color) {
  if (text_color_ == color) {
    return;
  }
  text_color_ = color;
  update();
}

QSize DualOptionsWidget::sizeHint() const {
  const QFontMetrics fm = fontMetrics();
  int max_w = 0;
  for (const QString& option : options_) {
    max_w = std::max(max_w, fm.horizontalAdvance(option));
  }
  const int segment_w = max_w + 2 * theme::space(kHPadding);
  return {optionCount() * segment_w, theme::metric(theme::Metric::InputOuterHeight)};
}

QSize DualOptionsWidget::minimumSizeHint() const {
  return sizeHint();
}

bool DualOptionsWidget::event(QEvent* event) {
  switch (event->type()) {
    case QEvent::Enter:
    case QEvent::Leave:
    case QEvent::HoverEnter:
    case QEvent::HoverLeave:
    case QEvent::FocusIn:
    case QEvent::FocusOut:
      update();
      break;
    default:
      break;
  }
  return QWidget::event(event);
}

void DualOptionsWidget::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  const auto fw_theme = frameworkTheme();
  const qreal border_width = theme::stroke(theme::Stroke::Hairline, fw_theme);
  const qreal corner_radius = theme::radius(theme::Radius::Input, fw_theme);

  const QRectF box =
      QRectF(rect()).adjusted(border_width / 2.0, border_width / 2.0, -border_width / 2.0, -border_width / 2.0);
  const qreal segment_w = box.width() / optionCount();

  const QColor bg =
      isEnabled() ? base_fill_color_ : theme::interaction(theme::Variant::Neutral, theme::State::Disabled, fw_theme);
  const QColor sel_fill =
      isEnabled() ? selected_fill_color_ : theme::interaction(theme::Variant::Accent, theme::State::Disabled, fw_theme);
  const QColor base_border = !isEnabled()                   ? theme::surface(PJ::theme::Surface::Separation, fw_theme)
                             : (underMouse() || hasFocus()) ? accent_color_
                                                            : border_color_;
  const QColor selected_border = isEnabled() ? accent_color_ : theme::surface(PJ::theme::Surface::Separation, fw_theme);

  painter.setBrush(bg);
  painter.setPen(QPen(base_border, border_width));
  painter.drawRoundedRect(box, corner_radius, corner_radius);

  const qreal selected_left = box.left() + segment_w * visual_selection_;
  const QRectF selected_box(selected_left, box.top(), segment_w, box.height());
  painter.setBrush(sel_fill);
  painter.setPen(QPen(selected_border, border_width));
  painter.drawRoundedRect(selected_box, corner_radius, corner_radius);

  const QColor label_color =
      isEnabled() ? text_color_ : theme::onSurface(theme::Surface::Backdrop, theme::Emphasis::Disabled, fw_theme);
  painter.setPen(label_color);
  for (int i = 0; i < optionCount(); ++i) {
    painter.drawText(
        QRectF(box.left() + segment_w * i, box.top(), segment_w, box.height()), Qt::AlignCenter, options_[i]);
  }
}

void DualOptionsWidget::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    const int index = static_cast<int>(event->pos().x() * optionCount() / std::max(1, width()));
    setSelectedIndex(std::clamp(index, 0, optionCount() - 1));
    event->accept();
    return;
  }
  QWidget::mousePressEvent(event);
}

void DualOptionsWidget::keyPressEvent(QKeyEvent* event) {
  switch (event->key()) {
    case Qt::Key_Left:
      setSelectedIndex(selected_ - 1);
      event->accept();
      break;
    case Qt::Key_Right:
      setSelectedIndex(selected_ + 1);
      event->accept();
      break;
    case Qt::Key_Space:
    case Qt::Key_Return:
      // Cycles through the segments; for two options this is the classic toggle.
      setSelectedIndex((selected_ + 1) % optionCount());
      event->accept();
      break;
    default:
      QWidget::keyPressEvent(event);
  }
}

void DualOptionsWidget::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::EnabledChange) {
    update();
  }
}

void DualOptionsWidget::animateSelectedIndex(int index) {
  const qreal target = static_cast<qreal>(index);
  if (selection_animation_ == nullptr || !isVisible()) {
    visual_selection_ = target;
    update();
    return;
  }

  selection_animation_->stop();
  selection_animation_->setStartValue(visual_selection_);
  selection_animation_->setEndValue(target);
  selection_animation_->start();
}

}  // namespace PJ
