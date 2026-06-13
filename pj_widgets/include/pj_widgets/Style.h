#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QProxyStyle>
#include <QSize>
#include <QStyleOption>
#include <algorithm>

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

  // Compact input chrome: Fusion sizes line edits / combo boxes / spin boxes a
  // few px taller than the text needs (~25 px at 10 pt). Trim every input to one
  // compact height so rows read tight, but never below font + 2 so text fits.
  QSize sizeFromContents(
      ContentsType type, const QStyleOption* option, const QSize& size, const QWidget* widget) const override {
    QSize s = QProxyStyle::sizeFromContents(type, option, size, widget);
    if (type == CT_LineEdit || type == CT_ComboBox || type == CT_SpinBox) {
      const int floor_h = option ? option->fontMetrics.height() + 2 : kInputHeight;
      s.setHeight(std::max(std::min(s.height(), kInputHeight), floor_h));
    }
    return s;
  }

  // The single governing height for ALL input chrome (line edits, combos, spin
  // boxes, and — via ScrubberBase::styledHeight querying CT_LineEdit — the
  // scrubbers). Fusion computes input height in sizeFromContents BEFORE the
  // stylesheet runs, so QSS can't set it; this constant is the one place to
  // retune it. The QSS `input_min_height` token is only a lower floor and does
  // not change the outer height once it is below this value.
  static constexpr int kInputHeight = 20;
};

}  // namespace PJ
