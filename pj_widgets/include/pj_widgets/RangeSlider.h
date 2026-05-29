#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Two-handle range slider with floating value/duration labels, so plugin .ui
// files (loaded by the host's custom QUiLoader) can declare a real range
// slider by class name "RangeSlider". Data + the rangeChanged event flow
// through the WidgetData protocol; see widget_binding.cpp.

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWidget>
#include <functional>

namespace PJ {

class RangeSlider : public QWidget {
  Q_OBJECT
  Q_ENUMS(RangeSliderTypes)

 public:
  enum Option { NoHandle = 0x0, LeftHandle = 0x1, RightHandle = 0x2, DoubleHandles = LeftHandle | RightHandle };
  Q_DECLARE_FLAGS(Options, Option)

  explicit RangeSlider(QWidget* parent = nullptr);
  RangeSlider(Qt::Orientation ori, Options t = DoubleHandles, QWidget* parent = nullptr);

  QSize minimumSizeHint() const override;

  int GetMinimun() const;
  void SetMinimum(int aMinimum);
  int GetMaximun() const;
  void SetMaximum(int aMaximum);
  int GetLowerValue() const;
  void SetLowerValue(int aLowerValue);
  int GetUpperValue() const;
  void SetUpperValue(int aUpperValue);
  void SetRange(int aMinimum, int aMaximum);

  void setOptions(Options t);
  void setMinTickPixelSpacing(int px);
  void setShowTickLabels(bool on);
  void setShowTicks(bool on);
  void setShowHandleValueTooltip(bool on);
  bool showHandleValueTooltip() const;

  // Floating labels — painted above handles during drag.
  void setFloatingLabelsVisible(bool on);
  bool floatingLabelsVisible() const;

  // Custom formatters for floating labels (default: decimal number).
  void setLabelFormatter(std::function<QString(double)> formatter);
  void setCenterLabelFormatter(std::function<QString(double, double)> formatter);

  bool showTicks() const;

  void setRangeReal(double minV, double maxV, int decimals);
  void setLowerValueReal(double v);
  void setUpperValueReal(double v);
  double lowerValueReal() const;
  double upperValueReal() const;
  int decimals() const;
  int toInt(double v) const;
  double toReal(int v) const;

 protected:
  void paintEvent(QPaintEvent* aEvent) override;
  void mousePressEvent(QMouseEvent* aEvent) override;
  void mouseMoveEvent(QMouseEvent* aEvent) override;
  void mouseReleaseEvent(QMouseEvent* aEvent) override;
  void changeEvent(QEvent* aEvent) override;
  void leaveEvent(QEvent* e) override;

  QRectF firstHandleRect() const;
  QRectF secondHandleRect() const;
  QRectF handleRect(int aValue) const;

 signals:
  void lowerValueChanged(int aLowerValue);
  void upperValueChanged(int aUpperValue);
  void rangeChanged(int aMin, int aMax);

 public slots:
  void setLowerValue(int aLowerValue);
  void setUpperValue(int aUpperValue);
  void setMinimum(int aMinimum);
  void setMaximum(int aMaximum);

 private:
  Q_DISABLE_COPY(RangeSlider)
  int validLength() const;

  int mMinimum = 0;
  int mMaximum = 100;
  int mLowerValue = 0;
  int mUpperValue = 100;
  bool mFirstHandlePressed = false;
  bool mSecondHandlePressed = false;
  bool mRangeDragActive = false;
  int mHoveredHandle = 0;  // 0 none, 1 first, 2 second — drives the PJPurple hover tint (timeSlider parity)
  int mRangeDragStartPos = 0;
  int mRangeDragLowerStart = 0;
  int mRangeDragUpperStart = 0;
  int mInterval = 100;
  int mDelta = 0;
  Qt::Orientation orientation = Qt::Horizontal;
  Options type = DoubleHandles;

  int mMinTickPx = 45;
  bool mShowTicks = true;
  bool mShowTickLabels = true;

  void drawTicks(QPainter& painter, const QRectF& backgroundRect);
  int niceStep(int raw) const;
  int firstTick(int min, int step) const;

  bool mShowHandleValueTooltip = true;
  bool mTooltipVisible = false;

  bool mFloatingLabels = false;
  std::function<QString(double)> mLabelFormatter;
  std::function<QString(double, double)> mCenterLabelFormatter;

  QRect mLowerLabelRect;
  QRect mUpperLabelRect;
  QRect mCenterLabelRect;

  void drawFloatingLabels(QPainter& painter);
  QString formatHandleValue(double value) const;

  void maybeShowHandleTooltip(const QPoint& globalPos, const QPoint& localPos);
  QString handleValueText(bool left) const;
};

}  // namespace PJ

Q_DECLARE_OPERATORS_FOR_FLAGS(PJ::RangeSlider::Options)
