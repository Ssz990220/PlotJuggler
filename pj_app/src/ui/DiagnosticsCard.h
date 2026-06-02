#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QFrame>

#include "pj_runtime/DiagnosticHistory.h"

class QEvent;
class QMouseEvent;

namespace Ui {
class DiagnosticsCard;
}

namespace PJ {

// Single diagnostic row inside DiagnosticsPopup. Shows level icon,
// timestamp, elided message, and a copy button. Body-click activates
// (caller opens a detail dialog); copy-click writes the full text to
// the clipboard without activating the row.
class DiagnosticsCard : public QFrame {
  Q_OBJECT
 public:
  explicit DiagnosticsCard(const DiagnosticRecord& record, QWidget* parent = nullptr);
  ~DiagnosticsCard() override;

  [[nodiscard]] const DiagnosticRecord& record() const;

 signals:
  void activated(const DiagnosticRecord& item);
  void copyRequested(const DiagnosticRecord& item);

 protected:
  void mouseReleaseEvent(QMouseEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void applyElidedMessage();
  [[nodiscard]] static QString levelIconPath(DiagnosticLevel level);

  Ui::DiagnosticsCard* ui_;
  DiagnosticRecord record_;
};

}  // namespace PJ
