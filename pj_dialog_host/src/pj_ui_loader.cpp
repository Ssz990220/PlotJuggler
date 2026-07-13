// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#include "pj_plugins/host_qt/pj_ui_loader.hpp"

#include <pj_widgets/ComboBox.h>
#include <pj_widgets/CredentialsEditor.h>
#include <pj_widgets/DateRangePicker.h>
#include <pj_widgets/RangeSlider.h>
#include <pj_widgets/SectionHeaderBand.h>
#include <pj_widgets/ToggleSwitch.h>

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
  } else if (class_name == QLatin1String("SectionHeaderBand")) {
    w = new SectionHeaderBand(QString(), parent);
  } else if (class_name == QLatin1String("ToggleSwitch")) {
    w = new ToggleSwitch(parent);
  } else if (class_name == QLatin1String("PJ::ComboBox")) {
    // The app-canonical dropdown (gradient popup delegate + popup frame
    // fixes); .ui files opt in with the promoted-widget class name, exactly
    // like pj_app's LeftPanel.ui does.
    w = new ComboBox(parent);
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
