// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene_common/layer_params.h"

#include <QDomDocument>
#include <QDomElement>

#include "pj_scene_common/scene_layer.h"

namespace PJ {

QString serializeLayerParams(const ISceneLayer& layer) {
  QDomDocument doc;
  const QDomElement element = layer.xmlSaveState(doc);
  if (element.isNull()) {
    return {};
  }
  doc.appendChild(element);
  return doc.toString();
}

bool applyLayerParams(ISceneLayer& layer, const QString& xml) {
  if (xml.isEmpty()) {
    return false;
  }
  QDomDocument doc;
  if (!doc.setContent(xml)) {
    return false;
  }
  const QDomElement element = doc.documentElement();
  if (element.isNull()) {
    return false;
  }
  return layer.xmlLoadState(element);
}

}  // namespace PJ
