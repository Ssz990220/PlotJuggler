// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Two-handle range slider implementation, wrapped in namespace PJ.
// See RangeSlider.h.

#include <pj_widgets/Hatch.h>
#include <pj_widgets/RangeSlider.h>

#include <QDebug>
#include <QRegion>
#include <algorithm>
#include <limits>

namespace PJ {

namespace {

// Geometry + colors mirror the app's playback slider (QSlider#timeSlider) so the
// range slider reads as the same control: a full-height rectangular track with a
// thin vertical handle. Those QSS color tokens are theme-independent
// (PJLightBlue/PJLightPurple/PJPurple are identical in light + dark, and
// border_default #B0B0BF vs #c0c0c0 is imperceptible), so hardcoding them here
// matches the playback slider on both themes without reading the theme.
const int kScHandleWidth = 8;   // timeSlider handle: 6px content + 1px border each side = 8px rendered
const int kScTrackHeight = 24;  // timeSlider groove + handle height (px)
const int kScLeftRightMargin = 1;

const QColor kGrooveBorder(0xB0, 0xB0, 0xBF);         // border_default
const QColor kSelection(0xC2, 0xDC, 0xFF);            // PJLightBlue (selected-range fill)
const QColor kHandle(0xFF, 0xAE, 0xFF);               // PJLightPurple (resting handle)
const QColor kHandleActive(0xCC, 0x00, 0xCC);         // PJPurple (hovered / pressed handle)
const QColor kHandleBorder(0xCC, 0x00, 0xCC);         // PJPurple (handle border)
const QColor kDisabledInk(0x80, 0x80, 0x80);          // muted grey when the slider is disabled
const QColor kMarkerLine(0x90, 0x90, 0x9A);           // chunk boundary line (muted)
const QColor kMarkerText(0x40, 0x40, 0x40);           // chunk label ink
const QColor kMarkerInRange(0xCC, 0x00, 0xCC, 0x4D);  // PJPurple @ ~30% — boxes overlapping the selection

}  // namespace

RangeSlider::RangeSlider(QWidget* a_parent) : QWidget(a_parent) {
  setMouseTracking(true);
}

RangeSlider::RangeSlider(Qt::Orientation ori, Options t, QWidget* a_parent)
    : QWidget(a_parent), orientation_(ori), type_(t) {
  setMouseTracking(true);
}

void RangeSlider::paintEvent(QPaintEvent* a_event) {
  Q_UNUSED(a_event);
  QPainter painter(this);

  // Groove geometry: a full-height rectangular track (timeSlider shape).
  QRectF background_rect;
  if (orientation_ == Qt::Horizontal) {
    background_rect = QRectF(kScLeftRightMargin, trackTop(), width() - kScLeftRightMargin * 2, kScTrackHeight);
  } else {
    background_rect =
        QRectF((width() - kScTrackHeight) / 2.0, kScLeftRightMargin, kScTrackHeight, height() - kScLeftRightMargin * 2);
  }

  const bool enabled = isEnabled();
  const QRectF left_handle_rect = firstHandleRect();
  const QRectF right_handle_rect = secondHandleRect();
  painter.setRenderHint(QPainter::Antialiasing, false);

  // 1. Selected-range fill (between the two handles) — PJLightBlue, like the
  //    playback slider's played sub-page. Drawn first; the groove border is
  //    stroked on top so it always reads crisply.
  QRectF selected_rect(background_rect);
  if (orientation_ == Qt::Horizontal) {
    selected_rect.setLeft(type_.testFlag(kLeftHandle) ? left_handle_rect.right() : left_handle_rect.left());
    selected_rect.setRight(type_.testFlag(kRightHandle) ? right_handle_rect.left() : right_handle_rect.right());
  } else {
    selected_rect.setTop(type_.testFlag(kLeftHandle) ? left_handle_rect.bottom() : left_handle_rect.top());
    selected_rect.setBottom(type_.testFlag(kRightHandle) ? right_handle_rect.top() : right_handle_rect.bottom());
  }
  painter.setPen(Qt::NoPen);
  painter.setBrush(enabled ? kSelection : kDisabledInk);
  painter.drawRect(selected_rect);

  // 1b. "No data" texture: the shared app hatch (PJ::drawNoDataHatch), over the UNSELECTED
  //     part of the TRACK (background_rect minus the [lower, upper] fill), in both enabled +
  //     disabled states. Phased to this widget's GLOBAL origin, so the diagonal lines line up
  //     with every other widget that paints the hatch (the Timeline, ...). CONTAINED to the
  //     track height: the hatch reads as one horizontal strip flanking the selection and never
  //     bleeds into the floating-label rows above/below the track. Clipped to the unselected
  //     region, so the lines stay STATIC as a handle drags (only the clip moves).
  {
    QRegion unselected(background_rect.toAlignedRect());
    unselected -= selected_rect.toAlignedRect();  // handles, drawn later, cover their own width on top
    painter.save();
    painter.setClipRegion(unselected);
    // Backdrop + hatch in ONE shared call (drawNoDataHatch), so the unselected track
    // composites the same ink over the same QPalette::Window backdrop the Timeline uses.
    // Without the backdrop fill the bare groove let the white dialog show through, giving
    // the identical ink ~26% more contrast (255 vs 238 backdrop) and a "busier" read.
    drawNoDataHatch(painter, background_rect, mapToGlobal(QPointF(0, 0)), enabled);
    painter.restore();
  }

  // 2. Groove outline — transparent body + 1px border (timeSlider groove:
  //    widget_background is transparent, border = border_default, square corners).
  painter.setPen(QPen(kGrooveBorder, 1));
  painter.setBrush(Qt::NoBrush);
  painter.drawRect(background_rect.adjusted(0.5, 0.5, -0.5, -0.5));

  if (!markers_.empty()) {
    drawMarkers(painter, background_rect);
  }

  // 3. Handles — thin full-height grips (timeSlider handle shape): PJLightPurple
  //    at rest, PJPurple when hovered or pressed, with a PJPurple border.
  auto paint_handle = [&](const QRectF& r, bool active) {
    painter.setPen(QPen(enabled ? kHandleBorder : kDisabledInk, 1));
    painter.setBrush(!enabled ? kDisabledInk.lighter(125) : (active ? kHandleActive : kHandle));
    painter.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
  };
  if (type_.testFlag(kLeftHandle)) {
    paint_handle(left_handle_rect, first_handle_pressed_ || hovered_handle_ == 1);
  }
  if (type_.testFlag(kRightHandle)) {
    paint_handle(right_handle_rect, second_handle_pressed_ || hovered_handle_ == 2);
  }

  if (floating_labels_) {
    drawFloatingLabels(painter);
  }
}

QRectF RangeSlider::firstHandleRect() const {
  float percentage = (lower_value_ - minimum_) * 1.0 / interval_;
  return handleRect(percentage * validLength() + kScLeftRightMargin);
}

QRectF RangeSlider::secondHandleRect() const {
  float percentage = (upper_value_ - minimum_) * 1.0 / interval_;
  return handleRect(
      percentage * validLength() + kScLeftRightMargin + (type_.testFlag(kLeftHandle) ? kScHandleWidth : 0));
}

QRectF RangeSlider::handleRect(int a_value) const {
  // Thin grip spanning the full track height (timeSlider handle: 6px wide,
  // groove-tall), centered across the short axis.
  if (orientation_ == Qt::Horizontal) {
    return QRect(a_value, trackTop(), kScHandleWidth, kScTrackHeight);
  } else {
    return QRect((width() - kScTrackHeight) / 2, a_value, kScTrackHeight, kScHandleWidth);
  }
}

void RangeSlider::mousePressEvent(QMouseEvent* a_event) {
  if (a_event->buttons() & Qt::LeftButton) {
    int pos_check, pos_max, pos_value, first_handle_rect_pos_value, second_handle_rect_pos_value;
    pos_check = (orientation_ == Qt::Horizontal) ? a_event->pos().y() : a_event->pos().x();
    pos_max = (orientation_ == Qt::Horizontal) ? height() : width();
    pos_value = (orientation_ == Qt::Horizontal) ? a_event->pos().x() : a_event->pos().y();
    first_handle_rect_pos_value = (orientation_ == Qt::Horizontal) ? firstHandleRect().x() : firstHandleRect().y();
    second_handle_rect_pos_value = (orientation_ == Qt::Horizontal) ? secondHandleRect().x() : secondHandleRect().y();

    // Floating labels double as hit-test targets.
    const bool on_lower_label =
        floating_labels_ && !lower_label_rect_.isNull() && lower_label_rect_.contains(a_event->pos());
    const bool on_upper_label =
        floating_labels_ && !upper_label_rect_.isNull() && upper_label_rect_.contains(a_event->pos());
    const bool on_center_label =
        floating_labels_ && !center_label_rect_.isNull() && center_label_rect_.contains(a_event->pos());

    second_handle_pressed_ =
        on_upper_label || (!on_lower_label && !on_center_label && secondHandleRect().contains(a_event->pos()));
    first_handle_pressed_ =
        on_lower_label || (!second_handle_pressed_ && !on_center_label && firstHandleRect().contains(a_event->pos()));
    range_drag_active_ = false;

    if (first_handle_pressed_) {
      delta_ = pos_value - (first_handle_rect_pos_value + kScHandleWidth / 2);
    } else if (second_handle_pressed_) {
      delta_ = pos_value - (second_handle_rect_pos_value + kScHandleWidth / 2);
    } else if (on_center_label && type_.testFlag(kDoubleHandles)) {
      range_drag_active_ = true;
      range_drag_start_pos_ = pos_value;
      range_drag_lower_start_ = lower_value_;
      range_drag_upper_start_ = upper_value_;
    } else if (
        type_.testFlag(kDoubleHandles) && pos_value > first_handle_rect_pos_value + kScHandleWidth &&
        pos_value < second_handle_rect_pos_value && pos_check >= 2 && pos_check <= pos_max - 2) {
      range_drag_active_ = true;
      range_drag_start_pos_ = pos_value;
      range_drag_lower_start_ = lower_value_;
      range_drag_upper_start_ = upper_value_;
    } else if (pos_check >= 2 && pos_check <= pos_max - 2) {
      int step = interval_ / 10 < 1 ? 1 : interval_ / 10;
      if (pos_value < first_handle_rect_pos_value) {
        setLowerValue(lower_value_ - step);
      } else if (pos_value > second_handle_rect_pos_value + kScHandleWidth) {
        setUpperValue(upper_value_ + step);
      }
    }
  }

  maybeShowHandleTooltip(a_event->globalPosition().toPoint(), a_event->pos());
}

void RangeSlider::mouseMoveEvent(QMouseEvent* a_event) {
  if (a_event->buttons() & Qt::LeftButton) {
    int pos_value, first_handle_rect_pos_value, second_handle_rect_pos_value;
    pos_value = (orientation_ == Qt::Horizontal) ? a_event->pos().x() : a_event->pos().y();
    first_handle_rect_pos_value = (orientation_ == Qt::Horizontal) ? firstHandleRect().x() : firstHandleRect().y();
    second_handle_rect_pos_value = (orientation_ == Qt::Horizontal) ? secondHandleRect().x() : secondHandleRect().y();

    if (range_drag_active_) {
      int pixel_delta = pos_value - range_drag_start_pos_;
      int value_delta = static_cast<int>(pixel_delta * 1.0 / validLength() * interval_);
      int new_lower = range_drag_lower_start_ + value_delta;
      int new_upper = range_drag_upper_start_ + value_delta;

      if (new_lower < minimum_) {
        new_upper += (minimum_ - new_lower);
        new_lower = minimum_;
      }
      if (new_upper > maximum_) {
        new_lower -= (new_upper - maximum_);
        new_upper = maximum_;
      }
      new_lower = std::max(new_lower, minimum_);
      new_upper = std::min(new_upper, maximum_);

      setLowerValue(new_lower);
      setUpperValue(new_upper);
    } else if (first_handle_pressed_ && type_.testFlag(kLeftHandle)) {
      if (pos_value - delta_ + kScHandleWidth / 2 <= second_handle_rect_pos_value) {
        setLowerValue(
            (pos_value - delta_ - kScLeftRightMargin - kScHandleWidth / 2) * 1.0 / validLength() * interval_ +
            minimum_);
      } else {
        setLowerValue(upper_value_);
      }
    } else if (second_handle_pressed_ && type_.testFlag(kRightHandle)) {
      if (first_handle_rect_pos_value + kScHandleWidth * (type_.testFlag(kDoubleHandles) ? 1.5 : 0.5) <=
          pos_value - delta_) {
        setUpperValue(
            (pos_value - delta_ - kScLeftRightMargin - kScHandleWidth / 2 -
             (type_.testFlag(kDoubleHandles) ? kScHandleWidth : 0)) *
                1.0 / validLength() * interval_ +
            minimum_);
      } else {
        setUpperValue(lower_value_);
      }
    }
  }

  // Hover tint (timeSlider parity): when not dragging, light up the handle the
  // cursor is over in PJPurple.
  if (!(a_event->buttons() & Qt::LeftButton)) {
    const QPointF p = a_event->position();
    int hovered = 0;
    if (type_.testFlag(kLeftHandle) && firstHandleRect().contains(p)) {
      hovered = 1;
    } else if (type_.testFlag(kRightHandle) && secondHandleRect().contains(p)) {
      hovered = 2;
    }
    hovered_handle_ = hovered;
  }

  update();
  maybeShowHandleTooltip(a_event->globalPosition().toPoint(), a_event->pos());
}

void RangeSlider::mouseReleaseEvent(QMouseEvent* a_event) {
  Q_UNUSED(a_event);

  first_handle_pressed_ = false;
  second_handle_pressed_ = false;
  range_drag_active_ = false;
  update();

  if (show_handle_value_tooltip_) {
    QToolTip::hideText();
    tooltip_visible_ = false;
  }
}

void RangeSlider::changeEvent(QEvent* a_event) {
  // Repaint on enable/disable so the groove/handles switch to/from the muted
  // (disabled) palette.
  if (a_event->type() == QEvent::EnabledChange) {
    update();
  }
}

void RangeSlider::leaveEvent(QEvent* e) {
  QWidget::leaveEvent(e);
  if (hovered_handle_ != 0) {
    hovered_handle_ = 0;
    update();
  }
  QToolTip::hideText();
  tooltip_visible_ = false;
}

QSize RangeSlider::minimumSizeHint() const {
  // Track height (== the playback scrubber) + ONE label row above it for the
  // floating handle labels. No reserve below the track — that was wasted padding.
  int h = kScTrackHeight;
  if (floating_labels_) {
    QFontMetrics fm(font());
    h += fm.height() + 6 + 4;  // label_height + gap (one row, matches trackTop)
  }
  return QSize(kScHandleWidth * 2 + kScLeftRightMargin * 2, h);
}

int RangeSlider::getMinimun() const {
  return minimum_;
}
int RangeSlider::getMaximun() const {
  return maximum_;
}
int RangeSlider::getLowerValue() const {
  return lower_value_;
}
int RangeSlider::getUpperValue() const {
  return upper_value_;
}

void RangeSlider::setLowerValue(int a_lower_value) {
  if (a_lower_value > maximum_) {
    a_lower_value = maximum_;
  }
  if (a_lower_value < minimum_) {
    a_lower_value = minimum_;
  }
  lower_value_ = a_lower_value;
  emit lowerValueChanged(lower_value_);
  update();
}

void RangeSlider::setUpperValue(int a_upper_value) {
  if (a_upper_value > maximum_) {
    a_upper_value = maximum_;
  }
  if (a_upper_value < minimum_) {
    a_upper_value = minimum_;
  }
  upper_value_ = a_upper_value;
  emit upperValueChanged(upper_value_);
  update();
}

void RangeSlider::setMinimum(int a_minimum) {
  if (a_minimum <= maximum_) {
    minimum_ = a_minimum;
  } else {
    int old_max = maximum_;
    minimum_ = old_max;
    maximum_ = a_minimum;
  }
  interval_ = maximum_ - minimum_;
  update();

  setLowerValue(minimum_);
  setUpperValue(maximum_);

  emit rangeChanged(minimum_, maximum_);
}

void RangeSlider::setMaximum(int a_maximum) {
  if (a_maximum >= minimum_) {
    maximum_ = a_maximum;
  } else {
    int old_min = minimum_;
    maximum_ = old_min;
    minimum_ = a_maximum;
  }
  interval_ = maximum_ - minimum_;
  update();

  setLowerValue(minimum_);
  setUpperValue(maximum_);

  emit rangeChanged(minimum_, maximum_);
}

int RangeSlider::validLength() const {
  int len = (orientation_ == Qt::Horizontal) ? width() : height();
  return len - kScLeftRightMargin * 2 - kScHandleWidth * (type_.testFlag(kDoubleHandles) ? 2 : 1);
}

int RangeSlider::trackTop() const {
  if (orientation_ != Qt::Horizontal || !floating_labels_) {
    return static_cast<int>((height() - kScTrackHeight) / 2);  // centered (no label row)
  }
  // One label row above the track (matches minimumSizeHint). No reserve below.
  const QFontMetrics fm(font());
  return fm.height() + 6 + 4;
}

void RangeSlider::setRange(int a_minimum, int a_maximum) {
  setMinimum(a_minimum);
  setMaximum(a_maximum);
}

void RangeSlider::setOptions(Options t) {
  type_ = t;
  update();
}

void RangeSlider::setMarkers(std::vector<Marker> markers) {
  std::sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.start < b.start; });
  markers_ = std::move(markers);
  update();
}

void RangeSlider::drawMarkers(QPainter& painter, const QRectF& background_rect) {
  if (interval_ <= 0) {
    return;
  }
  const int px_len = validLength();
  if (px_len <= 0) {
    return;
  }
  // Same value->x mapping the handles + ticks use.
  const int offset = kScLeftRightMargin + (type_.testFlag(kDoubleHandles) ? kScHandleWidth : 0);
  auto value_to_x = [&](int value) -> int {
    const double pct = static_cast<double>(value - minimum_) / static_cast<double>(interval_);
    return static_cast<int>(pct * static_cast<double>(px_len)) + offset;
  };
  const QFontMetrics fm(painter.font());
  const int top = static_cast<int>(background_rect.top());
  const int height = static_cast<int>(background_rect.bottom()) - top;

  for (const auto& m : markers_) {
    int x0 = value_to_x(m.start);
    int x1 = value_to_x(m.end);
    if (x1 <= x0) {
      x1 = x0 + 1;  // keep a degenerate box visible
    }
    const int box_w = x1 - x0;
    const QRect box(x0, top, box_w, height);

    // Shade the box when its [start, end] overlaps the current [lower, upper]
    // selection — the "which chunk falls in the range" cue. Translucent over the
    // groove so the blue selection fill still reads underneath.
    if (m.start < upper_value_ && m.end > lower_value_) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(kMarkerInRange);
      painter.drawRect(box);
    }

    // Box outline at the chunk's TRUE extent. Disjoint chunks therefore read as
    // separate boxes with blank slider space between them (the gaps).
    painter.setPen(QPen(kMarkerLine, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(box.adjusted(0, 0, -1, -1));

    // Chunk label, centered in the box, only when it fits.
    if (!m.label.isEmpty() && box_w >= fm.horizontalAdvance(m.label) + 4) {
      painter.setPen(kMarkerText);
      painter.drawText(box, Qt::AlignCenter, m.label);
    }
  }
}

void RangeSlider::setShowHandleValueTooltip(bool on) {
  show_handle_value_tooltip_ = on;
  if (!on) {
    QToolTip::hideText();
    tooltip_visible_ = false;
  }
}

bool RangeSlider::showHandleValueTooltip() const {
  return show_handle_value_tooltip_;
}

QString RangeSlider::handleValueText(bool left) const {
  return QString::number(left ? lower_value_ : upper_value_);
}

void RangeSlider::maybeShowHandleTooltip(const QPoint& global_pos, const QPoint& local_pos) {
  if (!show_handle_value_tooltip_) {
    return;
  }
  bool over_left = type_.testFlag(kLeftHandle) && firstHandleRect().contains(local_pos);
  bool over_right = type_.testFlag(kRightHandle) && secondHandleRect().contains(local_pos);
  if (first_handle_pressed_ && type_.testFlag(kLeftHandle)) {
    over_left = true;
  }
  if (second_handle_pressed_ && type_.testFlag(kRightHandle)) {
    over_right = true;
  }
  if (over_left) {
    QToolTip::showText(global_pos, handleValueText(true), this);
    tooltip_visible_ = true;
  } else if (over_right) {
    QToolTip::showText(global_pos, handleValueText(false), this);
    tooltip_visible_ = true;
  } else if (tooltip_visible_) {
    QToolTip::hideText();
    tooltip_visible_ = false;
  }
}

// --- Real-value convenience API (kept for parity with single-value sliders) ---
int RangeSlider::toInt(double v) const {
  return static_cast<int>(v + 0.5);
}
double RangeSlider::toReal(int v) const {
  return static_cast<double>(v);
}
void RangeSlider::setRangeReal(double min_v, double max_v, int /*decimals*/) {
  setMinimum(static_cast<int>(min_v));
  setMaximum(static_cast<int>(max_v));
}
void RangeSlider::setLowerValueReal(double v) {
  setLowerValue(toInt(v));
}
void RangeSlider::setUpperValueReal(double v) {
  setUpperValue(toInt(v));
}
double RangeSlider::lowerValueReal() const {
  return toReal(lower_value_);
}
double RangeSlider::upperValueReal() const {
  return toReal(upper_value_);
}
int RangeSlider::decimals() const {
  return 0;
}

void RangeSlider::setFloatingLabelsVisible(bool on) {
  floating_labels_ = on;
  update();
}
bool RangeSlider::floatingLabelsVisible() const {
  return floating_labels_;
}

void RangeSlider::setLabelFormatter(std::function<QString(double)> formatter) {
  label_formatter_ = std::move(formatter);
  update();
}
void RangeSlider::setCenterLabelFormatter(std::function<QString(double, double)> formatter) {
  center_label_formatter_ = std::move(formatter);
  update();
}

QString RangeSlider::formatHandleValue(double value) const {
  if (label_formatter_) {
    return label_formatter_(value);
  }
  return handleValueText(value == lowerValueReal());
}

void RangeSlider::drawFloatingLabels(QPainter& painter) {
  lower_label_rect_ = QRect();
  upper_label_rect_ = QRect();
  center_label_rect_ = QRect();

  if (orientation_ != Qt::Horizontal) {
    return;
  }

  painter.setRenderHint(QPainter::Antialiasing);
  QFont label_font = font();
  painter.setFont(label_font);
  QFontMetrics fm(label_font);

  const int label_height = fm.height() + 6;
  const int handle_top = trackTop();
  const int label_y = handle_top - label_height - 2;

  auto draw_label = [&](const QRectF& handle_rect, const QString& text) -> QRect {
    if (text.isEmpty()) {
      return QRect();
    }
    int text_width = fm.horizontalAdvance(text) + 8;
    int label_x = static_cast<int>(handle_rect.center().x()) - text_width / 2;
    label_x = std::max(0, std::min(label_x, width() - text_width));
    QRect rect(label_x, label_y, text_width, label_height);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(50, 50, 50, 220));
    painter.drawRoundedRect(rect, 4, 4);
    painter.setPen(Qt::white);
    painter.drawText(rect, Qt::AlignCenter, text);
    return rect;
  };

  if (type_.testFlag(kLeftHandle)) {
    lower_label_rect_ = draw_label(firstHandleRect(), formatHandleValue(static_cast<double>(lower_value_)));
  }
  if (type_.testFlag(kRightHandle)) {
    upper_label_rect_ = draw_label(secondHandleRect(), formatHandleValue(static_cast<double>(upper_value_)));
  }

  // Selected-duration chip sits ON the track, centered between the handles (the
  // start/end labels above float; the duration reads inside the selected fill).
  // Hidden when the handles are too close to fit it.
  if (center_label_formatter_) {
    QString center_text = center_label_formatter_(static_cast<double>(lower_value_), static_cast<double>(upper_value_));
    if (!center_text.isEmpty()) {
      const QRectF left_rect = firstHandleRect();
      const QRectF right_rect = secondHandleRect();
      const double gap_left = left_rect.right();
      const double gap_right = right_rect.left();
      const int text_width = fm.horizontalAdvance(center_text) + 16;
      if (gap_right - gap_left >= text_width + 6) {
        const double cx = (gap_left + gap_right) / 2.0;
        const int chip_h = fm.height() + 4;
        const double track_top = trackTop();
        QRect rect(
            static_cast<int>(cx - text_width / 2.0), static_cast<int>(track_top + (kScTrackHeight - chip_h) / 2.0),
            text_width, chip_h);
        // Bordered duration chip: 1px PJLightBlue outline around the blue fill.
        painter.setPen(QPen(QColor(0xC2, 0xDC, 0xFF), 1));
        painter.setBrush(QColor(30, 80, 160, 230));
        painter.drawRoundedRect(rect, 4, 4);
        painter.setPen(Qt::white);
        painter.drawText(rect, Qt::AlignCenter, center_text);
        center_label_rect_ = rect;
      }
    }
  }
}

}  // namespace PJ
