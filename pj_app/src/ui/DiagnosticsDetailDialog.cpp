// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/DiagnosticsDetailDialog.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QPlainTextEdit>
#include <QToolButton>

#include "pj_widgets/SvgUtil.h"
#include "ui_DiagnosticsDetailDialog.h"

namespace PJ {

namespace {

QString levelLabel(DiagnosticLevel level) {
  switch (level) {
    case DiagnosticLevel::kError:
      return DiagnosticsDetailDialog::tr("Error");
    case DiagnosticLevel::kWarning:
      return DiagnosticsDetailDialog::tr("Warning");
    case DiagnosticLevel::kInfo:
    default:
      return DiagnosticsDetailDialog::tr("Info");
  }
}

}  // namespace

DiagnosticsDetailDialog::DiagnosticsDetailDialog(const DiagnosticRecord& record, QWidget* parent)
    : Dialog(parent), ui_(new Ui::DiagnosticsDetailDialog) {
  setDialogTitle(tr("Diagnostic"));
  setAttribute(Qt::WA_DeleteOnClose, true);
  setMinimumSize(600, 440);

  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);

  ui_->buttonCopy->setIcon(LoadSvg(":/resources/svg/copy.svg", currentTheme()));
  connect(ui_->buttonCopy, &QToolButton::clicked, this, [this, record]() {
    QGuiApplication::clipboard()->setText(record.message);
  });

  ui_->detailTimestamp->setText(record.timestamp.toString(QStringLiteral("ddd d MMM yyyy HH:mm:ss")));
  ui_->detailLevel->setText(levelLabel(record.level));
  ui_->detailSource->setText(record.source.isEmpty() ? QStringLiteral("—") : record.source);
  ui_->detailId->setText(record.id.isEmpty() ? QStringLiteral("—") : record.id);
  ui_->detailMessage->setPlainText(record.message);

  if (parent != nullptr) {
    const QPoint centre = parent->mapToGlobal(parent->rect().center());
    move(centre.x() - width() / 2, centre.y() - height() / 2);
  }
}

DiagnosticsDetailDialog::~DiagnosticsDetailDialog() {
  delete ui_;
}

}  // namespace PJ
