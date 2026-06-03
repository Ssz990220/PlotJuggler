#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QPointer>
#include <QWidget>

class QVBoxLayout;

namespace PJ {

class ConfigPanelHost : public QWidget {
  Q_OBJECT
 public:
  explicit ConfigPanelHost(QWidget* parent = nullptr);

  // Shows `widget` as the sole hosted panel, taking ownership of it: the host
  // reparents it and schedules the previously hosted widget for deletion via
  // deleteLater(). Callers must not delete a widget after handing it over.
  // Passing nullptr is equivalent to clear().
  void setConfigWidget(QWidget* widget);

  // Removes and deletes (deleteLater) the current hosted widget, if any.
  void clear();

 private:
  QVBoxLayout* layout_ = nullptr;
  QPointer<QWidget> current_widget_;
};

}  // namespace PJ
