// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/object_topic_metadata.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
using namespace Qt::StringLiterals;

namespace pj::scene3d {

PJ::sdk::BuiltinObjectType builtinObjectTypeFor(const PJ::ObjectTopicDescriptor& descriptor) {
  if (descriptor.metadata_json.empty()) {
    return PJ::sdk::BuiltinObjectType::kNone;
  }
  const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(descriptor.metadata_json));
  if (!doc.isObject()) {
    return PJ::sdk::BuiltinObjectType::kNone;
  }
  const QJsonValue value = doc.object().value(u"builtin_object_type"_s);
  if (!value.isString()) {
    return PJ::sdk::BuiltinObjectType::kNone;
  }
  const auto parsed = PJ::sdk::parseBuiltinObjectType(value.toString().toStdString());
  return parsed.value_or(PJ::sdk::BuiltinObjectType::kNone);
}

}  // namespace pj::scene3d
