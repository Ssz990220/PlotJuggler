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

UniqueObjectTopicResolution resolveUniqueObjectTopic(
    PJ::ObjectStore& store, std::string_view topic_name, PJ::sdk::BuiltinObjectType object_type) {
  std::optional<PJ::ObjectTopicId> match;
  for (const PJ::ObjectTopicId candidate : store.listTopics()) {
    const PJ::ObjectTopicDescriptor& descriptor = store.descriptor(candidate);
    if (descriptor.topic_name != topic_name || builtinObjectTypeFor(descriptor) != object_type) {
      continue;
    }
    if (match.has_value()) {
      return UniqueObjectTopicResolution{.topic_id = std::nullopt, .ambiguous = true};
    }
    match = candidate;
  }
  return UniqueObjectTopicResolution{.topic_id = match};
}

}  // namespace pj::scene3d
