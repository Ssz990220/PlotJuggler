#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QComboBox>

namespace PJ {

// Apply PJ's dropdown styling to an EXISTING QComboBox in place: install the
// gradient item delegate and strip the popup's native frame/shadow so only the
// QSS border shows. This is the shared core of PJ::ComboBox's constructor, also
// used to upgrade plain QComboBoxes loaded from plugin .ui files without
// swapping the widget (so their model, current index, and signal connections
// survive). Idempotent. NOTE: the 2px popup-overlap nudge in
// ComboBox::showPopup() is NOT applied here — it needs the subclass.
void applyComboBoxStyling(QComboBox* combo);

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
