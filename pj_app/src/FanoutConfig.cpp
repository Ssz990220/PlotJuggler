// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "FanoutConfig.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>

namespace PJ::detail {

namespace {
Q_LOGGING_CATEGORY(lcFanout, "pj.app.fileloader")
}  // namespace

std::vector<std::string> extractFanout(std::string_view config) {
  const auto single_instance_fallback = [&]() { return std::vector<std::string>{std::string(config)}; };
  if (config.empty()) {
    return single_instance_fallback();
  }
  const QByteArray bytes(config.data(), static_cast<qsizetype>(config.size()));
  const auto doc = QJsonDocument::fromJson(bytes);
  if (!doc.isObject()) {
    return single_instance_fallback();
  }
  const QJsonValue v = doc.object().value(QStringLiteral("__pj_fanout"));
  if (!v.isArray()) {
    return single_instance_fallback();
  }
  const QJsonArray arr = v.toArray();
  if (arr.isEmpty()) {
    return single_instance_fallback();
  }
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(arr.size()));
  for (qsizetype idx = 0; idx < arr.size(); ++idx) {
    const QJsonValue entry = arr.at(idx);
    if (!entry.isString()) {
      // A non-string entry means the plugin emitted a malformed fanout array.
      // Skipping it would silently drop an instance the user asked for, so log
      // it — the caller surfaces the aggregate result to the user.
      qCWarning(lcFanout) << "[FileLoader] extractFanout: dropping non-string __pj_fanout entry at index" << idx;
      continue;
    }
    out.emplace_back(entry.toString().toStdString());
  }
  if (out.empty()) {
    qCWarning(lcFanout) << "[FileLoader] extractFanout: __pj_fanout array had no usable string entries; "
                           "falling back to single-instance import";
    return single_instance_fallback();
  }
  return out;
}

QString parseDisplaySuffix(std::string_view cfg, const QString& fallback) {
  if (cfg.empty()) {
    return fallback;
  }
  const QByteArray bytes(cfg.data(), static_cast<qsizetype>(cfg.size()));
  const auto doc = QJsonDocument::fromJson(bytes);
  if (!doc.isObject()) {
    return fallback;
  }
  const QJsonValue v = doc.object().value(QStringLiteral("display_suffix"));
  if (!v.isString()) {
    return fallback;
  }
  const QString s = v.toString();
  return s.isEmpty() ? fallback : s;
}

QString parseDisplayName(std::string_view cfg) {
  if (cfg.empty()) {
    return QString{};
  }
  const QByteArray bytes(cfg.data(), static_cast<qsizetype>(cfg.size()));
  const auto doc = QJsonDocument::fromJson(bytes);
  if (!doc.isObject()) {
    return QString{};
  }
  const QJsonValue v = doc.object().value(QStringLiteral("display_name"));
  return v.isString() ? v.toString() : QString{};
}

}  // namespace PJ::detail
