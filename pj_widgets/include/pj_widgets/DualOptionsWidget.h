// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QColor>
#include <QString>
#include <QStringList>
#include <QWidget>

class QVariantAnimation;

namespace PJ {

// Horizontal "segmented control" — a joined row of pill segments that
// replaces an exclusive group of QRadioButtons in config panels. Clicking a
// segment selects it; the selected segment renders as a raised neutral chip
// while the others show the input background. Despite the (historical) name
// it supports any number of segments >= 2; the two-option constructors remain
// as the common-case convenience.
//
// Usage:
//   auto* w = new DualOptionsWidget("Frame", "Arrow");
//   auto* m = new DualOptionsWidget(QStringList{"Contains", "Wildcard", "RegExp"});
//   w->setSelectedIndex(0);                           // 0 = leftmost
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
  // Applies `options` via setOptions, so a list with fewer than 2 entries
  // leaves the "Option A"/"Option B" defaults in place.
  explicit DualOptionsWidget(const QStringList& options, QWidget* parent = nullptr);

  // Replace all labels at once; triggers a geometry update. Lists with fewer
  // than 2 entries are ignored. If the current selection falls beyond the new
  // list it clamps to the last segment (emitting selectionChanged).
  void setOptions(const QStringList& options);
  void setOptions(const QString& opt0, const QString& opt1);

  [[nodiscard]] int optionCount() const {
    return static_cast<int>(options_.size());
  }
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
  // Selects the segment at index (0 = leftmost). Out-of-range indices are
  // ignored; no-op when already selected. Emits selectionChanged when the
  // value actually changes.
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

  QStringList options_{"Option A", "Option B"};
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
