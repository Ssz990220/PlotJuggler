// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/ComboBoxGradientDelegate.h"

#include <QLinearGradient>
#include <QModelIndex>
#include <QPainter>
#include <QPalette>
#include <QStyleOptionViewItem>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

namespace {

constexpr int kItemHorizontalPadding = 12;
constexpr int kItemMinHeight = 28;

}  // namespace

void ComboBoxGradientDelegate::paint(
    QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
  QStyleOptionViewItem opt = option;
  initStyleOption(&opt, index);

  const bool highlighted = (opt.state & QStyle::State_Selected) || (opt.state & QStyle::State_MouseOver);

  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);

  if (highlighted) {
    const auto fw_theme = theme::appTheme();
    // Special variant: per-state gradient from the Primary endpoint to the Error endpoint.
    const auto special = theme::special(theme::State::Checked, fw_theme);
    QLinearGradient grad(opt.rect.topLeft(), opt.rect.topRight());
    grad.setColorAt(0.0, special.first);
    grad.setColorAt(1.0, special.second);
    painter->fillRect(opt.rect, grad);
    painter->setPen(theme::onSpecial(theme::State::Checked, fw_theme));
  } else {
    painter->setPen(opt.palette.color(QPalette::Text));
  }
  const QRect text_rect = opt.rect.adjusted(kItemHorizontalPadding, 0, -kItemHorizontalPadding, 0);
  painter->drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft, opt.text);
  painter->restore();
}

QSize ComboBoxGradientDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
  QSize s = QStyledItemDelegate::sizeHint(option, index);
  s.setHeight(qMax(s.height(), kItemMinHeight));
  return s;
}

}  // namespace PJ
