#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QIcon>
#include <QPointer>
#include <QPropertyAnimation>
#include <QWidget>

namespace PJ {

// iOS-style pill toggle. A circular thumb slides between two
// end positions inside a rounded track; clicking anywhere on the
// widget (or Space / Enter while focused) flips the state.
//
// The two "slots" — the halves of the track on either side of the
// thumb — are paintable. Default implementation draws the optional
// leftIcon() / rightIcon() centered in each slot, which covers the
// common case (sun/moon, off/on glyphs, etc.). Subclass and override
// paintLeftSlot / paintRightSlot for custom backgrounds (text,
// gradients, multi-element compositions).
//
// Geometry follows the widget's current size: the thumb diameter
// is `height - 2*kThumbMargin`, slots are width/2 each. Default
// sizeHint() is 56x32 — change with setFixedSize / setMinimumSize
// if you need bigger.
class ToggleSwitch : public QWidget {
  Q_OBJECT
  Q_PROPERTY(bool checked READ isChecked WRITE setChecked NOTIFY toggled)
  Q_PROPERTY(qreal thumbPosition READ thumbPosition WRITE setThumbPosition)

 public:
  explicit ToggleSwitch(QWidget* parent = nullptr);
  ~ToggleSwitch() override;

  bool isChecked() const {
    return checked_;
  }

  QIcon leftIcon() const {
    return left_icon_;
  }
  void setLeftIcon(const QIcon& icon);

  QIcon rightIcon() const {
    return right_icon_;
  }
  void setRightIcon(const QIcon& icon);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 public slots:
  // Animated by default; pass `animate = false` for programmatic
  // initialization (e.g. restoring state from settings) so the
  // thumb snaps to its endpoint without the 180ms slide and
  // without emitting `toggled`.
  void setChecked(bool checked, bool animate);
  void setChecked(bool checked) {
    setChecked(checked, true);
  }
  void toggle();

 signals:
  void toggled(bool checked);
  void clicked();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void changeEvent(QEvent* event) override;

  // Slot rects sit inside the thumb margin, sized to one half of
  // the track; they are static (independent of thumb position).
  // `thumb_position` is 0.0 with the thumb fully left, 1.0 fully
  // right — animates smoothly through intermediate values.
  //
  // Default behaviour: paints the configured QIcon for the slot,
  // faded so only the icon OPPOSITE the thumb shows. The thumb
  // covers one slot at each end position; the other side shows
  // its icon at full opacity, and during animation the two icons
  // cross-fade. Override to draw arbitrary backgrounds (text,
  // gradients, multi-element compositions, opacity-independent
  // content, etc.).
  virtual void paintLeftSlot(QPainter& painter, const QRect& slot_rect, qreal thumb_position);
  virtual void paintRightSlot(QPainter& painter, const QRect& slot_rect, qreal thumb_position);

 private:
  qreal thumbPosition() const {
    return thumb_position_;
  }
  void setThumbPosition(qreal pos);

  QRect thumbRect() const;
  QRect leftSlotRect() const;
  QRect rightSlotRect() const;

  bool checked_ = false;
  qreal thumb_position_ = 0.0;  // 0.0 = thumb on left, 1.0 = thumb on right
  QIcon left_icon_;
  QIcon right_icon_;
  QPointer<QPropertyAnimation> anim_;
};

}  // namespace PJ
