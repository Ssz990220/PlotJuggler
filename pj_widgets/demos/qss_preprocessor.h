#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>

namespace pj_widgets_demos {

// Mirrors pj_app::Theme's QSS preprocessor (palette block + ${token}
// substitution). pj_widgets demos can't depend on pj_app — pj_widgets is
// a leaf module per CLAUDE.md — so this is the demo-private copy. Loads
// stylesheet_<theme>.qss from PJ_QSS_DIR, expands ${token} placeholders
// against the palette block, and returns the body ready for setStyleSheet.
QString LoadAndExpandQss(const QString& theme);

// Convenience: load + apply via qApp->setStyleSheet, and write
// QSettings("StyleSheet::theme") so pj_widgets controls that consult
// currentTheme() (DoubleScrubber, SvgUtil icon palette, etc.) pick the
// matching per-theme paint colours.
void ApplyTheme(const QString& theme);

}  // namespace pj_widgets_demos
