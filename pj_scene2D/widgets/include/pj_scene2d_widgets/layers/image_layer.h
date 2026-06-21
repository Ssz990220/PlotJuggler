#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomElement>
#include <QWidget>
#include <memory>

#include "pj_scene2d_widgets/layers/scene2d_layer.h"

namespace PJ {

class MediaSource;
class ImagePipelineSource;

// Scene2DLayer for a single Image topic: contributes an image MediaSource to the
// composite. Exposes one display control — the magnification filter (smooth vs
// pixelated) — used when the image is zoomed past 1:1 (e.g. for pixel inspection).
class ImageLayer final : public Scene2DLayer {
  Q_OBJECT
 public:
  ImageLayer(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name,
      QObject* parent = nullptr);

  QWidget* createConfigWidget(QWidget* parent) override;

 protected:
  [[nodiscard]] std::unique_ptr<MediaSource> createMediaSource(const SceneLayerContext& ctx) override;
  void onBeforeDetach() override;
  void saveOptions(QDomElement& element) const override;
  bool loadOptions(const QDomElement& element) override;

 private:
  void setMagnifyNearest(bool nearest);
  void applyOptions();

  bool magnify_nearest_ = false;                 ///< false = linear (default), true = nearest/pixelated
  ImagePipelineSource* image_source_ = nullptr;  ///< borrowed; valid between attach and detach
};

}  // namespace PJ
