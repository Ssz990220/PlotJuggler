// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QColor>
#include <QString>
#include <QWidget>
#include <array>

class QVariantAnimation;

namespace PJ {

// Two-segment horizontal "segmented control" — a joined pair of pill halves
// that replaces a pair of QRadioButtons in config panels. Clicking a half
// selects it; the selected half renders as a raised neutral chip while the
// other half shows the input background.
//
// Usage:
//   auto* w = new DualOptionsWidget("Frame", "Arrow");
//   w->setSelectedIndex(0);                           // 0 = left, 1 = right
//   connect(w, &DualOptionsWidget::selectionChanged, [](int i){ ... });
//
// Colors come from the QSS via Qt stylesheet qproperties:
//   PJ--DualOptionsWidget {
//     qproperty-accentColor:       ${border_checked};
//     qproperty-borderColor:       ${border_default};
//     qproperty-baseFillColor:     ${input_background};
//     qproperty-selectedFillColor: ${item_selection_background};
//     qproperty-textColor:         ${default_text};
//   }
class DualOptionsWidget : public QWidget {
  Q_OBJECT
  Q_PROPERTY(int selectedIndex READ selectedIndex WRITE setSelectedIndex NOTIFY selectionChanged)
  Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor)
  Q_PROPERTY(QColor borderColor READ borderColor WRITE setBorderColor)
  Q_PROPERTY(QColor baseFillColor READ baseFillColor WRITE setBaseFillColor)
  Q_PROPERTY(QColor selectedFillColor READ selectedFillColor WRITE setSelectedFillColor)
  Q_PROPERTY(QColor textColor READ textColor WRITE setTextColor)

 public:
  explicit DualOptionsWidget(QWidget* parent = nullptr);
  explicit DualOptionsWidget(const QString& opt0, const QString& opt1, QWidget* parent = nullptr);

  // Replace both labels at once; triggers a geometry update.
  void setOptions(const QString& opt0, const QString& opt1);

  [[nodiscard]] int selectedIndex() const {
    return selected_;
  }
  [[nodiscard]] bool isFirstSelected() const {
    return selected_ == 0;
  }
  [[nodiscard]] bool isSecondSelected() const {
    return selected_ == 1;
  }

  [[nodiscard]] QColor accentColor() const {
    return accent_color_;
  }
  void setAccentColor(const QColor& color);

  [[nodiscard]] QColor borderColor() const {
    return border_color_;
  }
  void setBorderColor(const QColor& color);

  [[nodiscard]] QColor baseFillColor() const {
    return base_fill_color_;
  }
  void setBaseFillColor(const QColor& color);

  [[nodiscard]] QColor selectedFillColor() const {
    return selected_fill_color_;
  }
  void setSelectedFillColor(const QColor& color);

  [[nodiscard]] QColor textColor() const {
    return text_color_;
  }
  void setTextColor(const QColor& color);

  [[nodiscard]] QSize sizeHint() const override;
  [[nodiscard]] QSize minimumSizeHint() const override;

 public slots:
  // Selects segment 0 (left) or 1 (right). No-op when already selected.
  // Emits selectionChanged when the value actually changes.
  void setSelectedIndex(int index);

 signals:
  void selectionChanged(int index);

 protected:
  bool event(QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  void animateSelectedIndex(int index);

  std::array<QString, 2> options_{"Option A", "Option B"};
  int selected_ = 0;
  qreal visual_selection_ = 0.0;
  QVariantAnimation* selection_animation_ = nullptr;
  QColor accent_color_;
  QColor border_color_;
  QColor base_fill_color_;
  QColor selected_fill_color_;
  QColor text_color_;
};

}  // namespace PJ
