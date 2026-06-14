#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QAbstractButton>
#include <QColor>

namespace PJ {

// A standard, fixed-size colour swatch: a rounded square painted in the current
// colour that opens a ColorPickerPopup when clicked. The single reusable
// replacement for the per-layer swatch buttons that each module used to hand-roll
// (scene_entities / pointcloud / robot_model / poses).
//
// colorChanged() fires only for USER picks made through the popup; setColor() is
// programmatic and silent, so a host can seed/restore the swatch without feedback.
class ColorPickerWidget : public QAbstractButton {
  Q_OBJECT
 public:
  explicit ColorPickerWidget(QWidget* parent = nullptr);
  ~ColorPickerWidget() override;

  [[nodiscard]] QColor color() const {
    return color_;
  }
  // Programmatic set — updates the swatch, does NOT emit colorChanged().
  void setColor(const QColor& color);

  [[nodiscard]] QSize sizeHint() const override;
  [[nodiscard]] QSize minimumSizeHint() const override;

 signals:
  // Emitted only when the user picks a colour in the popup.
  void colorChanged(QColor color);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  void openPopup();

  QColor color_{Qt::white};
};

}  // namespace PJ
