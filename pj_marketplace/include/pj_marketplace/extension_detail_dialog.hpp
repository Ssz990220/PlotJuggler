#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_marketplace/extension.hpp"
#include "pj_widgets/Dialog.h"

namespace Ui {
class ExtensionDetailDialog;
}

namespace PJ {

// Canonical app chrome (PJ::Dialog): frameless title bar + close, no system
// (window-manager) decorations.
class ExtensionDetailDialog : public Dialog {
  Q_OBJECT

 public:
  // `needs_restart` mirrors the card's pending state (a staged install/update or a
  // staged uninstall awaiting a restart): when set, the dialog shows a disabled
  // "Needs Restart" indicator and offers no Install/Update/Uninstall action, so it
  // cannot re-stage an operation that is already pending.
  // `installing` mirrors the card's in-flight/queued state (this id is the active
  // install or is waiting in the install queue / Update All batch): when set, the
  // dialog shows a disabled "Installing" indicator and offers no action, so it
  // cannot enqueue the same operation a second time behind the running one.
  // `bundled_version` is the version this extension ships with ("core"), or empty
  // if it is not bundled. A core plugin at its bundled version shows the Uninstall
  // action disabled (it ships with the app); one updated above its bundled version
  // shows a "Downgrade to bundled" action instead — the bundled build is always a
  // compatible downgrade.
  explicit ExtensionDetailDialog(
      const Extension& ext, const QString& installed_version, bool needs_restart = false, bool installing = false,
      const QString& bundled_version = {}, QWidget* parent = nullptr);
  ~ExtensionDetailDialog() override;

 signals:
  void installRequested();
  void uninstallRequested();
  // Emitted for a core plugin updated above its bundled version: revert to the
  // shipped version (staged for the next launch) rather than a plain uninstall.
  void downgradeRequested();

 private:
  Ui::ExtensionDetailDialog* ui_ = nullptr;
};

}  // namespace PJ
