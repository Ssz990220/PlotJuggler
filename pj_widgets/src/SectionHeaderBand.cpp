// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/SectionHeaderBand.h"

#include <QHBoxLayout>
#include <QLabel>

namespace PJ {

SectionHeaderBand::SectionHeaderBand(const QString& title, QWidget* parent) : QWidget(parent) {
  // QWidget subclasses don't paint stylesheet backgrounds unless told to
  // (plain QWidgets from .ui files get this implicitly).
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedHeight(24);

  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  label_ = new QLabel(title, this);
  // Leading inset via the label's indent, not layout margins — hosts that
  // re-apply chrome metrics overwrite the layout margins at runtime.
  label_->setIndent(2);
  layout->addWidget(label_);
  layout->addStretch(1);
}

void SectionHeaderBand::setText(const QString& title) {
  label_->setText(title);
}

QString SectionHeaderBand::text() const {
  return label_->text();
}

}  // namespace PJ
