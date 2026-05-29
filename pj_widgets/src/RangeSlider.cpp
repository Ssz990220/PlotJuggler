// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Two-handle range slider implementation, wrapped in namespace PJ.
// See RangeSlider.h.

#include <pj_widgets/RangeSlider.h>

#include <QDebug>
#include <limits>

namespace PJ {

namespace {

// Geometry + colors mirror the app's playback slider (QSlider#timeSlider) so the
// range slider reads as the same control: a full-height rectangular track with a
// thin vertical handle. Those QSS color tokens are theme-independent
// (PJLightBlue/PJLightPurple/PJPurple are identical in light + dark, and
// border_default #B0B0BF vs #c0c0c0 is imperceptible), so hardcoding them here
// matches the playback slider on both themes without reading the theme.
const int scHandleWidth = 8;   // timeSlider handle: 6px content + 1px border each side = 8px rendered
const int scTrackHeight = 24;  // timeSlider groove + handle height (px)
const int scLeftRightMargin = 1;

const QColor kGrooveBorder(0xB0, 0xB0, 0xBF);  // border_default
const QColor kSelection(0xC2, 0xDC, 0xFF);     // PJLightBlue (selected-range fill)
const QColor kHandle(0xFF, 0xAE, 0xFF);        // PJLightPurple (resting handle)
const QColor kHandleActive(0xCC, 0x00, 0xCC);  // PJPurple (hovered / pressed handle)
const QColor kHandleBorder(0xCC, 0x00, 0xCC);  // PJPurple (handle border)
const QColor kDisabledInk(0x80, 0x80, 0x80);   // muted grey when the slider is disabled

}  // namespace

RangeSlider::RangeSlider(QWidget* aParent) : QWidget(aParent) {
  setMouseTracking(true);
}

RangeSlider::RangeSlider(Qt::Orientation ori, Options t, QWidget* aParent)
    : QWidget(aParent), orientation(ori), type(t) {
  setMouseTracking(true);
}

void RangeSlider::paintEvent(QPaintEvent* aEvent) {
  Q_UNUSED(aEvent);
  QPainter painter(this);

  // Groove geometry: a full-height rectangular track (timeSlider shape).
  QRectF backgroundRect;
  if (orientation == Qt::Horizontal) {
    backgroundRect =
        QRectF(scLeftRightMargin, (height() - scTrackHeight) / 2.0, width() - scLeftRightMargin * 2, scTrackHeight);
  } else {
    backgroundRect =
        QRectF((width() - scTrackHeight) / 2.0, scLeftRightMargin, scTrackHeight, height() - scLeftRightMargin * 2);
  }

  const bool enabled = isEnabled();
  const QRectF leftHandleRect = firstHandleRect();
  const QRectF rightHandleRect = secondHandleRect();
  painter.setRenderHint(QPainter::Antialiasing, false);

  // 1. Selected-range fill (between the two handles) — PJLightBlue, like the
  //    playback slider's played sub-page. Drawn first; the groove border is
  //    stroked on top so it always reads crisply.
  QRectF selectedRect(backgroundRect);
  if (orientation == Qt::Horizontal) {
    selectedRect.setLeft(type.testFlag(LeftHandle) ? leftHandleRect.right() : leftHandleRect.left());
    selectedRect.setRight(type.testFlag(RightHandle) ? rightHandleRect.left() : rightHandleRect.right());
  } else {
    selectedRect.setTop(type.testFlag(LeftHandle) ? leftHandleRect.bottom() : leftHandleRect.top());
    selectedRect.setBottom(type.testFlag(RightHandle) ? rightHandleRect.top() : rightHandleRect.bottom());
  }
  painter.setPen(Qt::NoPen);
  painter.setBrush(enabled ? kSelection : kDisabledInk);
  painter.drawRect(selectedRect);

  // 2. Groove outline — transparent body + 1px border (timeSlider groove:
  //    widget_background is transparent, border = border_default, square corners).
  painter.setPen(QPen(kGrooveBorder, 1));
  painter.setBrush(Qt::NoBrush);
  painter.drawRect(backgroundRect.adjusted(0.5, 0.5, -0.5, -0.5));

  if (mShowTicks) {
    drawTicks(painter, backgroundRect);
  }

  // 3. Handles — thin full-height grips (timeSlider handle shape): PJLightPurple
  //    at rest, PJPurple when hovered or pressed, with a PJPurple border.
  auto paintHandle = [&](const QRectF& r, bool active) {
    painter.setPen(QPen(enabled ? kHandleBorder : kDisabledInk, 1));
    painter.setBrush(!enabled ? kDisabledInk.lighter(125) : (active ? kHandleActive : kHandle));
    painter.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
  };
  if (type.testFlag(LeftHandle)) {
    paintHandle(leftHandleRect, mFirstHandlePressed || mHoveredHandle == 1);
  }
  if (type.testFlag(RightHandle)) {
    paintHandle(rightHandleRect, mSecondHandlePressed || mHoveredHandle == 2);
  }

  if (mFloatingLabels) {
    drawFloatingLabels(painter);
  }
}

QRectF RangeSlider::firstHandleRect() const {
  float percentage = (mLowerValue - mMinimum) * 1.0 / mInterval;
  return handleRect(percentage * validLength() + scLeftRightMargin);
}

QRectF RangeSlider::secondHandleRect() const {
  float percentage = (mUpperValue - mMinimum) * 1.0 / mInterval;
  return handleRect(percentage * validLength() + scLeftRightMargin + (type.testFlag(LeftHandle) ? scHandleWidth : 0));
}

QRectF RangeSlider::handleRect(int aValue) const {
  // Thin grip spanning the full track height (timeSlider handle: 6px wide,
  // groove-tall), centered across the short axis.
  if (orientation == Qt::Horizontal) {
    return QRect(aValue, (height() - scTrackHeight) / 2, scHandleWidth, scTrackHeight);
  } else {
    return QRect((width() - scTrackHeight) / 2, aValue, scTrackHeight, scHandleWidth);
  }
}

void RangeSlider::mousePressEvent(QMouseEvent* aEvent) {
  if (aEvent->buttons() & Qt::LeftButton) {
    int posCheck, posMax, posValue, firstHandleRectPosValue, secondHandleRectPosValue;
    posCheck = (orientation == Qt::Horizontal) ? aEvent->pos().y() : aEvent->pos().x();
    posMax = (orientation == Qt::Horizontal) ? height() : width();
    posValue = (orientation == Qt::Horizontal) ? aEvent->pos().x() : aEvent->pos().y();
    firstHandleRectPosValue = (orientation == Qt::Horizontal) ? firstHandleRect().x() : firstHandleRect().y();
    secondHandleRectPosValue = (orientation == Qt::Horizontal) ? secondHandleRect().x() : secondHandleRect().y();

    // Floating labels double as hit-test targets.
    const bool on_lower_label = mFloatingLabels && !mLowerLabelRect.isNull() && mLowerLabelRect.contains(aEvent->pos());
    const bool on_upper_label = mFloatingLabels && !mUpperLabelRect.isNull() && mUpperLabelRect.contains(aEvent->pos());
    const bool on_center_label =
        mFloatingLabels && !mCenterLabelRect.isNull() && mCenterLabelRect.contains(aEvent->pos());

    mSecondHandlePressed =
        on_upper_label || (!on_lower_label && !on_center_label && secondHandleRect().contains(aEvent->pos()));
    mFirstHandlePressed =
        on_lower_label || (!mSecondHandlePressed && !on_center_label && firstHandleRect().contains(aEvent->pos()));
    mRangeDragActive = false;

    if (mFirstHandlePressed) {
      mDelta = posValue - (firstHandleRectPosValue + scHandleWidth / 2);
    } else if (mSecondHandlePressed) {
      mDelta = posValue - (secondHandleRectPosValue + scHandleWidth / 2);
    } else if (on_center_label && type.testFlag(DoubleHandles)) {
      mRangeDragActive = true;
      mRangeDragStartPos = posValue;
      mRangeDragLowerStart = mLowerValue;
      mRangeDragUpperStart = mUpperValue;
    } else if (
        type.testFlag(DoubleHandles) && posValue > firstHandleRectPosValue + scHandleWidth &&
        posValue < secondHandleRectPosValue && posCheck >= 2 && posCheck <= posMax - 2) {
      mRangeDragActive = true;
      mRangeDragStartPos = posValue;
      mRangeDragLowerStart = mLowerValue;
      mRangeDragUpperStart = mUpperValue;
    } else if (posCheck >= 2 && posCheck <= posMax - 2) {
      int step = mInterval / 10 < 1 ? 1 : mInterval / 10;
      if (posValue < firstHandleRectPosValue) {
        setLowerValue(mLowerValue - step);
      } else if (posValue > secondHandleRectPosValue + scHandleWidth) {
        setUpperValue(mUpperValue + step);
      }
    }
  }

  maybeShowHandleTooltip(aEvent->globalPosition().toPoint(), aEvent->pos());
}

void RangeSlider::mouseMoveEvent(QMouseEvent* aEvent) {
  if (aEvent->buttons() & Qt::LeftButton) {
    int posValue, firstHandleRectPosValue, secondHandleRectPosValue;
    posValue = (orientation == Qt::Horizontal) ? aEvent->pos().x() : aEvent->pos().y();
    firstHandleRectPosValue = (orientation == Qt::Horizontal) ? firstHandleRect().x() : firstHandleRect().y();
    secondHandleRectPosValue = (orientation == Qt::Horizontal) ? secondHandleRect().x() : secondHandleRect().y();

    if (mRangeDragActive) {
      int pixelDelta = posValue - mRangeDragStartPos;
      int valueDelta = static_cast<int>(pixelDelta * 1.0 / validLength() * mInterval);
      int newLower = mRangeDragLowerStart + valueDelta;
      int newUpper = mRangeDragUpperStart + valueDelta;

      if (newLower < mMinimum) {
        newUpper += (mMinimum - newLower);
        newLower = mMinimum;
      }
      if (newUpper > mMaximum) {
        newLower -= (newUpper - mMaximum);
        newUpper = mMaximum;
      }
      newLower = std::max(newLower, mMinimum);
      newUpper = std::min(newUpper, mMaximum);

      setLowerValue(newLower);
      setUpperValue(newUpper);
    } else if (mFirstHandlePressed && type.testFlag(LeftHandle)) {
      if (posValue - mDelta + scHandleWidth / 2 <= secondHandleRectPosValue) {
        setLowerValue(
            (posValue - mDelta - scLeftRightMargin - scHandleWidth / 2) * 1.0 / validLength() * mInterval + mMinimum);
      } else {
        setLowerValue(mUpperValue);
      }
    } else if (mSecondHandlePressed && type.testFlag(RightHandle)) {
      if (firstHandleRectPosValue + scHandleWidth * (type.testFlag(DoubleHandles) ? 1.5 : 0.5) <= posValue - mDelta) {
        setUpperValue(
            (posValue - mDelta - scLeftRightMargin - scHandleWidth / 2 -
             (type.testFlag(DoubleHandles) ? scHandleWidth : 0)) *
                1.0 / validLength() * mInterval +
            mMinimum);
      } else {
        setUpperValue(mLowerValue);
      }
    }
  }

  // Hover tint (timeSlider parity): when not dragging, light up the handle the
  // cursor is over in PJPurple.
  if (!(aEvent->buttons() & Qt::LeftButton)) {
    const QPointF p = aEvent->position();
    int hovered = 0;
    if (type.testFlag(LeftHandle) && firstHandleRect().contains(p)) {
      hovered = 1;
    } else if (type.testFlag(RightHandle) && secondHandleRect().contains(p)) {
      hovered = 2;
    }
    mHoveredHandle = hovered;
  }

  update();
  maybeShowHandleTooltip(aEvent->globalPosition().toPoint(), aEvent->pos());
}

void RangeSlider::mouseReleaseEvent(QMouseEvent* aEvent) {
  Q_UNUSED(aEvent);

  mFirstHandlePressed = false;
  mSecondHandlePressed = false;
  mRangeDragActive = false;
  update();

  if (mShowHandleValueTooltip) {
    QToolTip::hideText();
    mTooltipVisible = false;
  }
}

void RangeSlider::changeEvent(QEvent* aEvent) {
  // Repaint on enable/disable so the groove/handles switch to/from the muted
  // (disabled) palette.
  if (aEvent->type() == QEvent::EnabledChange) {
    update();
  }
}

void RangeSlider::leaveEvent(QEvent* e) {
  QWidget::leaveEvent(e);
  if (mHoveredHandle != 0) {
    mHoveredHandle = 0;
    update();
  }
  QToolTip::hideText();
  mTooltipVisible = false;
}

QSize RangeSlider::minimumSizeHint() const {
  int h = scTrackHeight;
  if (mFloatingLabels) {
    QFontMetrics fm(font());
    int label_row = fm.height() + 6 + 4;  // label_height + gap
    h += label_row * 2;
  }
  return QSize(scHandleWidth * 2 + scLeftRightMargin * 2, h);
}

int RangeSlider::GetMinimun() const {
  return mMinimum;
}
void RangeSlider::SetMinimum(int aMinimum) {
  setMinimum(aMinimum);
}
int RangeSlider::GetMaximun() const {
  return mMaximum;
}
void RangeSlider::SetMaximum(int aMaximum) {
  setMaximum(aMaximum);
}
int RangeSlider::GetLowerValue() const {
  return mLowerValue;
}
void RangeSlider::SetLowerValue(int aLowerValue) {
  setLowerValue(aLowerValue);
}
int RangeSlider::GetUpperValue() const {
  return mUpperValue;
}
void RangeSlider::SetUpperValue(int aUpperValue) {
  setUpperValue(aUpperValue);
}

void RangeSlider::setLowerValue(int aLowerValue) {
  if (aLowerValue > mMaximum) {
    aLowerValue = mMaximum;
  }
  if (aLowerValue < mMinimum) {
    aLowerValue = mMinimum;
  }
  mLowerValue = aLowerValue;
  emit lowerValueChanged(mLowerValue);
  update();
}

void RangeSlider::setUpperValue(int aUpperValue) {
  if (aUpperValue > mMaximum) {
    aUpperValue = mMaximum;
  }
  if (aUpperValue < mMinimum) {
    aUpperValue = mMinimum;
  }
  mUpperValue = aUpperValue;
  emit upperValueChanged(mUpperValue);
  update();
}

void RangeSlider::setMinimum(int aMinimum) {
  if (aMinimum <= mMaximum) {
    mMinimum = aMinimum;
  } else {
    int oldMax = mMaximum;
    mMinimum = oldMax;
    mMaximum = aMinimum;
  }
  mInterval = mMaximum - mMinimum;
  update();

  setLowerValue(mMinimum);
  setUpperValue(mMaximum);

  emit rangeChanged(mMinimum, mMaximum);
}

void RangeSlider::setMaximum(int aMaximum) {
  if (aMaximum >= mMinimum) {
    mMaximum = aMaximum;
  } else {
    int oldMin = mMinimum;
    mMaximum = oldMin;
    mMinimum = aMaximum;
  }
  mInterval = mMaximum - mMinimum;
  update();

  setLowerValue(mMinimum);
  setUpperValue(mMaximum);

  emit rangeChanged(mMinimum, mMaximum);
}

int RangeSlider::validLength() const {
  int len = (orientation == Qt::Horizontal) ? width() : height();
  return len - scLeftRightMargin * 2 - scHandleWidth * (type.testFlag(DoubleHandles) ? 2 : 1);
}

void RangeSlider::SetRange(int aMinimum, int aMaximum) {
  setMinimum(aMinimum);
  setMaximum(aMaximum);
}

void RangeSlider::setOptions(Options t) {
  type = t;
  update();
}

void RangeSlider::setMinTickPixelSpacing(int px) {
  mMinTickPx = px;
  update();
}
void RangeSlider::setShowTickLabels(bool on) {
  mShowTickLabels = on;
  update();
}
void RangeSlider::setShowTicks(bool on) {
  mShowTicks = on;
  update();
}
bool RangeSlider::showTicks() const {
  return mShowTicks;
}

int RangeSlider::niceStep(int raw) const {
  if (raw <= 1) {
    return 1;
  }
  int p = 1;
  while (p * 10 <= raw) {
    p *= 10;
  }
  int d = raw / p;
  int step = 1;
  if (d <= 1) {
    step = 1;
  } else if (d <= 2) {
    step = 2;
  } else if (d <= 5) {
    step = 5;
  } else {
    step = 10;
  }
  return step * p;
}

int RangeSlider::firstTick(int min, int step) const {
  if (step <= 0) {
    return min;
  }
  int r = min % step;
  return (r == 0) ? min : (min + (step - r));
}

void RangeSlider::drawTicks(QPainter& painter, const QRectF& backgroundRect) {
  if (mInterval <= 0) {
    return;
  }
  int pxLen = validLength();
  if (pxLen <= 0) {
    return;
  }
  int approxCount = std::max(2, pxLen / std::max(10, mMinTickPx));
  int idealStep = std::max(1, (mMaximum - mMinimum) / approxCount);
  int step = niceStep(idealStep);
  int start = firstTick(mMinimum, step);

  QFontMetrics fm(painter.font());
  int majorLen = 10;
  int minorLen = 6;
  int minorStep = step / 2;
  if (minorStep < 1) {
    minorStep = 1;
  }

  auto valueToPos = [&](int value) {
    const float percentage = (value - mMinimum) * 1.0f / mInterval;
    const int offset = scLeftRightMargin + (type.testFlag(DoubleHandles) ? scHandleWidth : 0);
    const int base = static_cast<int>(percentage * pxLen) + offset;
    return base;
  };

  painter.setPen(Qt::gray);

  for (int v = start; v <= mMaximum; v += minorStep) {
    bool major = ((v - start) % step) == 0;
    int pos = valueToPos(v);

    if (orientation == Qt::Horizontal) {
      int y = backgroundRect.bottom();
      painter.drawLine(pos, y, pos, y + (major ? majorLen : minorLen));
      if (major && mShowTickLabels) {
        QString txt = QString::number(v);
        int w = fm.horizontalAdvance(txt);
        painter.drawText(pos - w / 2, y + majorLen + fm.ascent() + 2, txt);
      }
    } else {
      int x = backgroundRect.right();
      painter.drawLine(x, pos, x + (major ? majorLen : minorLen), pos);
      if (major && mShowTickLabels) {
        QString txt = QString::number(v);
        painter.drawText(x + majorLen + 4, pos + fm.ascent() / 2, txt);
      }
    }
  }
}

void RangeSlider::setShowHandleValueTooltip(bool on) {
  mShowHandleValueTooltip = on;
  if (!on) {
    QToolTip::hideText();
    mTooltipVisible = false;
  }
}

bool RangeSlider::showHandleValueTooltip() const {
  return mShowHandleValueTooltip;
}

QString RangeSlider::handleValueText(bool left) const {
  return QString::number(left ? mLowerValue : mUpperValue);
}

void RangeSlider::maybeShowHandleTooltip(const QPoint& globalPos, const QPoint& localPos) {
  if (!mShowHandleValueTooltip) {
    return;
  }
  bool overLeft = type.testFlag(LeftHandle) && firstHandleRect().contains(localPos);
  bool overRight = type.testFlag(RightHandle) && secondHandleRect().contains(localPos);
  if (mFirstHandlePressed && type.testFlag(LeftHandle)) {
    overLeft = true;
  }
  if (mSecondHandlePressed && type.testFlag(RightHandle)) {
    overRight = true;
  }
  if (overLeft) {
    QToolTip::showText(globalPos, handleValueText(true), this);
    mTooltipVisible = true;
  } else if (overRight) {
    QToolTip::showText(globalPos, handleValueText(false), this);
    mTooltipVisible = true;
  } else if (mTooltipVisible) {
    QToolTip::hideText();
    mTooltipVisible = false;
  }
}

// --- Real-value convenience API (kept for parity with single-value sliders) ---
int RangeSlider::toInt(double v) const {
  return static_cast<int>(v + 0.5);
}
double RangeSlider::toReal(int v) const {
  return static_cast<double>(v);
}
void RangeSlider::setRangeReal(double minV, double maxV, int /*decimals*/) {
  setMinimum(static_cast<int>(minV));
  setMaximum(static_cast<int>(maxV));
}
void RangeSlider::setLowerValueReal(double v) {
  setLowerValue(toInt(v));
}
void RangeSlider::setUpperValueReal(double v) {
  setUpperValue(toInt(v));
}
double RangeSlider::lowerValueReal() const {
  return toReal(mLowerValue);
}
double RangeSlider::upperValueReal() const {
  return toReal(mUpperValue);
}
int RangeSlider::decimals() const {
  return 0;
}

void RangeSlider::setFloatingLabelsVisible(bool on) {
  mFloatingLabels = on;
  update();
}
bool RangeSlider::floatingLabelsVisible() const {
  return mFloatingLabels;
}

void RangeSlider::setLabelFormatter(std::function<QString(double)> formatter) {
  mLabelFormatter = std::move(formatter);
  update();
}
void RangeSlider::setCenterLabelFormatter(std::function<QString(double, double)> formatter) {
  mCenterLabelFormatter = std::move(formatter);
  update();
}

QString RangeSlider::formatHandleValue(double value) const {
  if (mLabelFormatter) {
    return mLabelFormatter(value);
  }
  return handleValueText(value == lowerValueReal());
}

void RangeSlider::drawFloatingLabels(QPainter& painter) {
  mLowerLabelRect = QRect();
  mUpperLabelRect = QRect();
  mCenterLabelRect = QRect();

  if (orientation != Qt::Horizontal) {
    return;
  }

  painter.setRenderHint(QPainter::Antialiasing);
  QFont label_font = font();
  painter.setFont(label_font);
  QFontMetrics fm(label_font);

  const int label_height = fm.height() + 6;
  const int handle_top = (height() - scTrackHeight) / 2;
  const int label_y = handle_top - label_height - 2;

  auto drawLabel = [&](const QRectF& handle_rect, const QString& text) -> QRect {
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

  if (type.testFlag(LeftHandle)) {
    mLowerLabelRect = drawLabel(firstHandleRect(), formatHandleValue(static_cast<double>(mLowerValue)));
  }
  if (type.testFlag(RightHandle)) {
    mUpperLabelRect = drawLabel(secondHandleRect(), formatHandleValue(static_cast<double>(mUpperValue)));
  }

  if (mCenterLabelFormatter) {
    QString center_text = mCenterLabelFormatter(static_cast<double>(mLowerValue), static_cast<double>(mUpperValue));
    if (!center_text.isEmpty()) {
      QRectF left_rect = firstHandleRect();
      QRectF right_rect = secondHandleRect();
      double center_x = (left_rect.center().x() + right_rect.center().x()) / 2.0;
      int text_width = fm.horizontalAdvance(center_text) + 8;
      int cx = static_cast<int>(center_x) - text_width / 2;
      cx = std::max(0, std::min(cx, width() - text_width));
      const int handle_bottom = (height() + scTrackHeight) / 2;
      int center_label_y = handle_bottom + 2;
      QRect rect(cx, center_label_y, text_width, label_height);
      // Bordered duration chip: 1px PJLightBlue outline around the blue fill.
      painter.setPen(QPen(QColor(0xC2, 0xDC, 0xFF), 1));
      painter.setBrush(QColor(30, 80, 160, 220));
      painter.drawRoundedRect(rect, 4, 4);
      painter.setPen(Qt::white);
      painter.drawText(rect, Qt::AlignCenter, center_text);
      mCenterLabelRect = rect;
    }
  }
}

}  // namespace PJ
