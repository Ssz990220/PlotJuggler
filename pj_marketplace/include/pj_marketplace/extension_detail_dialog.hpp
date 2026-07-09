#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDialog>

#include "pj_marketplace/extension.hpp"

namespace Ui {
class ExtensionDetailDialog;
}

namespace PJ {

class ExtensionDetailDialog : public QDialog {
  Q_OBJECT

 public:
  // `needs_restart` mirrors the card's pending state (a staged install/update or a
  // staged uninstall awaiting a restart): when set, the dialog shows a disabled
  // "Needs Restart" indicator and offers no Install/Update/Uninstall action, so it
  // cannot re-stage an operation that is already pending.
  explicit ExtensionDetailDialog(
      const Extension& ext, const QString& installed_version, bool needs_restart = false, QWidget* parent = nullptr);
  ~ExtensionDetailDialog() override;

 signals:
  void installRequested();
  void uninstallRequested();

 private:
  Ui::ExtensionDetailDialog* ui_ = nullptr;
};

}  // namespace PJ
