#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QComboBox>

namespace PJ {

// QComboBox subclass that auto-installs PJ::ComboBoxGradientDelegate so
// the popup paints the app's light_purple → light_blue gradient on the
// selected / hovered item. Use this instead of QComboBox everywhere in
// PJ4 so every dropdown gets the styling without per-call-site wiring.
//
// In .ui files, promote QComboBox to PJ::ComboBox via Qt Designer's
// "Promoted Widgets" mechanism (header: pj_widgets/ComboBox.h).
class ComboBox : public QComboBox {
  Q_OBJECT
 public:
  explicit ComboBox(QWidget* parent = nullptr);

  // Nudge the popup window up by a small offset so its top border
  // overlaps the closed combo's bottom border rather than stacking next
  // to it (which reads as a double line on some compositors).
  void showPopup() override;
};

}  // namespace PJ
