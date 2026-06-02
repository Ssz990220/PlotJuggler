#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/DiagnosticHistory.h"
#include "pj_widgets/Dialog.h"

namespace Ui {
class DiagnosticsDetailDialog;
}

namespace PJ {

// App-styled (frameless, custom title bar) dialog showing a single
// diagnostic's full record (level, timestamp, source, id, message).
// Modeless; the constructor sets WA_DeleteOnClose so callers can do
// `dlg->show()` and forget about lifetime.
class DiagnosticsDetailDialog : public Dialog {
  Q_OBJECT
 public:
  explicit DiagnosticsDetailDialog(const DiagnosticRecord& record, QWidget* parent = nullptr);
  ~DiagnosticsDetailDialog() override;

 private:
  Ui::DiagnosticsDetailDialog* ui_;
};

}  // namespace PJ
