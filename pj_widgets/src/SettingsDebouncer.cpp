// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/SettingsDebouncer.h"

#include <QCoreApplication>
#include <QSettings>
#include <utility>

namespace PJ {

SettingsDebouncer::SettingsDebouncer(QString group, int delay_ms, QObject* parent)
    : QObject(parent), group_(std::move(group)) {
  timer_.setSingleShot(true);
  timer_.setInterval(delay_ms);
  connect(&timer_, &QTimer::timeout, this, &SettingsDebouncer::flush);
  // A quit can land inside the settle window; flush so the last change persists.
  if (auto* app = QCoreApplication::instance()) {
    connect(app, &QCoreApplication::aboutToQuit, this, &SettingsDebouncer::flush);
  }
}

SettingsDebouncer::~SettingsDebouncer() {
  flush();
}

void SettingsDebouncer::queue(QAnyStringView key, const QVariant& value) {
  pending_.insert(key.toString(), value);
  timer_.start();
}

void SettingsDebouncer::queue(const QHash<QString, QVariant>& values) {
  if (values.isEmpty()) {
    return;
  }
  for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
    pending_.insert(it.key(), it.value());
  }
  timer_.start();
}

void SettingsDebouncer::flush() {
  if (pending_.isEmpty()) {
    return;
  }
  QSettings settings;
  if (!group_.isEmpty()) {
    settings.beginGroup(group_);
  }
  for (auto it = pending_.constBegin(); it != pending_.constEnd(); ++it) {
    settings.setValue(it.key(), it.value());
  }
  pending_.clear();
}

}  // namespace PJ
