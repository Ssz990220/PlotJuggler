#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QProgressBar>

namespace PJ {

// App-styled QProgressBar variant: a rounded, thick-outlined trough with a
// light-blue rounded fill and a single-colour centred caption (e.g.
// "Loading MCAP"). The look is supplied entirely by the host app's
// stylesheet through the class selector `PJ--ProgressBar` — see the
// `PJ--ProgressBar` / `PJ--ProgressBar::chunk` rules in
// resources/stylesheet_{light,dark}.qss.
//
// Why a subclass at all, given it carries no extra logic: a bare
// `QProgressBar { }` QSS rule would restyle *every* progress bar in the app
// (the ProgressDialog bar included). A dedicated type gives the stylesheet a
// `PJ--ProgressBar` selector to scope the look to opt-in instances. That only
// works because Q_OBJECT gives this class its own meta-object — without it
// `metaObject()->className()` would still report "QProgressBar" and the
// selector would silently never match. Do not drop Q_OBJECT.
//
// The caption is the standard QProgressBar `format()` string: set a literal
// (`setFormat("Loading MCAP")`) for a fixed label, or keep the default "%p%"
// for a percentage. The value/range still drive the fill width regardless.
// The text is drawn once, on top of the fill, in a single colour — it does
// NOT split/invert where the fill crosses behind it (that effect would need a
// custom paintEvent, which this variant deliberately avoids).
class ProgressBar : public QProgressBar {
  Q_OBJECT
 public:
  explicit ProgressBar(QWidget* parent = nullptr);
};

}  // namespace PJ
