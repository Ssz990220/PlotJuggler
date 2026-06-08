#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

namespace PJ {
class MessageParserPluginBase;
}  // namespace PJ

class QWidget;

namespace pj::scene3d {

// First concrete Scene3DLayer: a single sensor_msgs/PointCloud2 topic.
// Owns its own PointcloudRenderPass and absorbs the per-topic state that
// used to live in Scene3DDockWidget::PointCloudTopicState.
//
// Exposes a per-instance config widget containing the color-field combo.
// Adding new per-instance parameters (point size, alpha, colormap choice)
// is local to this class — Scene3DDockWidget and Scene3DConfigPanel do
// not need to change.
class PointCloudLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  // The topic id + initial display name are needed before attach() since
  // info() can be called immediately after construction (e.g. by the
  // dock to populate its row).
  PointCloudLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~PointCloudLayer() override;

  // Scene3DLayer
  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] QString sourceFrame() const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setFixedFrame(const QString& frame) override;
  void setTrackerTime(PJ::Timepoint time) override;
  void setVisible(bool visible) override;

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;
  // Source-frame extent of the decoded cloud, recomputed on each decode in
  // renderAt(). nullopt until the first cloud is decoded.
  [[nodiscard]] std::optional<AABB> worldBounds() const override {
    return world_bounds_;
  }

  QWidget* createConfigWidget(QWidget* parent) override;

  // PointCloud-specific accessors used by the config widget. Kept on the
  // concrete class so the abstract base doesn't carry pointcloud
  // concepts.
  [[nodiscard]] QStringList availableColorFields() const {
    return available_color_fields_;
  }
  [[nodiscard]] QString colorField() const {
    return QString::fromStdString(color_field_);
  }
  [[nodiscard]] PointcloudRenderPass::Shape shape() const {
    return shape_;
  }
  [[nodiscard]] float sizeMeters() const {
    return size_meters_;
  }
  [[nodiscard]] int sizePixels() const {
    return size_pixels_;
  }
  [[nodiscard]] PointcloudRenderPass::ColorType colorType() const {
    return color_type_;
  }
  [[nodiscard]] QColor solidColor() const {
    return solid_color_;
  }
  [[nodiscard]] PointcloudRenderPass::Colormap colormap() const {
    return colormap_;
  }
  [[nodiscard]] bool invertLut() const {
    return invert_lut_;
  }
  [[nodiscard]] bool autoRange() const {
    return auto_range_;
  }
  [[nodiscard]] float manualRangeMin() const {
    return manual_range_min_;
  }
  [[nodiscard]] float manualRangeMax() const {
    return manual_range_max_;
  }

  void setColorField(const QString& field);
  void setShape(PointcloudRenderPass::Shape shape);
  void setSizeMeters(float meters);
  void setSizePixels(int pixels);
  void setColorType(PointcloudRenderPass::ColorType type);
  void setSolidColor(QColor color);
  void setColormap(PointcloudRenderPass::Colormap cm);
  void setInvertLut(bool invert);
  void setAutoRange(bool enable);
  void setManualRange(float min_value, float max_value);

 signals:
  // Emitted whenever the color-field set or selection changes — the
  // config widget (if alive) listens and updates its combo without
  // having to be rebuilt.
  void colorFieldsChanged(const QStringList& fields);
  void currentColorFieldChanged(const QString& field);
  // Auto-range mode pushes the computed (min, max) back to the panel so
  // the spinboxes (when subsequently unhidden) start from real data
  // values rather than stale defaults.
  void autoRangeComputed(float min_value, float max_value);

 private:
  // Decode the first sample once at attach so we know available fields,
  // source frame, and time range before render is called.
  bool bootstrap();
  // Decode + push the cloud at time_ns into the render pass. No-op when
  // we don't have a parser or when the store has no sample at/before
  // time_ns.
  void renderAt(int64_t time_ns);
  // Re-decode at the bootstrap time after a color-field or fixed-frame
  // change so the renderer reflects the new state without waiting for
  // the next tracker tick.
  void refreshNow();

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  PJ::MessageParserPluginBase* parser_ = nullptr;
  // Per-topic mutex serialising parseObject across all consumers of this
  // topic's parser (SessionManager hands back a singleton). See parseLocked().
  std::shared_ptr<std::mutex> parser_mutex_;

  std::string color_field_;
  QStringList available_color_fields_;
  std::string source_frame_;
  QString fixed_frame_;
  // The latest tracker time pushed to this layer; the Timepoint the cached VBO
  // was (re)decoded at. Used by refreshNow() to re-decode at the current playhead.
  PJ::Timepoint decoded_at_ns_{};

  bool visible_ = true;
  bool range_dirty_ = true;
  int64_t ts_first_ = 0;
  int64_t ts_last_ = 0;

  // Source-frame bounds of the most recently decoded cloud (see worldBounds()).
  std::optional<AABB> world_bounds_;

  // User-tunable per-cloud parameters. Mirror the render pass's current
  // values; the panel reads them on rebuild so a re-opened config widget
  // always reflects what the user picked.
  PointcloudRenderPass::Shape shape_ = PointcloudRenderPass::Shape::kSphere;
  float size_meters_ = 0.01f;
  int size_pixels_ = 2;
  PointcloudRenderPass::ColorType color_type_ = PointcloudRenderPass::ColorType::kField;
  QColor solid_color_{255, 255, 255};
  PointcloudRenderPass::Colormap colormap_ = PointcloudRenderPass::Colormap::kTurbo;
  bool invert_lut_ = false;
  bool auto_range_ = true;
  float manual_range_min_ = 0.0f;
  float manual_range_max_ = 1.0f;

  PointcloudRenderPass cloud_pass_;
};

}  // namespace pj::scene3d
