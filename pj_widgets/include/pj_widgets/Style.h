#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QProxyStyle>

namespace PJ {

// QProxyStyle layered over Fusion to suppress two Qt-default Linux
// behaviours that clash with the app's chrome:
//   - SH_DialogButtonBox_ButtonsHaveIcons: stops the platform from
//     stamping its own theme glyphs onto Ok / Cancel / Save / Yes / No
//     inside any QDialogButtonBox still used by plugin-provided dialogs
//     loaded through pj_dialog_host.
//   - SH_UnderlineShortcut: turns off the underline-the-mnemonic
//     letter ("&Save" → "S" with an underline) that bleeds through on
//     menus and dialog buttons.
class Style : public QProxyStyle {
  Q_OBJECT
 public:
  using QProxyStyle::QProxyStyle;

  int styleHint(
      StyleHint hint, const QStyleOption* option = nullptr, const QWidget* widget = nullptr,
      QStyleHintReturn* return_data = nullptr) const override {
    if (hint == SH_DialogButtonBox_ButtonsHaveIcons || hint == SH_UnderlineShortcut) {
      return 0;
    }
    return QProxyStyle::styleHint(hint, option, widget, return_data);
  }
};

}  // namespace PJ
