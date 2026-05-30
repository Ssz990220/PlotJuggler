#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// QUiLoader subclass that teaches QUiLoader to instantiate the host-provided
// custom widgets (RangeSlider, DateRangePicker, CredentialsEditor) so plugin .ui files can declare
// them by class name. Plain Qt widget classes fall through to the base
// QUiLoader. Used by both PanelEngine and DialogEngine so every dialog/panel
// sees the same widget vocabulary.

#include <QUiLoader>

namespace PJ {

class PjUiLoader : public QUiLoader {
 public:
  explicit PjUiLoader(QObject* parent = nullptr);

  QWidget* createWidget(const QString& class_name, QWidget* parent, const QString& name) override;
};

}  // namespace PJ
