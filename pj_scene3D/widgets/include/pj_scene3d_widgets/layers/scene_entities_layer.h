#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QString>
#include <QStringList>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "pj_scene3d_widgets/passes/marker_render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

class QWidget;

namespace pj::scene3d {

// Concrete Scene3DLayer for a single visualization_msgs/MarkerArray-equivalent
// topic (canonical sdk::SceneEntities, type kSceneEntities). Owns a
// MarkerRenderPass and decodes the latest batch at/before the tracker time from
// the ObjectStore. Mirrors PointCloudEntity's "decode-from-store-at-tracker-time"
// lifecycle; substitutes the marker render pass and SceneEntities object type.
//
// Exposes a per-instance config widget with the viewer-side display overrides
// (opacity / color-override / wireframe). These are a per-topic preference, NOT
// marker data — the marker protocol has no such fields.
class SceneEntitiesLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  SceneEntitiesLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~SceneEntitiesLayer() override;

  // Scene3DLayer / ISceneLayer
  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] QString sourceFrame() const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setTrackerTime(PJ::Timepoint time) override;
  void setVisible(bool visible) override;

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;

  QWidget* createConfigWidget(QWidget* parent) override;

  // Per-instance display-override accessors used by the config widget. Kept on
  // the concrete class so the abstract base doesn't carry marker concepts.
  [[nodiscard]] float opacity() const {
    return overrides_.opacity;
  }
  [[nodiscard]] bool colorOverrideEnabled() const {
    return overrides_.color_override;
  }
  [[nodiscard]] QColor overrideColor() const {
    return QColor::fromRgbF(overrides_.override_color.r, overrides_.override_color.g, overrides_.override_color.b);
  }
  [[nodiscard]] bool wireframe() const {
    return overrides_.wireframe;
  }

  void setOpacity(float opacity);
  void setColorOverrideEnabled(bool enabled);
  void setOverrideColor(QColor color);
  void setWireframe(bool enabled);

 private:
  // Decode the first sample once at attach so we know the source frame and time
  // range before render is called.
  bool bootstrap();
  // Decode + push the batch at/before time_ns into the render pass. No-op when
  // there's no parser or the store has no sample at/before time_ns.
  void renderAt(int64_t time_ns);
  // Re-decode at the current playhead after a visibility change.
  void refreshNow();
  // Push the current overrides to the pass and request a repaint.
  void applyOverrides();

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  // Parsers are deliberately NOT cached: every decode resolves a fresh
  // ParserBinding through ctx_.session (see parseLocked()), so a file reload
  // that re-registers the topic's parser slot can never leave us dangling.

  std::string source_frame_;
  // The latest tracker time pushed to this entity; the time the cached batch was
  // (re)decoded at. Used by refreshNow() to re-decode at the current playhead.
  PJ::Timepoint decoded_at_ns_{};

  bool visible_ = true;
  int64_t ts_first_ = 0;

  // Viewer-side display overrides pushed wholesale to the pass. The override
  // color lives here as a normalized vec4; the config widget derives a QColor
  // from it on demand (overrideColor()) — single source of truth.
  MarkerRenderPass::DisplayOverrides overrides_;

  MarkerRenderPass pass_;
};

}  // namespace pj::scene3d
