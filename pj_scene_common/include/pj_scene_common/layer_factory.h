#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene_common/scene_layer.h"

namespace PJ {

/// Per-scene-family registry that maps object types to layer constructors.
class LayerFactory {
 public:
  /// Creates an unattached layer for the requested object topic.
  using Creator = std::function<std::unique_ptr<ISceneLayer>(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name)>;

  /// Registers or replaces the creator for an object type.
  void registerType(sdk::BuiltinObjectType object_type, Creator creator);

  [[nodiscard]] bool supports(sdk::BuiltinObjectType object_type) const;

  /// Returns nullptr when the object type is unsupported or has no creator.
  [[nodiscard]] std::unique_ptr<ISceneLayer> create(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) const;

 private:
  std::unordered_map<sdk::BuiltinObjectType, Creator> creators_;
};

}  // namespace PJ
