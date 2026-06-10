#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomElement>
#include <QWidget>
#include <memory>

#include "pj_scene2d_core/depth_pipeline_source.h"
#include "pj_scene2d_widgets/layers/scene2d_layer.h"

namespace PJ {

class MediaSource;

// Scene2DLayer for a DepthImage topic: contributes a depth-pipeline MediaSource
// with per-layer colormap, near/far range (auto or manual), and opacity config.
class DepthImageLayer final : public Scene2DLayer {
  Q_OBJECT
 public:
  DepthImageLayer(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name,
      QObject* parent = nullptr);

  QWidget* createConfigWidget(QWidget* parent) override;

 protected:
  [[nodiscard]] std::unique_ptr<MediaSource> createMediaSource(const SceneLayerContext& ctx) override;
  void onBeforeDetach() override;
  void saveOptions(QDomElement& element) const override;
  bool loadOptions(const QDomElement& element) override;

 private:
  void applyTo(DepthPipelineSource& source) const;
  void applyOptions();
  void setColormap(DepthColormap colormap);
  void setAutoRange(bool enabled);
  void setRange(float near_m, float far_m);
  void setOpacity(float opacity);

  DepthColormap colormap_ = DepthColormap::kTurbo;
  float near_m_ = 0.0f;
  float far_m_ = 10.0f;
  bool auto_range_ = true;
  float opacity_ = 1.0f;
  DepthPipelineSource* depth_source_ = nullptr;
};

}  // namespace PJ
