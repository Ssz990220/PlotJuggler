// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene_common/layer_factory.h"

#include <utility>

namespace PJ {

void LayerFactory::registerType(sdk::BuiltinObjectType object_type, Creator creator) {
  creators_[object_type] = std::move(creator);
}

bool LayerFactory::supports(sdk::BuiltinObjectType object_type) const {
  return creators_.find(object_type) != creators_.end();
}

std::unique_ptr<ISceneLayer> LayerFactory::create(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) const {
  const auto it = creators_.find(object_type);
  if (it == creators_.end() || !it->second) {
    return nullptr;
  }
  return it->second(topic_id, object_type, display_name);
}

}  // namespace PJ
