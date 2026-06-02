#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QStyledItemDelegate>

class QModelIndex;
class QPainter;
class QStyleOptionViewItem;

namespace PJ {

// Custom item delegate that paints a horizontal light_purple → light_blue
// gradient on selected/hovered rows. Works around a Qt QSS limitation:
// QComboBox popups paint items via QStyle::drawPrimitive(PE_PanelItemViewItem)
// which consults QPalette::Highlight (a solid colour) and ignores any
// `background: qlineargradient(...)` rule on `::item:selected`. By overriding
// paint() we sidestep the QSS path entirely.
//
// Apply to a QComboBox via:
//   combo->setItemDelegate(new PJ::ComboBoxGradientDelegate(combo));
class ComboBoxGradientDelegate : public QStyledItemDelegate {
  Q_OBJECT
 public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

}  // namespace PJ
