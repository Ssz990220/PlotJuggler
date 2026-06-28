#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QIcon>
#include <QProxyStyle>
#include <QSize>
#include <QStyleOption>
#include <algorithm>

#include "pj_widgets/SvgUtil.h"  // loadSvg, currentTheme

namespace PJ {

// QProxyStyle layered over Fusion to adjust a few Qt-default behaviours
// that clash with the app's chrome:
//   - SH_DialogButtonBox_ButtonsHaveIcons: stops the platform from
//     stamping its own theme glyphs onto Ok / Cancel / Save / Yes / No
//     inside any QDialogButtonBox still used by plugin-provided dialogs
//     loaded through pj_dialog_host.
//   - SH_UnderlineShortcut: turns off the underline-the-mnemonic
//     letter ("&Save" → "S" with an underline) that bleeds through on
//     menus and dialog buttons.
//   - SP_LineEditClearButton: swaps Qt's default clear-field glyph (a
//     filled dark disc with a white X on Fusion) for the app's own
//     close X, so a QLineEdit's "clear" affordance matches the close X
//     used everywhere else (tabs, dialogs).
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

  // Replace Qt's default QLineEdit clear button (a filled dark disc with a white
  // glyph on Fusion) with the app's own close X (resources/svg/close-button.svg,
  // the same glyph used for tab / dialog close), so a filter field's "clear" X
  // matches the rest of the chrome. Honoured even while the app stylesheet is
  // active: QStyleSheetStyle forwards standardIcon() to this base style when no QSS
  // rule supplies the icon (verified). loadSvg tints the glyph to the active theme
  // ink and caches it; QLineEdit caches the returned icon, so a live theme switch
  // keeps the prior tint until the field next rebuilds the button.
  QIcon standardIcon(
      StandardPixmap standard_icon, const QStyleOption* option = nullptr,
      const QWidget* widget = nullptr) const override {
    if (standard_icon == SP_LineEditClearButton) {
      return QIcon(loadSvg(QStringLiteral(":/resources/svg/close-button.svg"), currentTheme()));
    }
    return QProxyStyle::standardIcon(standard_icon, option, widget);
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
