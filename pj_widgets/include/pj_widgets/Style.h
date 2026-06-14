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

  // FALLBACK ONLY. Compact input height for when NO stylesheet is active (e.g.
  // a unit test that constructs widgets without the app QSS). In the running app
  // the QSS is the source of truth: once qApp->setStyleSheet() runs, a
  // QStyleSheetStyle wraps this style and governs input height from the
  // `input_outer_height` / `input_min_height` tokens in
  // resources/stylesheet_*.qss — this override is then bypassed. Kept so inputs
  // still resolve to one sane height without a stylesheet; mirrors the QSS outer
  // height, so the two never disagree.
  QSize sizeFromContents(
      ContentsType type, const QStyleOption* option, const QSize& size, const QWidget* widget) const override {
    QSize s = QProxyStyle::sizeFromContents(type, option, size, widget);
    if (type == CT_LineEdit || type == CT_ComboBox || type == CT_SpinBox) {
      s.setHeight(kInputHeight);
    }
    return s;
  }

  // The compact input outer height. The PRIMARY knob is the QSS token
  // `input_outer_height` (which the app actually uses); keep this C++ mirror in
  // lockstep with it (like ThemeColors mirrors the palette) so the no-stylesheet
  // fallback above matches what the stylesheet produces.
  static constexpr int kInputHeight = 20;
};

}  // namespace PJ
