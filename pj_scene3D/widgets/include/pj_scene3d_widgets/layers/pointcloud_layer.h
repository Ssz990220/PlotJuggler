#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QFutureWatcher>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/compressed_point_cloud.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

class QWidget;

namespace pj::scene3d {

// First concrete Scene3DLayer: a single sensor_msgs/PointCloud2 topic.
// Owns its own PointcloudRenderPass and absorbs the per-topic state that
// used to live in Scene3DDockWidget::PointCloudTopicState.
//
// Dual-mode: the topic may carry raw PointCloud objects (converted and pushed
// synchronously) or CompressedPointCloud blobs (Draco / Cloudini), which are
// decoded on the Qt thread pool and pushed when the result lands. The mode is
// derived per-sample from the parsed object, never latched.
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
  // dock to populate its row). object_type is what info() reports (the layer
  // renders kPointCloud and kCompressedPointCloud topics identically).
  PointCloudLayer(
      PJ::ObjectTopicId topic_id, QString display_name, PJ::sdk::BuiltinObjectType object_type,
      QObject* parent = nullptr);
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
  // Identity of an ObjectStore sample, computable WITHOUT parsing the payload:
  // (store timestamp, stored payload byte size). The size disambiguates a
  // same-timestamp payload swap (the store allows duplicate stamps); a swap that
  // also keeps the byte count is the accepted blind spot. The default
  // {INT64_MIN, 0} is the "none" sentinel — no real sample ever equals it.
  struct SampleId {
    int64_t stamp = std::numeric_limits<int64_t>::min();
    std::size_t size = 0;
    bool operator==(const SampleId&) const = default;
  };
  struct DecodeResult {
    SampleId id;
    std::shared_ptr<PJ::sdk::PointCloud> cloud;  // null on decode failure
    QString error;
  };
  // The newest sample requested while a decode ran (the depth-1 "latest wins" queue).
  struct PendingDecode {
    PJ::sdk::CompressedPointCloud cloud;  // its shared anchor keeps the blob alive
    SampleId id;
  };

  // Decode the first sample once at attach so we know available fields,
  // source frame, and time range before render is called.
  bool bootstrap();
  // Decode + push the cloud at time_ns into the render pass. No-op when
  // we don't have a parser or when the store has no sample at/before
  // time_ns. Skips all work when the pass already holds exactly that
  // sample with the current color field (the common tracker tick).
  void renderAt(int64_t time_ns);
  // Re-decode at the bootstrap time after a color-field or fixed-frame
  // change so the renderer reflects the new state without waiting for
  // the next tracker tick.
  void refreshNow();

  // Convert a canonical PointCloud (raw, or freshly decompressed) into the render
  // struct and push it to the pass. The single point where a cloud reaches the GPU,
  // shared by the raw and compressed paths. `id` identifies the pushed sample for
  // the redundant-push skip in renderAt().
  void pushCloud(const PJ::sdk::PointCloud& cloud, SampleId id);

  // Track a (possibly changing) source frame_id; notify the dock/panel on change.
  void updateSourceFrame(const std::string& frame_id);

  // --- Compressed-cloud async decode (Draco / Cloudini) ---
  // Compressed decode is CPU-heavy (~100ms for large Draco clouds), so it runs on the
  // Qt thread pool and never blocks the UI. requestDecode() records the request as
  // wanted_ and coalesces latest-wins; onDecodeFinished() runs on the GUI thread,
  // caches the result, and pushes it only if it still matches wanted_.
  // Raw PointCloud topics skip all of this and convert synchronously (cheap).
  void ensureDecodeWorker();
  void requestDecode(const PJ::sdk::CompressedPointCloud& cloud, SampleId id);
  void startDecode(const PJ::sdk::CompressedPointCloud& cloud, SampleId id);
  void onDecodeFinished();
  void populateColorFields(const PJ::sdk::PointCloud& cloud);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  // Parsers are deliberately NOT cached: every decode resolves a fresh
  // ParserBinding through ctx_.session (see parseLocked()), so a file reload
  // that re-registers the topic's parser slot can never leave us dangling.

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

  // What info() reports; kPointCloud and kCompressedPointCloud render identically.
  PJ::sdk::BuiltinObjectType object_type_ = PJ::sdk::BuiltinObjectType::kPointCloud;

  // Sample + color field currently held by the render pass; lets renderAt() skip
  // the per-tick parse/convert/upload when nothing changed.
  SampleId last_pushed_id_;
  std::string last_pushed_color_field_;

  // Async compressed-cloud decode state. Inert for raw PointCloud topics.
  QFutureWatcher<DecodeResult>* decode_watcher_ = nullptr;  // child of this; created on first compressed sample
  SampleId inflight_;                                       // the sample currently decoding; default = idle
  std::optional<PendingDecode> pending_;
  std::shared_ptr<PJ::sdk::PointCloud> decoded_cache_;  // last decoded cloud, reused on color-field change
  SampleId decoded_cache_id_;                           // identity of decoded_cache_
  SampleId wanted_;     // the sample the tracker currently wants; gates painting async results
  SampleId failed_id_;  // last sample whose decode failed; never re-requested (store bytes are immutable)

  PointcloudRenderPass cloud_pass_;
};

}  // namespace pj::scene3d
