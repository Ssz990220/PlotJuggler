#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>

namespace PJ::theme {

// C++ mirror of the brand-colour tokens defined in
// resources/stylesheet_{dark,light}.qss. The QSS preprocessor handles
// substitution at stylesheet-load time but doesn't reach widgets that
// paint themselves (ScrubberBase, ComboBoxGradientDelegate, etc.). When
// the palette values change in the QSS, update these in lockstep —
// this header is the single C++ source of truth.

inline const QColor kPurple(0xCC, 0x00, 0xCC);
inline const QColor kPurpleDark(0x99, 0x00, 0x99);
inline const QColor kBlue(0x11, 0x77, 0xFF);
inline const QColor kLightBlue(0xC2, 0xDC, 0xFF);
inline const QColor kLightPurple(0xFF, 0xAE, 0xFF);

// Text painted on top of `item_selection_background` (light/mid blue in
// both themes — selection_text in the QSS palette).
inline const QColor kSelectionText(0x00, 0x00, 0x00);

// Per-theme input chrome fill — matches the `input_background` palette
// token. Dark gets a touch lighter than dark_background, light gets
// pure white. Used by ScrubberBase::paintEvent etc.
inline const QColor kInputBackgroundDark(0x4D, 0x4D, 0x5A);
inline const QColor kInputBackgroundLight(0xFF, 0xFF, 0xFF);

// Semantic accent — error / danger. Mirrors the `accent_error` QSS token
// (identical in both themes today). Used for invalid/warning row text and
// other self-painted widgets that can't reach the QSS palette.
inline const QColor kAccentError(0xD3, 0x2F, 0x2F);

}  // namespace PJ::theme
