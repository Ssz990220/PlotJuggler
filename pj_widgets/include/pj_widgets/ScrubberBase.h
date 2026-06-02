#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QPixmap>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QWidget>

class QLineEdit;
class QTimer;
class QVariantAnimation;

namespace PJ {

// Blender-style horizontal numeric scrubber. Drag the body to scrub the
// value (cursor wraps around screen edges so you can scrub forever); click
// without dragging to enter an inline text editor; hover to reveal the
// left/right step arrows. Concrete value type is provided by subclasses
// via stepBy / displayText / commitText.
class ScrubberBase : public QWidget {
  Q_OBJECT
 public:
  explicit ScrubberBase(QWidget* parent = nullptr);
  ~ScrubberBase() override;

  int pixelsPerStep() const {
    return pixels_per_step_;
  }
  void setPixelsPerStep(int px);

  bool cursorHiddenDuringDrag() const {
    return cursor_hidden_during_drag_;
  }
  void setCursorHiddenDuringDrag(bool hidden) {
    cursor_hidden_during_drag_ = hidden;
  }

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 protected:
  // Called by subclass on every drag step and on every arrow click. The
  // sign of `steps_signed` is the direction; magnitude is always 1 in
  // current usage but the API leaves room for batching.
  virtual void stepBy(int steps_signed) = 0;

  // Subclass-formatted text to draw in the centre.
  virtual QString displayText() const = 0;

  // Parse and apply the edited text. Return false if invalid; the widget
  // reverts to the prior value silently.
  virtual bool commitText(const QString& text) = 0;

  // Subclasses call this after setting a new value to repaint.
  void valueRepaint();

  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  bool eventFilter(QObject* obj, QEvent* event) override;

 private:
  enum class State { Idle, Armed, Dragging, Editing };
  enum class Zone { LeftArrow, RightArrow, Body };

  // Hit-rect helpers
  QRect leftArrowRect() const;
  QRect rightArrowRect() const;
  QRect centerRect() const;
  Zone zoneAt(const QPoint& pos) const;
  void updateCursorForPos(const QPoint& pos);

  // State transitions
  void enterEditMode();
  void exitEditMode(bool commit);
  void startDrag();
  void endDrag();

  // Arrow autorepeat
  void startAutoRepeat(int direction);
  void stopAutoRepeat();
  void onAutoRepeatTick();

  // Hover fade
  void setHoverAlphaInternal(qreal alpha);
  void animateHover(bool entering);

  // Drag math
  void handleDragMove(const QPointF& global_pos);
  static QRect screenGeometryAt(const QPointF& global_pos);

  // Lazy editor
  QLineEdit* ensureLineEdit();

  // Config
  int pixels_per_step_ = 5;
  bool cursor_hidden_during_drag_ = true;
  static constexpr int kClickDragThreshold = 4;
  static constexpr int kAutorepeatDelayMs = 400;
  static constexpr int kAutorepeatIntervalMs = 50;
  static constexpr int kArrowZoneWidth = 16;
  static constexpr int kCornerRadiusPx = 4;
  static constexpr int kHoverFadeMs = 120;

  // State
  State state_ = State::Idle;
  bool is_hovered_ = false;
  qreal hover_alpha_ = 0.0;

  // Drag bookkeeping
  QPoint press_screen_pos_;
  QPointF last_drag_global_;
  qreal accumulated_pixels_ = 0.0;
  qreal total_move_ = 0.0;

  QLineEdit* line_edit_ = nullptr;
  QTimer* autorepeat_timer_ = nullptr;
  int autorepeat_direction_ = 0;
  QVariantAnimation* hover_animation_ = nullptr;
};

}  // namespace PJ
