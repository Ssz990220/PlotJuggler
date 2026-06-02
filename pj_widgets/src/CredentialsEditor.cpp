// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/CredentialsEditor.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QToolButton>

namespace PJ {

bool CredentialsEditor::isApiKeyValid(const QString& key, const QString& pattern) {
  if (pattern.isEmpty()) {
    return !key.isEmpty();
  }
  const QRegularExpression re(pattern);
  // A malformed pattern can't validate anything; fall back to the non-empty rule
  // rather than rejecting every key on a plugin typo.
  if (!re.isValid()) {
    return !key.isEmpty();
  }
  const QRegularExpressionMatch m = re.match(key);
  return m.hasMatch() && m.capturedLength() == key.length();
}

bool CredentialsEditor::isCertReadable(const QString& path) {
  if (path.isEmpty()) {
    return false;
  }
  const QFileInfo fi(path);
  return fi.exists() && fi.isReadable();
}

void CredentialsEditor::applyTick(QLabel* tick, const QString& text, bool ok) {
  if (text.isEmpty()) {
    tick->clear();
    tick->setStyleSheet(QString());
    return;
  }
  tick->setText(ok ? QString::fromUtf8("✓") : QString::fromUtf8("✗"));
  tick->setStyleSheet(
      ok ? QStringLiteral("color: green; font-weight: bold;") : QStringLiteral("color: red; font-weight: bold;"));
}

CredentialsEditor::CredentialsEditor(QWidget* parent) : QWidget(parent) {
  auto* layout = new QFormLayout(this);

  // Certificate path + Browse + validity tick.
  cert_path_ = new QLineEdit(this);
  cert_path_->setObjectName(QStringLiteral("certPath"));
  cert_tick_ = new QLabel(this);
  cert_tick_->setObjectName(QStringLiteral("certTick"));
  auto* browse = new QPushButton(tr("Browse..."), this);
  auto* cert_row = new QHBoxLayout();
  cert_row->setContentsMargins(0, 0, 0, 0);
  cert_row->addWidget(cert_path_);
  cert_row->addWidget(cert_tick_);
  cert_row->addWidget(browse);
  layout->addRow(tr("Certificate:"), cert_row);

  // API key, masked, with a Show/Hide reveal toggle + validity tick.
  api_key_ = new QLineEdit(this);
  api_key_->setObjectName(QStringLiteral("apiKey"));
  api_key_->setEchoMode(QLineEdit::Password);
  api_key_tick_ = new QLabel(this);
  api_key_tick_->setObjectName(QStringLiteral("apiKeyTick"));
  auto* reveal = new QToolButton(this);
  reveal->setObjectName(QStringLiteral("apiKeyReveal"));
  reveal->setCheckable(true);
  reveal->setText(tr("Show"));
  auto* key_row = new QHBoxLayout();
  key_row->setContentsMargins(0, 0, 0, 0);
  key_row->addWidget(api_key_);
  key_row->addWidget(api_key_tick_);
  key_row->addWidget(reveal);
  layout->addRow(tr("API Key:"), key_row);

  // Plaintext fallback.
  allow_insecure_ = new QCheckBox(tr("Allow insecure / plaintext connection"), this);
  allow_insecure_->setObjectName(QStringLiteral("allowInsecure"));
  layout->addRow(QString(), allow_insecure_);

  connect(reveal, &QToolButton::toggled, this, [this, reveal](bool revealed) {
    api_key_->setEchoMode(revealed ? QLineEdit::Normal : QLineEdit::Password);
    reveal->setText(revealed ? tr("Hide") : tr("Show"));
  });

  connect(browse, &QPushButton::clicked, this, [this]() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Select certificate"), cert_path_->text());
    if (!path.isEmpty()) {
      cert_path_->setText(path);
    }
  });

  // Live ticks — also fire on the plugin's pre-fill setText (PJ3 parity).
  connect(cert_path_, &QLineEdit::textChanged, this, [this](const QString& text) {
    applyTick(cert_tick_, text, isCertReadable(text));
  });
  connect(api_key_, &QLineEdit::textChanged, this, [this]() { refreshApiKeyTick(); });
}

void CredentialsEditor::setApiKeyPattern(const QString& pattern) {
  api_key_pattern_ = pattern;
  refreshApiKeyTick();
}

void CredentialsEditor::refreshApiKeyTick() {
  const QString text = api_key_->text();
  applyTick(api_key_tick_, text, isApiKeyValid(text, api_key_pattern_));
}

}  // namespace PJ
