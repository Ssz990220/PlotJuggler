#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>

namespace PJ {

// Application-wide event filter that bypasses QSS specificity battles
// by directly manipulating widgets when they're polished:
//
//   - Every QMenu (including auto-generated context menus) receives
//     objectName="PJMenu" so the QMenu#PJMenu QSS rules apply.
//
//   - Every QComboBox has its popup view's palette overridden with the
//     theme's popup background, and its frame shape cleared. QSS rules
//     that should have done this lose to Fusion's hard-coded
//     palette(Window) / palette(Base) painting of the dropdown
//     container — setting the palette directly wins because that's the
//     value Fusion reads.
//
//   - Every QComboBoxPrivateContainer (the private QFrame Qt wraps
//     around a combo's popup listview) gets its frame stripped, drop
//     shadow disabled, and palette pointed at the popup background
//     so the container's own paint matches the listview underneath.
//
// Install once on qApp at startup with installEventFilter().
class WidgetTuner : public QObject {
  Q_OBJECT
 public:
  using QObject::QObject;

  bool eventFilter(QObject* watched, QEvent* event) override;
};

}  // namespace PJ
