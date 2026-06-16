// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#include "pj_plugins/host_qt/pj_ui_loader.hpp"

#include <pj_widgets/CredentialsEditor.h>
#include <pj_widgets/DateRangePicker.h>
#include <pj_widgets/RangeSlider.h>

namespace PJ {

PjUiLoader::PjUiLoader(QObject* parent) : QUiLoader(parent) {
  // Don't let QUiLoader pull in widgets from external designer plugin paths;
  // we only ever create standard Qt widgets + the ones registered below.
  setLanguageChangeEnabled(false);
}

QWidget* PjUiLoader::createWidget(const QString& class_name, QWidget* parent, const QString& name) {
  QWidget* w = nullptr;
  if (class_name == QLatin1String("RangeSlider")) {
    w = new RangeSlider(Qt::Horizontal, RangeSlider::kDoubleHandles, parent);
  } else if (class_name == QLatin1String("DateRangePicker")) {
    w = new DateRangePicker(parent);
  } else if (class_name == QLatin1String("CredentialsEditor")) {
    w = new CredentialsEditor(parent);
  }

  if (w != nullptr) {
    if (!name.isEmpty()) {
      w->setObjectName(name);
    }
    return w;
  }
  return QUiLoader::createWidget(class_name, parent, name);
}

}  // namespace PJ
