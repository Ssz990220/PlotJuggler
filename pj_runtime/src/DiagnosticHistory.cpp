// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/DiagnosticHistory.h"

#include <algorithm>

#include "pj_marketplace/qt_diagnostic_bridge.hpp"

namespace PJ {

DiagnosticHistory::DiagnosticHistory(QObject* parent) : QObject(parent) {}

void DiagnosticHistory::connectBridge(QtDiagnosticBridge* bridge) {
  if (bridge == nullptr) {
    return;
  }
  connect(
      bridge, &QtDiagnosticBridge::diagnosticReported, this,
      [this](int level, QString source, QString id, QString message) {
        record(static_cast<DiagnosticLevel>(level), source, id, message);
      });
}

void DiagnosticHistory::record(const DiagnosticRecord& r) {
  records_.prepend(r);
  trim();
  emit recorded(records_.front());
}

void DiagnosticHistory::record(
    DiagnosticLevel level, const QString& source, const QString& id, const QString& message) {
  record(DiagnosticRecord{level, source, id, message, QDateTime::currentDateTime()});
}

void DiagnosticHistory::clear() {
  if (records_.isEmpty()) {
    return;
  }
  records_.clear();
  emit cleared();
}

void DiagnosticHistory::setMaxRecords(int n) {
  max_records_ = std::max(1, n);
  trim();
}

int DiagnosticHistory::maxRecords() const {
  return max_records_;
}

int DiagnosticHistory::size() const {
  return static_cast<int>(records_.size());
}

bool DiagnosticHistory::isEmpty() const {
  return records_.isEmpty();
}

QList<DiagnosticRecord> DiagnosticHistory::snapshot() const {
  return records_;
}

void DiagnosticHistory::trim() {
  while (records_.size() > max_records_) {
    records_.removeLast();
  }
}

}  // namespace PJ
