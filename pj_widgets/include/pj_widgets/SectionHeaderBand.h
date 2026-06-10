#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QWidget>

class QLabel;

namespace PJ {

// 24-px section header band — the titlebar-tone strip that titles panel
// sections ("Grid", "Curve Width", …). The background comes from the host
// app's stylesheet via the class selector (PJ--SectionHeaderBand); the 2-px
// leading indent is baked here so every band reads the same without
// per-instance QSS. Height is fixed but adjustable by the host (chrome
// metrics) through plain setFixedHeight.
class SectionHeaderBand : public QWidget {
  Q_OBJECT
 public:
  explicit SectionHeaderBand(const QString& title, QWidget* parent = nullptr);

  void setText(const QString& title);
  [[nodiscard]] QString text() const;

 private:
  QLabel* label_ = nullptr;
};

}  // namespace PJ
