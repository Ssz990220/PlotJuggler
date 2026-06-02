// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/DiagnosticsCard.h"

#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QToolButton>

#include "pj_widgets/SvgUtil.h"
#include "ui_DiagnosticsCard.h"

namespace PJ {

DiagnosticsCard::DiagnosticsCard(const DiagnosticRecord& record, QWidget* parent)
    : QFrame(parent), ui_(new Ui::DiagnosticsCard), record_(record) {
  setAttribute(Qt::WA_StyledBackground, true);
  ui_->setupUi(this);

  ui_->cardLevelIcon->setPixmap(
      RenderSvgPixmap(levelIconPath(record_.level), currentTheme(), QSize(16, 16), devicePixelRatioF()));
  ui_->cardTimestamp->setText(record_.timestamp.toString(QStringLiteral("HH:mm:ss")));
  ui_->cardMessage->setToolTip(record_.message);
  ui_->cardMessage->installEventFilter(this);
  applyElidedMessage();

  ui_->cardCopyButton->setIcon(LoadSvg(":/resources/svg/copy.svg", currentTheme()));

  connect(ui_->cardCopyButton, &QToolButton::clicked, this, [this]() { emit copyRequested(record_); });
}

DiagnosticsCard::~DiagnosticsCard() {
  delete ui_;
}

const DiagnosticRecord& DiagnosticsCard::record() const {
  return record_;
}

void DiagnosticsCard::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
    emit activated(record_);
  }
  QFrame::mouseReleaseEvent(event);
}

bool DiagnosticsCard::eventFilter(QObject* watched, QEvent* event) {
  if (watched == ui_->cardMessage && event->type() == QEvent::Resize) {
    applyElidedMessage();
  }
  return QFrame::eventFilter(watched, event);
}

void DiagnosticsCard::applyElidedMessage() {
  const QFontMetrics fm(ui_->cardMessage->font());
  ui_->cardMessage->setText(fm.elidedText(record_.message, Qt::ElideRight, ui_->cardMessage->width()));
}

QString DiagnosticsCard::levelIconPath(DiagnosticLevel level) {
  switch (level) {
    case DiagnosticLevel::kError:
      return QStringLiteral(":/resources/svg/diag_error.svg");
    case DiagnosticLevel::kWarning:
      return QStringLiteral(":/resources/svg/diag_warning.svg");
    case DiagnosticLevel::kInfo:
    default:
      return QStringLiteral(":/resources/svg/diag_info.svg");
  }
}

}  // namespace PJ
