#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;

namespace PJ {

// Composite credentials editor — the host-side widget for `cert_dialog.ui`'s
// `<widget class="CredentialsEditor">`. It builds three inner inputs with the
// exact objectNames the PanelEngine sub-dialog harvester expects:
//   - `certPath`      QLineEdit (+ a Browse button, + `certTick` validity label)
//   - `apiKey`        QLineEdit, masked, with a Show/Hide reveal toggle, + `apiKeyTick`
//   - `allowInsecure` QCheckBox (plaintext fallback)
// The plugin pre-fills these via setText/setChecked addressed by name and reads
// them back through onTextChanged/onToggled on sub-dialog OK. Validity ticks
// (green ✓ / red ✗) are self-contained: they update live on textChanged using
// the predicates below, so no protocol round-trip is needed (port of PJ3
// CertDialog::validateKey/validateCert).
//
// Domain-neutral by design: this widget knows nothing about any specific cloud
// provider's key format. The api-key tick defaults to "non-empty is valid". A
// plugin that wants a stricter, provider-specific shape declares the regular
// expression in its own `.ui` via the `apiKeyPattern` property (applied by
// QUiLoader at construction) — e.g.
//   <property name="apiKeyPattern"><string>^msco_[a-z0-9]{32}_[0-9a-f]{8}$</string></property>
// so the format policy lives with the plugin that owns it, not in pj_widgets.
class CredentialsEditor : public QWidget {
  Q_OBJECT
  // Optional regex the api-key must fully match to be considered valid. Empty
  // (the default) means "any non-empty key is valid". Set by the plugin's .ui.
  Q_PROPERTY(QString apiKeyPattern READ apiKeyPattern WRITE setApiKeyPattern)
 public:
  explicit CredentialsEditor(QWidget* parent = nullptr);

  [[nodiscard]] QString apiKeyPattern() const {
    return api_key_pattern_;
  }
  // Set the api-key validation regex and re-evaluate the live tick. An empty
  // (or invalid) pattern falls back to the non-empty check.
  void setApiKeyPattern(const QString& pattern);

  // True when `key` is acceptable under `pattern`: if `pattern` is empty, any
  // non-empty key passes; otherwise `key` must fully match the regex. Pure, so
  // tested directly.
  [[nodiscard]] static bool isApiKeyValid(const QString& key, const QString& pattern = QString());
  // True when `path` names an existing, readable file (PJ3 tls_utils.h isCertReadable).
  [[nodiscard]] static bool isCertReadable(const QString& path);

 private:
  // Set `tick` to green ✓ / red ✗ / blank (when `text` is empty) per `ok`.
  static void applyTick(QLabel* tick, const QString& text, bool ok);
  // Re-run the api-key tick against the current text + pattern.
  void refreshApiKeyTick();

  QLineEdit* cert_path_ = nullptr;
  QLineEdit* api_key_ = nullptr;
  QCheckBox* allow_insecure_ = nullptr;
  QLabel* cert_tick_ = nullptr;
  QLabel* api_key_tick_ = nullptr;
  QString api_key_pattern_;
};

}  // namespace PJ
