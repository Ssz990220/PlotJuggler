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
  // `is_bundled` marks a "core" extension shipped with the application: it can be
  // updated but never uninstalled, so the Uninstall action is shown disabled.
  explicit ExtensionDetailDialog(
      const Extension& ext, const QString& installed_version, bool needs_restart = false, bool installing = false,
      bool is_bundled = false, QWidget* parent = nullptr);
  ~ExtensionDetailDialog() override;

 signals:
  void installRequested();
  void uninstallRequested();

 private:
  Ui::ExtensionDetailDialog* ui_ = nullptr;
};

}  // namespace PJ
