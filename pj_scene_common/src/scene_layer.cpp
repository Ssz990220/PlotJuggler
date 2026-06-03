#include "pj_scene_common/scene_layer.h"

namespace PJ {

ISceneLayer::ISceneLayer(QObject* parent) : QObject(parent) {}

ISceneLayer::~ISceneLayer() = default;

void ISceneLayer::setFixedFrame(const QString& /*frame*/) {}

QDomElement ISceneLayer::xmlSaveState(QDomDocument& /*doc*/) const {
  return {};
}

bool ISceneLayer::xmlLoadState(const QDomElement& /*element*/) {
  return true;
}

}  // namespace PJ
