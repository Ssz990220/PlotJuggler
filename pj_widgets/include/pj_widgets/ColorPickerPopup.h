#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QDialog>
#include <QSize>
#include <QWidget>

class QLineEdit;

namespace PJ {

// Horizontal hue strip with a single drag handle. Click/drag emits
// hueChanged; setHue suppresses the signal so the popup can sync state
// without triggering a feedback loop.
class HueSlider : public QWidget {
  Q_OBJECT
 public:
  explicit HueSlider(QWidget* parent = nullptr);

  [[nodiscard]] int hue() const noexcept {
    return hue_;
  }
  void setHue(int hue);

  [[nodiscard]] QSize sizeHint() const override;

 signals:
  void hueChanged(int hue);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;

 private:
  void updateFromMouseX(int x);

  int hue_ = 0;  // [0, 359]
};

// Saturation/value square for the current hue. Background repaints when
// the hue changes; cursor is a hollow white circle at (s*w, (1-v)*h).
class SVSquare : public QWidget {
  Q_OBJECT
 public:
  explicit SVSquare(QWidget* parent = nullptr);

  void setHue(int hue);
  void setSV(qreal s, qreal v);

  [[nodiscard]] QColor color() const;
  [[nodiscard]] QSize sizeHint() const override;

 signals:
  void colorChanged(QColor color);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;

 private:
  void updateFromMouseXY(int x, int y);

  int hue_ = 0;
  qreal s_ = 1.0;
  qreal v_ = 1.0;
};

// HSV color picker popup: HueSlider + SVSquare + hex line edit, stacked.
// Inherits QDialog so the application-wide dialog stylesheet applies (a
// plain QWidget would be transparent under the global theme). Constructed
// with Qt::Popup so a click outside or Escape closes it. Emits colorChanged
// live during drag.
class ColorPickerPopup : public QDialog {
  Q_OBJECT
 public:
  explicit ColorPickerPopup(QWidget* parent = nullptr);

  void setColor(const QColor& color);
  [[nodiscard]] QColor color() const;

 signals:
  void colorChanged(QColor color);

 private:
  void onHueChanged(int hue);
  void onSquareChanged(QColor color);
  void onHexCommitted();
  void recomputeAndEmit();
  void pushToHexField(const QColor& color);

  HueSlider* hue_slider_ = nullptr;
  SVSquare* sv_square_ = nullptr;
  QLineEdit* hex_edit_ = nullptr;

  int hue_ = 0;
  qreal s_ = 1.0;
  qreal v_ = 1.0;
};

}  // namespace PJ
