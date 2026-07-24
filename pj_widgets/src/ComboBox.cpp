// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/ComboBox.h"

#include <QAbstractItemView>
#include <QBoxLayout>
#include <QFrame>
#include <QWidget>

#include "pj_widgets/ComboBoxGradientDelegate.h"
#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

void applyComboBoxStyling(QComboBox* combo) {
  if (combo == nullptr) {
    return;
  }
  combo->setItemDelegate(new ComboBoxGradientDelegate(combo));
  // QAbstractItemView inherits QFrame; without this, Qt's style paints
  // its own 1-px frame around the view ON TOP of the QSS border, giving
  // a double-line at the popup's edges. Killing the QFrame frame leaves
  // only the QSS border + border-radius visible.
  combo->view()->setFrameShape(QFrame::NoFrame);

  // Popup container (QComboBoxPrivateContainer): make it ARGB so QSS
  // border-radius clips corners to alpha=0; kill its frame; zero
  // margins so the inner view fills it exactly; suppress WM shadow.
  if (auto* popup_window = combo->view()->window()) {
    popup_window->setAttribute(Qt::WA_TranslucentBackground, true);
    popup_window->setWindowFlag(Qt::NoDropShadowWindowHint, true);
    if (auto* frame = qobject_cast<QFrame*>(popup_window)) {
      frame->setFrameShape(QFrame::NoFrame);
    }
    popup_window->setContentsMargins(
        theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
        theme::space(theme::Space::None));
    if (auto* lay = popup_window->layout()) {
      lay->setContentsMargins(
          theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
          theme::space(theme::Space::None));
      lay->setSpacing(theme::space(theme::Space::None));
    }
  }
}

ComboBox::ComboBox(QWidget* parent) : QComboBox(parent) {
  applyComboBoxStyling(this);
}

void ComboBox::showPopup() {
  QComboBox::showPopup();
  // Slide the popup window up so its 1-px top border overlaps the closed
  // combo's 1-px bottom border. Qt's default positioning places the two
  // borders as adjacent 1-px lines, which reads as a double horizontal
  // line on some compositors. A QProxyStyle override of
  // SC_ComboBoxListBoxPopup was tried — works geometrically but breaks
  // QSS application on the widget (Qt's QStyleSheetStyle stops applying
  // rules to a widget once setStyle() replaces its style).
  if (auto* popup_window = view()->window()) {
    constexpr int kPopupVerticalNudgePx = 2;
    popup_window->move(popup_window->x(), popup_window->y() - kPopupVerticalNudgePx);
  }
}

}  // namespace PJ
